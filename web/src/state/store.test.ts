import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ClientHandlers } from '../net/client';
import type { StateMessage, ServerMessage } from '../net/protocol';

// `first` is the module's own controller, the one the persistence functions drive.
const mock = vi.hoisted(() => ({ send: vi.fn(), handlers: null as ClientHandlers | null, first: null as ClientHandlers | null }));
vi.mock('../audio/player', () => ({ AudioPlayer: class { reset() {} } }));
vi.mock('./theme', () => ({ applyTheme() {} }));
vi.mock('../net/client', () => ({
  defaultWebSocketUrl: () => 'ws://test',
  SdrClient: class {
    constructor(_url: string, handlers: ClientHandlers) { mock.handlers = handlers; mock.first ??= handlers; }
    send = mock.send;
    setUrl() {}
    connect() {}
    close() {}
  },
}));

import { RadioController, bands, decoders, dsp, tuning, viewport, BANDWIDTH_PROFILES, bandwidthProfile, waterfallRowWidth } from './store';
import { applySharedTuning, loadPreferences, readUrlTuning, shareUrl } from './persist';

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('window', {
    requestAnimationFrame: (callback: () => void) => setTimeout(callback, 16),
    cancelAnimationFrame: clearTimeout,
    setTimeout,
  });
  mock.send.mockClear();
  bandwidthProfile.value = BANDWIDTH_PROFILES[1];
  bands.value = [
    { id: '20m', center: 14_200_000, low: 14_000_000, high: 14_350_000 },
    { id: '40m', center: 7_100_000, low: 7_000_000, high: 7_300_000 },
  ] as typeof bands.value;
  tuning.value = { band: '20m', freq: 14_200_000, mode: 'usb', low: 300, high: 2700, cwPitch: 700 };
  viewport.value = { lowHz: 14_000_000, highHz: 14_350_000, width: 1024, fps: 12 };
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

function state(ack: StateMessage['ack']): StateMessage {
  return { type: 'state', ...tuning.value, cw_pitch: 700, agc: 'slow', gain: 0, nr: 0,
    autonotch: false, volume: 1, squelch: -200, audio_enabled: true, audio_bitrate: 48000,
    notches: [], viewport: { enabled: true, low: 14_000_000, high: 14_350_000, width: 1024, fps: 12 }, ack };
}

describe('live controls', () => {
  it('sends one DSP message per frame while a slider is dragged, with the latest values', () => {
    const radio = new RadioController();
    mock.send.mockClear();
    for (let i = 1; i <= 10; i++) radio.setDsp({ nr: i / 100 });
    expect(dsp.value.nr).toBe(0.1);
    const sent = () => mock.send.mock.calls.filter(([message]) => message.type === 'dsp');
    expect(sent()).toHaveLength(0);
    vi.advanceTimersByTime(16);
    expect(sent()).toHaveLength(1);
    expect(sent()[0][0]).toMatchObject({ type: 'dsp', nr: 0.1 });
    radio.setDsp({ autonotch: true });
    vi.advanceTimersByTime(16);
    expect(sent()).toHaveLength(2);
    expect(sent()[1][0]).toMatchObject({ nr: 0.1, autonotch: true });
  });
  it('negotiates waterfall precision and returns to fine steps on an older server', () => {
    const radio = new RadioController();
    const welcome = (capabilities: string[]) => {
      mock.handlers!.onMessage({ type: 'welcome', capabilities, site: {}, bands: bands.value } as ServerMessage);
      vi.advanceTimersByTime(16);
    };
    welcome(['wfc3', 'wfc4']);
    expect(mock.send).toHaveBeenCalledWith(expect.objectContaining({ type: 'viewport', codec: 'wfc4', step_db: 2 }));
    radio.setBandwidthProfile(BANDWIDTH_PROFILES[2]);
    vi.advanceTimersByTime(16);
    expect(mock.send).toHaveBeenLastCalledWith(expect.objectContaining({ type: 'viewport', codec: 'wfc4', step_db: 1 }));
    radio.setBandwidthProfile(BANDWIDTH_PROFILES[1]);
    welcome(['wfc3']);
    expect(mock.send).toHaveBeenLastCalledWith(expect.objectContaining({ type: 'viewport', codec: 'wfc3', step_db: 1 }));
  });
  it('keeps the signal and filter in place through rapid CW mode changes', () => {
    const radio = new RadioController();
    tuning.value = { ...tuning.value, cwPitch: 900 };
    const signal = tuning.value.freq;
    const view = { ...viewport.value };
    for (const [mode, offset] of [['cw', 900], ['cwl', -900], ['usb', 0]] as const) {
      radio.setMode(mode);
      expect(tuning.value.freq + offset).toBe(signal);
      if (offset) expect((tuning.value.low + tuning.value.high) / 2).toBe(offset);
      radio.tune(signal + 10);
      vi.advanceTimersByTime(16);
      expect(mock.send).toHaveBeenLastCalledWith(expect.objectContaining({ mode, cw_pitch: 900,
        freq: signal + 10 - offset, low: tuning.value.low, high: tuning.value.high }));
      radio.tune(signal);
    }
    expect(viewport.value).toEqual(view);
  });
  it('opens on a band on the air when the one it had is off the air by its hours', () => {
    new RadioController();
    const band = (id: string, center: number, extra: object) => ({ id, name: id, center, low: center - 1e6, high: center + 1e6,
      sample_rate: 2.048e6, max_bandwidth: 20000, listeners: 0, running: true, ...extra });
    const listed = [
      band('20m', 14.1e6, { on_air: false, next_change: 1, shared_input: '20m' }),
      band('30m', 10.1e6, { on_air: true }),
      band('40m', 7.1e6, { on_air: true, shared_input: '20m' }),
    ];
    // Remembered 20 m: the band on its input takes over.
    tuning.value = { ...tuning.value, band: '20m', freq: 14.2e6 };
    mock.handlers!.onMessage({ type: 'welcome', capabilities: [], site: {}, bands: listed } as unknown as ServerMessage);
    expect(tuning.value.band).toBe('40m');
    // Nothing remembered: the first on the air, not the first listed.
    tuning.value = { ...tuning.value, band: '' };
    mock.handlers!.onMessage({ type: 'welcome', capabilities: [], site: {}, bands: listed } as unknown as ServerMessage);
    expect(tuning.value.band).toBe('30m');
  });
  it('keeps the site listener total separate from per-band counts', () => {
    new RadioController();
    mock.handlers!.onMessage({ type: 'band-status', bands: [
      { id: '20m', listeners: 2, running: true }, { id: '40m', listeners: 3, running: false },
    ] });
    mock.handlers!.onMessage({ type: 'meter', listeners: 5 } as ServerMessage);
    expect(bands.value.map(band => band.listeners)).toEqual([2, 3]);
    expect(bands.value[1].running).toBe(false);
  });
  it('shares the CW signal, pitch and viewport without offsetting the carrier twice', () => {
    vi.stubGlobal('location', { origin: 'https://receiver.test', pathname: '/', hash: '' });
    tuning.value = { ...tuning.value, mode: 'cw', freq: 14_200_000, cwPitch: 900 };
    const url = new URL(shareUrl());
    vi.stubGlobal('location', url);
    const shared = readUrlTuning();
    expect(shared).toMatchObject({ freq: 14_200_900, cwPitch: 900, mode: 'cw',
      viewLow: viewport.value.lowHz, viewHigh: viewport.value.highHz });
    tuning.value = { ...tuning.value, mode: 'usb', cwPitch: 700 };
    new RadioController().tune(shared.freq!, { mode: shared.mode, cwPitch: shared.cwPitch });
    expect(tuning.value.freq).toBe(14_200_000);
  });
  it('takes a Slow or Steady stored before v2 for Auto, and a choice stored since as it is', () => {
    vi.stubGlobal('location', { origin: 'https://receiver.test', pathname: '/', hash: '' });
    const load = (entries: Record<string, unknown>) => {
      vi.stubGlobal('localStorage', { getItem: (key: string) => key in entries ? JSON.stringify(entries[key]) : null });
      dsp.value = { ...dsp.value, agc: 'medium' };
      loadPreferences();
      return dsp.value.agc;
    };
    expect(load({ 'fernsdr.preferences.v1': { agc: 'steady' } })).toBe('auto');
    expect(load({ 'fernsdr.preferences.v1': { agc: 'slow' } })).toBe('auto');
    expect(load({ 'fernsdr.preferences.v1': { agc: 'fast' } })).toBe('fast');
    expect(load({ 'fernsdr.preferences.v2': { agc: 'slow' }, 'fernsdr.preferences.v1': { agc: 'fast' } })).toBe('slow');
  });
  it('restores each band frequency, filter and zoom when switching back', () => {
    const radio = new RadioController();
    radio.tune(14_215_000);
    radio.setPassband(200, 2300);
    radio.setViewport(14_200_000, 14_230_000);
    radio.selectBand('40m');
    radio.tune(7_123_000);
    radio.setPassband(-2700, -300);
    radio.setViewport(7_110_000, 7_130_000);
    radio.selectBand('20m');
    expect(tuning.value).toMatchObject({ band: '20m', freq: 14_215_000, low: 200, high: 2300 });
    expect(viewport.value).toMatchObject({ lowHz: 14_200_000, highHz: 14_230_000 });
    radio.selectBand('40m');
    expect(tuning.value).toMatchObject({ band: '40m', freq: 7_123_000, low: -2700, high: -300 });
    expect(viewport.value).toMatchObject({ lowHz: 7_110_000, highHz: 7_130_000 });
  });

  it('reselecting the active band does not reset a zoomed view', () => {
    const radio = new RadioController();
    viewport.value = { ...viewport.value, lowHz: 14_200_000, highHz: 14_220_000 };
    radio.selectBand('20m');
    expect(viewport.value.lowHz).toBe(14_200_000);
    expect(mock.send).not.toHaveBeenCalled();
  });

  it('keeps the current receiver when two bands cover a linked frequency', () => {
    const radio = new RadioController();
    bands.value = [{ ...bands.value[0], id: 'wide', low: 0, high: 30_000_000 }, ...bands.value];
    radio.goTo(14_210_000);
    expect(tuning.value.band).toBe('20m');
  });

  it('negotiates compact meters only when the server advertises them', () => {
    new RadioController();
    mock.handlers!.onMessage({ type: 'welcome', site: { name: 'test' }, bands: bands.value,
      capabilities: ['meter-v1'] } as ServerMessage);
    expect(mock.send).toHaveBeenCalledWith({ type: 'hello', capabilities: ['meter-v1'] });
  });
  it('negotiates codecs per connection and forgets support from a previous receiver', () => {
    new RadioController();
    const welcome = { type: 'welcome', site: { name: 'test' }, bands: bands.value };
    mock.handlers!.onMessage({ ...welcome, capabilities: ['nac2', 'meter-v1', 'wfc3', 'audio-discontinuity', 'unknown'] } as ServerMessage);
    vi.advanceTimersByTime(16);
    expect(mock.send).toHaveBeenCalledWith({ type: 'hello', capabilities: ['meter-v1', 'nac2', 'audio-discontinuity'] });
    expect(mock.send).toHaveBeenCalledWith(expect.objectContaining({ type: 'viewport', codec: 'wfc3' }));
    mock.send.mockClear();
    mock.handlers!.onMessage(welcome as ServerMessage);
    vi.advanceTimersByTime(16);
    expect(mock.send).not.toHaveBeenCalledWith(expect.objectContaining({ type: 'hello' }));
    expect(mock.send).toHaveBeenCalledWith(expect.objectContaining({ type: 'viewport', codec: 'wfc2' }));
  });
  it('learns the public decoders from the welcome and from station changes', () => {
    new RadioController();
    const ft8 = { id: 'ft8', channels: [{ id: '20m-ft8-14074', band: '20m', mode: 'ft8', dial: 14_074_000, low: 0, high: 4000 }] };
    mock.handlers!.onMessage({ type: 'welcome', site: { name: 'test' }, bands: bands.value, decoders: [ft8] } as ServerMessage);
    expect(decoders.value).toEqual([ft8]);
    mock.handlers!.onMessage({ type: 'station', site: { name: 'test' } } as ServerMessage);
    expect(decoders.value).toEqual([ft8]);
    mock.handlers!.onMessage({ type: 'station', site: { name: 'test' }, decoders: [] } as unknown as ServerMessage);
    expect(decoders.value).toEqual([]);
    mock.handlers!.onMessage({ type: 'welcome', site: { name: 'test' }, bands: bands.value } as ServerMessage);
    expect(decoders.value).toEqual([]);
  });
  it('selects an available band when a remembered band was removed', () => {
    new RadioController();
    tuning.value = { ...tuning.value, band: 'removed' };
    mock.handlers!.onMessage({ type: 'welcome', site: { name: 'test' }, bands: bands.value } as ServerMessage);
    expect(tuning.value.band).toBe('20m');
    expect(mock.send).toHaveBeenCalledWith(expect.objectContaining({ type: 'tune', band: '20m' }));
  });

  it('opens a shared link before the welcome falls back to a band and replays the settings', () => {
    const receiver = bands.value;
    bands.value = [];
    const stop = applySharedTuning({ band: '40m', freq: 7_150_000, mode: 'lsb', viewLow: 7_140_000, viewHigh: 7_160_000 });
    const welcome = { type: 'welcome', site: { name: 'test' }, bands: receiver } as ServerMessage;
    mock.first!.onMessage(welcome);
    vi.advanceTimersByTime(16);
    const sent = mock.send.mock.calls.map(([message]) => message);
    expect(sent.map((message) => message.type)).toEqual(['tune', 'tune', 'dsp', 'audio', 'viewport']);
    expect(sent[0]).toMatchObject({ band: '40m', freq: 7_100_000 });
    expect(sent[1]).toMatchObject({ band: '40m', freq: 7_150_000, mode: 'lsb' });
    expect(sent[4]).toMatchObject({ low: 7_140_000, high: 7_160_000 });

    // A reconnect replays where the listener is now; the link is not applied again.
    mock.send.mockClear();
    mock.first!.onMessage(welcome);
    vi.advanceTimersByTime(16);
    expect(mock.send.mock.calls.map(([message]) => message.type)).toEqual(['tune', 'dsp', 'audio', 'viewport']);
    stop();
  });

  it('updates the view immediately and sends the latest position in the next frame', () => {
    const radio = new RadioController();
    for (let i = 0; i < 100; i++) radio.setViewport(14_100_000 + i, 14_150_000 + i);
    expect(viewport.value.lowHz).toBe(14_100_099);
    expect(mock.send).not.toHaveBeenCalled();
    vi.advanceTimersByTime(16);
    expect(mock.send).toHaveBeenCalledTimes(1);
    expect(mock.send).toHaveBeenCalledWith(expect.objectContaining({ type: 'viewport', low: 14_100_099 }));
  });

  it('ignores old tuning and viewport replies without losing a newer mode response', () => {
    const radio = new RadioController();
    radio.tune(14_210_000);
    radio.setViewport(14_190_000, 14_220_000);
    mock.handlers!.onMessage({ ...state({ tune: 0, viewport: 0, dsp: 0 }), freq: 14_200_000 });
    expect(tuning.value.freq).toBe(14_210_000);
    expect(viewport.value.lowHz).toBe(14_190_000);
    mock.handlers!.onMessage({ ...state({ tune: 1, viewport: 0, dsp: 0 }), mode: 'am', low: -4500, high: 4500 });
    expect(tuning.value.mode).toBe('am');
    expect(tuning.value.low).toBe(-4500);
    expect(viewport.value.lowHz).toBe(14_190_000);
  });

  it('does not let a queued drag undo a mode or band selection', () => {
    const radio = new RadioController();
    radio.tune(14_210_000);
    radio.setMode('am');
    vi.advanceTimersByTime(16);
    expect(mock.send).toHaveBeenCalledTimes(1);
    expect(mock.send).toHaveBeenLastCalledWith(expect.objectContaining({ mode: 'am' }));
    radio.tune(14_220_000);
    radio.selectBand('40m');
    vi.advanceTimersByTime(16);
    expect(mock.send.mock.calls.filter(([message]) => message.type === 'tune').at(-1)?.[0].band).toBe('40m');
  });

  it('moves the view to the passband once tuning takes it off screen, and not before', () => {
    const radio = new RadioController();
    radio.setViewport(14_190_000, 14_220_000);
    // Inside, even right at the edge: the view stays.
    radio.tune(14_217_000);
    expect(viewport.value).toMatchObject({ lowHz: 14_190_000, highHz: 14_220_000 });
    // The upper edge of the USB passband crosses the view's: it centres on it.
    radio.tune(14_217_500);
    expect(viewport.value.lowHz).toBe(14_217_500 + 1500 - 15_000);
    expect(viewport.value.highHz).toBe(14_217_500 + 1500 + 15_000);
    // A drag on the waterfall does not move the view under the pointer.
    radio.setViewport(14_190_000, 14_220_000);
    radio.tune(14_250_000, {}, false);
    expect(viewport.value).toMatchObject({ lowHz: 14_190_000, highHz: 14_220_000 });
    // At the edge of the band the view stops there, with the passband in it.
    radio.tune(14_349_000);
    expect(viewport.value.highHz).toBe(14_350_000);
    expect(viewport.value.highHz - viewport.value.lowHz).toBe(30_000);
  });

  it('sends a cross-band tune before the new viewport', () => {
    const radio = new RadioController();
    radio.goTo(7_100_000);
    vi.advanceTimersByTime(16);
    expect(mock.send.mock.calls.map(([message]) => message.type)).toEqual(['tune', 'viewport']);
    radio.disconnect();
    vi.advanceTimersByTime(50);
    expect(mock.send).toHaveBeenCalledTimes(2);
  });
});

describe('waterfall palette', () => {
  it('wears the operator palette until the listener picks one', async () => {
    const { adoptOperatorPalette, display } = await import('./store');
    display.value = { ...display.value, palette: 'classic', paletteChosen: false };
    adoptOperatorPalette({ palette: 'aurora' });
    expect(display.value.palette).toBe('aurora');
    adoptOperatorPalette({ palette: 'not-a-palette' });
    expect(display.value.palette).toBe('classic');
    adoptOperatorPalette(null);
    expect(display.value.palette).toBe('classic');
    display.value = { ...display.value, palette: 'ember', paletteChosen: true };
    adoptOperatorPalette({ palette: 'mono' });
    expect(display.value.palette).toBe('ember');
  });
});

describe('waterfall row width', () => {
  it('rounds down to a sixteenth of an octave, so near sizes share rows', () => {
    expect(waterfallRowWidth(1096)).toBe(1088);
    expect(waterfallRowWidth(1100)).toBe(1088);
    expect(waterfallRowWidth(1024)).toBe(1024);
    expect(waterfallRowWidth(2048)).toBe(2048);
    expect(waterfallRowWidth(780)).toBe(768);
    expect(waterfallRowWidth(1536)).toBe(1536);
    expect(waterfallRowWidth(40)).toBe(64);
    for (let pixels = 64; pixels <= 4096; pixels++) {
      const width = waterfallRowWidth(pixels);
      expect(width).toBeLessThanOrEqual(pixels);
      expect(pixels / width).toBeLessThan(1.0667);
    }
  });
});
