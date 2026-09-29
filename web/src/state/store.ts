/**
 * Application state and the controller that owns the connection.
 *
 * Reactive boxes (reactive.svelte.ts) rather than a reducer: almost every
 * piece of this UI is a small independent readout that updates at its own
 * rate (meters at 10 Hz, the waterfall at 60), and fine-grained reactivity
 * keeps a mobile browser from re-rendering the whole tree for an S-meter
 * tick.
 */
import { box, computed } from './reactive.svelte';
import { AudioPlayer } from '../audio/player';
import { carrierForSignal, signalForCarrier } from '../util/cw';
import { defaultPassband, constrainPassband } from '../util/passband';
import { constrainViewport } from '../util/viewport';
import { offAir } from '../util/band-hours';
import { applyTheme as applyOperatorTheme, type Theme } from './theme';
import { SdrClient, defaultWebSocketUrl, type ConnectionState } from '../net/client';
import { BandMemories } from './band-memory';
import { planFor } from './bandplan';
import type {
  BandDescription,
  DecoderDescription,
  MeterMessage,
  RdsMessage,
  ServerBinary,
  ServerMessage,
  SiteDescription,
  StateMessage,
  WaterfallPacket,
} from '../net/protocol';

export interface Tuning {
  band: string;
  freq: number;
  mode: string;
  low: number;
  high: number;
  cwPitch: number;
}

export interface DspSettings {
  agc: string;
  gain: number;
  nr: number;
  autonotch: boolean;
  notches: { hz: number; width: number }[];
  squelch: number;
  /** Mute by the shape of the passband rather than its level. */
  autoSquelch: boolean;
  /** Audio below this many hertz is cut after demodulation; 0 is off. */
  highpass: number;
  /** NFM de-emphasis in microseconds; 0 is flat, for a decoder. */
  deemphasis: number;
  /**
   * Broadcast FM de-emphasis in microseconds, or null to follow the station's
   * region: 75 in the Americas, 50 elsewhere (see wfmDeemphasis()).
   */
  wfmDeemphasis: number | null;
  /** On NFM, notch out the station's CTCSS tone once it is measured. */
  ctcssFilter: boolean;
  /**
   * Tone squelch on NFM: muted unless this CTCSS tone, in Hz, is received;
   * 0 for off. Not remembered between visits: one forgotten would keep every
   * other channel silent without saying why.
   */
  ctcssSquelch: number;
}

export interface Viewport {
  lowHz: number;
  highHz: number;
  width: number;
  fps: number;
}

/**
 * Named bandwidth profiles. Made explicit and visible because the receivers
 * this runs on are often on a link measured in tens of megabits total, and a
 * user who knows they are on a phone plan should be able to say so.
 */
export interface BandwidthProfile {
  id: 'low' | 'balanced' | 'high';
  label: string;
  description: string;
  /** The audio ceiling. NAC3 uses less whenever the signal allows. */
  audioBitrate: number;
  /**
   * NAC3 frames per packet. Each extra frame adds one frame of delay (about
   * 10 ms) and saves a packet header and most of a frame's side information.
   */
  audioFrames: number;
  /** How far below the receiver's own noise the codec's noise sits, dB. */
  noiseMargin: number;
  waterfallFps: number;
  maxWaterfallWidth: number;
  waterfallStepDb: 1 | 2;
}

export const BANDWIDTH_PROFILES: BandwidthProfile[] = [
  {
    id: 'low',
    label: 'Low',
    description: 'Smallest stream: audio in packets of four frames, about 30 ms more delay; fewer waterfall columns in steps of up to 2 dB.',
    audioBitrate: 32000,
    audioFrames: 4,
    noiseMargin: 10,
    waterfallFps: 8,
    maxWaterfallWidth: 768,
    waterfallStepDb: 2,
  },
  {
    id: 'balanced',
    label: 'Balanced',
    description: 'Audio in packets of two frames, about 10 ms more delay; waterfall shading in steps of up to 2 dB.',
    audioBitrate: 48000,
    audioFrames: 2,
    noiseMargin: 12,
    waterfallFps: 12,
    maxWaterfallWidth: 1536,
    waterfallStepDb: 2,
  },
  {
    id: 'high',
    label: 'High',
    description: 'Lowest delay and the cleanest audio: one frame per packet, codec noise far under the receiver\'s own; finer 1 dB waterfall.',
    audioBitrate: 64000,
    audioFrames: 1,
    noiseMargin: 18,
    waterfallFps: 20,
    maxWaterfallWidth: 2560,
    waterfallStepDb: 1,
  },
];

/** The audio command for a profile. */
function audioCommand(profile: BandwidthProfile): Record<string, unknown> {
  return {
    type: 'audio',
    enabled: true,
    bitrate: profile.audioBitrate,
    frames: profile.audioFrames,
    noise_margin: profile.noiseMargin,
  };
}

export type Palette = 'aurora' | 'classic' | 'mono' | 'ember';
export type ThemeChoice = 'auto' | 'dark' | 'light';

export interface DisplaySettings {
  palette: Palette;
  /**
   * Whether the listener picked the palette themselves. Until they do, the
   * waterfall wears the operator's choice from the theme and follows it when
   * the operator changes it; after, the listener's choice stays.
   */
  paletteChosen: boolean;
  /** Level mapped to the bottom of the colour scale, dBFS. */
  floorDb: number;
  /** Level mapped to the top. */
  ceilingDb: number;
  /** Automatically follow the noise floor. */
  autoLevels: boolean;
  showBandPlan: boolean;
  showSpectrum: boolean;
  /**
   * How much of the receive panel shows: the essentials a visitor needs, or
   * also the sound shaping and fine filter controls a radio operator reaches
   * for (tone, low cut, de-emphasis, filter edges). The tools under the dial
   * are there either way. The listener's own choice, kept in this browser.
   */
  controls: 'essential' | 'full';
  spectrumHeight: number;
}

// --- state -------------------------------------------------------------

export const connectionState = box<ConnectionState>('connecting');
/**
 * The listener timeout: when the receiver will let this listener's place go
 * (a performance.now() time) while it waits for an answer, and why it did
 * once it has.
 */
export const inactivityDeadline = box<number | null>(null);
export const inactiveReason = box<string | null>(null);
export const connectionDetail = box<string>('');
export const site = box<SiteDescription | null>(null);
export const bands = box<BandDescription[]>([]);
export const meter = box<MeterMessage | null>(null);
/** Broadcast FM's RDS for the station tuned, or null for none. */
export const rds = box<RdsMessage | null>(null);
export const notice = box<string>('');
/** Decoders whose decodes listeners may read, from the welcome. */
export const decoders = box<DecoderDescription[]>([]);
export const errorMessage = box<string>('');

export const tuning = box<Tuning>({
  band: '',
  freq: 0,
  mode: 'usb',
  low: 300,
  high: 2700,
  cwPitch: 700,
});

export const dsp = box<DspSettings>({
  agc: 'auto',
  gain: 0,
  nr: 0,
  autonotch: false,
  notches: [],
  squelch: -200,
  autoSquelch: false,
  highpass: 0,
  deemphasis: 300,
  wfmDeemphasis: null,
  ctcssFilter: true,
  ctcssSquelch: 0,
});

/**
 * The broadcast FM de-emphasis in force: the listener's choice, or the one
 * stations use where this receiver is. ITU Region 2 (the Americas) uses 75
 * microseconds, as does South Korea, which the region cannot tell; everyone
 * else 50.
 */
export function wfmDeemphasis(settings: DspSettings = dsp.value): number {
  if (settings.wfmDeemphasis !== null) return settings.wfmDeemphasis;
  return planFor(site.value?.band_plan, site.value?.grid)?.region === 2 ? 75 : 50;
}

/**
 * The row width to ask for, for a canvas `pixels` wide: rounded down to one
 * of a few widths, a sixteenth of an octave apart (64 pixels between 1024
 * and 2048). The server codes each row once for everyone who asks for the
 * same view at the same width, so pages on screens of nearly the same size
 * share their rows; the canvas stretches a row by at most 6.25 %.
 */
export function waterfallRowWidth(pixels: number): number {
  const width = Math.max(64, Math.round(pixels));
  const step = 2 ** Math.max(2, Math.floor(Math.log2(width)) - 4);
  return Math.floor(width / step) * step;
}

export const viewport = box<Viewport>({ lowHz: 0, highHz: 0, width: 1024, fps: 12 });
export const bandwidthProfile = box<BandwidthProfile>(BANDWIDTH_PROFILES[1]);

export const display = box<DisplaySettings>({
  palette: 'classic',
  paletteChosen: false,
  floorDb: -115,
  ceilingDb: -35,
  autoLevels: true,
  showBandPlan: true,
  controls: 'essential',
  showSpectrum: true,
  spectrumHeight: 150,
});

export const theme = box<ThemeChoice>('auto');

const PALETTE_IDS: readonly Palette[] = ['aurora', 'classic', 'mono', 'ember'];

/** The operator's palette, for a listener who has not picked one. */
export function adoptOperatorPalette(operator: Theme | null): void {
  const settings = display.value;
  if (settings.paletteChosen) return;
  const palette = PALETTE_IDS.find((id) => id === operator?.palette) ?? 'classic';
  if (palette !== settings.palette) display.value = { ...settings, palette };
}

/**
 * Applies the theme to the document. `auto` follows the system and keeps
 * following it, so a user who changes their OS at sunset does not have to
 * come back here.
 */
export function applyTheme(choice: ThemeChoice): void {
  theme.value = choice;
  const root = document.documentElement;
  if (choice === 'auto') {
    const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    root.dataset.colorScheme = dark ? 'dark' : 'light';
    root.removeAttribute('data-theme-locked');
  } else {
    root.dataset.colorScheme = choice;
    root.setAttribute('data-theme-locked', 'true');
  }
}

export function watchSystemTheme(): () => void {
  const query = window.matchMedia('(prefers-color-scheme: dark)');
  const update = () => {
    if (theme.value === 'auto') applyTheme('auto');
  };
  query.addEventListener('change', update);
  return () => query.removeEventListener('change', update);
}

export const volume = box(0.8);
export const filterLimit = box(6000);
export const muted = box(false);
export const audioState = box<'idle' | 'starting' | 'running' | 'suspended' | 'failed'>('idle');

/**
 * What the operator is listening to, as opposed to what the receiver is tuned
 * to. They differ only in CW, where the signal sits one pitch away from the
 * carrier; everywhere else this is `tuning.freq` unchanged.
 */
export const signalFreq = computed(() =>
  signalForCarrier(tuning.value.freq, tuning.value.mode, tuning.value.cwPitch),
);
/** Why audio is not playing, shown in the gate when a press does not take. */
export const audioDiagnostics = box<string>('');
export const audioLatencyMs = box(0);
export const audioTargetMs = box(0);
export const audioUnderruns = box(0);
export const networkLatencyMs = box(0);
export const streamTraffic = box<import('../net/client').StreamTraffic | null>(null);

export interface ChatLine { id: number; name: string; text: string; at: number; }

/** The chat widget's messages, newest last. Capped: this is a chat, not a log. */
export const chatLog = box<ChatLine[]>([]);
export const chatRefusal = box('');

/** The operator's colours, background and widgets, as sent by the receiver. */
export const operatorTheme = box<Theme | null>(null);

export const currentBand = computed(() => bands.value.find((b) => b.id === tuning.value.band) ?? null);

/** Combined stream rate, for the status readout. */
export const totalBitrate = computed(() => {
  const m = meter.value;
  return m ? m.audio_bps + m.waterfall_bps : 0;
});

// --- controller --------------------------------------------------------

type WaterfallSink = (packet: WaterfallPacket) => void;

export class RadioController {
  readonly player = new AudioPlayer();
  readonly bandMemories = new BandMemories();
  private client: SdrClient;
  private waterfallSink: WaterfallSink | null = null;
  private audioGeneration = -1;
  private audioRate = 0;
  private sendTimer: number | null = null;
  private dspTimer: number | null = null;
  private viewportTimer: number | null = null;
  private pendingTune: Record<string, unknown> | null = null;
  private revision = { tune: 0, viewport: 0, dsp: 0 };
  private requestedWaterfallWidth = 1024;
  private waterfallCodec = 'wfc2';
  /**
   * Until the server has told us its state once, the passband in `tuning` is
   * only a placeholder. Sending it back would override the mode's own default
   * with a number the user never chose - which is how a fresh client ends up
   * on a filter width that matches none of the presets.
   */
  private hasServerState = false;
  /** Waiting for the receiver to list its bands; see whenBandsListed(). */
  private bandsListed: (() => void) | null = null;

  constructor(private readonly url?: string) {
    // Resolved lazily in connect(): building it here would make importing this
    // module depend on a DOM, which puts a `location` reference in the way of
    // every unit test that touches anything downstream of it.
    this.client = new SdrClient('', {
      onState: (state, detail) => {
        const wasOpen = connectionState.peek() === 'open';
        connectionState.value = state;
        connectionDetail.value = detail ?? '';
        streamTraffic.value = null;
        // Reset when the stream stops, and again when it comes back. The
        // second one is the one that matters: the first clears a ring that is
        // about to go stale, and everything that happens during the outage
        // happens after it. Coming back with the jitter policy where the
        // outage left it is how a listener ends up at half a second of
        // latency for the rest of the session.
        if (state !== 'open' || !wasOpen) this.player.reset();
      },
      onMessage: (message) => this.handleMessage(message),
      onBinary: (packet) => this.handleBinary(packet),
      onTraffic: (traffic) => { streamTraffic.value = traffic; },
      onInactive: (reason) => {
        inactivityDeadline.value = null;
        inactiveReason.value = reason;
      },
      // The welcome carries the available bands. Restore once, after it arrives.
      onReady: () => {},
    });

    this.player.onStateChange = (state) => {
      audioState.value = state;
      audioDiagnostics.value = state === 'running' ? '' : this.player.diagnostics;
    };
    this.player.onStats = (stats) => {
      audioLatencyMs.value = Math.round(stats.latencyMs);
      audioTargetMs.value = Math.round(stats.targetMs);
      audioUnderruns.value = stats.underruns;
    };
  }

  connect(): void {
    this.client.setUrl(this.url ?? defaultWebSocketUrl());
    this.client.connect();
  }

  disconnect(): void {
    this.cancelPendingTune();
    if (this.viewportTimer !== null) window.cancelAnimationFrame(this.viewportTimer);
    this.viewportTimer = null;
    this.client.close();
  }

  setWaterfallSink(sink: WaterfallSink | null): void {
    this.waterfallSink = sink;
  }

  /**
   * Runs `callback` once the receiver has listed its bands: at once if it
   * already has, otherwise inside the welcome, right after the bands are
   * assigned and before the welcome falls back to the first band and replays
   * the settings, so whatever the callback tunes is what gets replayed.
   * Returns a function that stops waiting.
   */
  whenBandsListed(callback: () => void): () => void {
    if (bands.peek().length > 0) {
      callback();
      return () => {};
    }
    this.bandsListed = callback;
    return () => {
      if (this.bandsListed === callback) this.bandsListed = null;
    };
  }

  // --- user actions ---

  /** Retuning is coalesced: dragging across a band should not flood the link. */
  /**
   * `freq` is the frequency the operator means - the signal. In CW the receiver
   * is tuned one pitch away from it so the signal lands inside the filter;
   * everywhere else the two are the same.
   */
  tune(freq: number, options: Partial<Tuning> = {}, follow = true): void {
    const merged = { ...tuning.value, ...options };
    merged.cwPitch = Math.max(200, Math.min(1500, merged.cwPitch));
    if (options.mode && options.mode !== tuning.value.mode) {
      const defaults = defaultPassband(merged.mode, merged.cwPitch);
      merged.low = options.low ?? defaults.low;
      merged.high = options.high ?? defaults.high;
    }
    const carrier = Math.round(carrierForSignal(freq, merged.mode, merged.cwPitch));
    const next = { ...merged, freq: carrier };
    tuning.value = next;
    if (follow) this.followPassband(next);
    this.queueTune({
      type: 'tune',
      band: next.band,
      freq: Math.round(next.freq),
      mode: next.mode,
      low: next.low,
      high: next.high,
      cw_pitch: next.cwPitch,
    });
  }

  setMode(mode: string): void {
    this.cancelPendingTune();
    const before = tuning.value;
    const signal = signalForCarrier(before.freq, before.mode, before.cwPitch);
    const next = { ...before, mode, ...defaultPassband(mode, before.cwPitch),
      freq: Math.round(carrierForSignal(signal, mode, before.cwPitch)) };
    tuning.value = next;
    this.sendCommand('tune', { type: 'tune', mode, freq: next.freq, band: next.band,
      low: next.low, high: next.high, cw_pitch: next.cwPitch });
  }

  setPassband(low: number, high: number, edge?: 'low' | 'high'): void {
    const band = currentBand.value;
    const current = tuning.value;
    const width = band?.max_bandwidth ?? 20000;
    const minimum = Math.max(-filterLimit.value, -width, (band?.sample_low ?? band?.low ?? -Infinity) - current.freq);
    const maximum = Math.min(filterLimit.value, width, (band?.sample_high ?? band?.high ?? Infinity) - current.freq);
    const passband = constrainPassband(low, high, minimum, maximum, width, edge);
    if (!passband) return;
    const next = { ...current, ...passband };
    tuning.value = next;
    this.queueTune({
      type: 'tune',
      freq: Math.round(next.freq),
      mode: next.mode,
      low: next.low,
      high: next.high,
      band: next.band,
      cw_pitch: next.cwPitch,
    });
  }

  selectBand(id: string): void {
    const band = bands.value.find((b) => b.id === id);
    if (!band || id === tuning.value.band) return;
    this.cancelPendingTune();
    this.switchBand(band);
    const tune = tuning.value;
    this.sendCommand('tune', { type: 'tune', band: id, freq: Math.round(tune.freq),
      mode: tune.mode, low: tune.low, high: tune.high, cw_pitch: tune.cwPitch });
    this.sendViewport();
  }

  private switchBand(band: BandDescription): void {
    this.bandMemories.remember(tuning.value, viewport.value);
    const saved = this.bandMemories.recall(band);
    tuning.value = saved?.tuning ?? { ...tuning.value, band: band.id, freq: band.center };
    viewport.value = { ...viewport.value, lowHz: saved?.lowHz ?? band.low, highHz: saved?.highHz ?? band.high };
  }

  loadBandMemories(value: unknown, lastBand: unknown): void {
    this.bandMemories.load(value);
    const saved = typeof lastBand === 'string' ? this.bandMemories.last(lastBand) : undefined;
    if (!saved) return;
    tuning.value = { ...saved.tuning };
    viewport.value = { ...viewport.value, lowHz: saved.lowHz, highHz: saved.highHz };
    this.hasServerState = true;
  }

  /**
   * Goes to a frequency named somewhere else - a chat message, a band plan
   * marker - crossing bands if it has to.
   *
   * Returns false when no band covers it, so the caller can decline to offer
   * the link at all rather than moving the receiver somewhere surprising.
   *
   * The view follows differently depending on why it is moving. Within the
   * band already on screen, whatever zoom the listener chose is theirs and is
   * kept; the view only slides if the destination is off the edge of it. On a
   * band change a remembered view keeps its zoom, with the destination moved
   * into view when necessary. A band never visited starts at its full span.
   */
  goTo(freq: number, mode?: string): boolean {
    const covers = (b: BandDescription) => freq >= b.low && freq <= b.high;
    const band = bands.value.find((b) => b.id === tuning.value.band && covers(b)) ?? bands.value.find(covers);
    if (!band) return false;

    const changingBand = band.id !== tuning.value.band;
    if (changingBand) {
      this.switchBand(band);
      this.sendViewport();
    }
    const view = viewport.value;
    const span = view.highHz - view.lowHz;
    const margin = span * 0.1;
    if (span > 0 && (freq < view.lowHz + margin || freq > view.highHz - margin)) {
      this.setViewport(freq - span / 2, freq + span / 2);
    }

    if (mode && mode !== tuning.value.mode) {
      this.cancelPendingTune();
      const carrier = Math.round(carrierForSignal(freq, mode, tuning.value.cwPitch));
      const next = { ...tuning.value, band: band.id, mode, freq: carrier,
        ...defaultPassband(mode, tuning.value.cwPitch) };
      tuning.value = next;
      this.sendCommand('tune', { type: 'tune', band: band.id, freq: carrier, mode,
        low: next.low, high: next.high, cw_pitch: next.cwPitch });
    } else {
      this.tune(freq, { band: band.id });
    }
    return true;
  }

  /**
   * Keeps what is being listened to on screen. Tuning by keyboard, by the
   * wheel or by typing a frequency can walk the passband off the edge of the
   * view; once any of it is outside, the view moves to put the passband in
   * the middle, at the zoom the listener chose. Within the view nothing
   * moves, so stepping across a signal on screen does not scroll it away.
   * A drag on the waterfall passes follow = false: the view would slide out
   * from under the pointer.
   */
  private followPassband(next: Tuning): void {
    const view = viewport.value;
    const span = view.highHz - view.lowHz;
    if (!(span > 0)) return;
    const low = next.freq + Math.min(next.low, next.high);
    const high = next.freq + Math.max(next.low, next.high);
    if (low >= view.lowHz && high <= view.highHz) return;
    const middle = (low + high) / 2;
    this.setViewport(middle - span / 2, middle + span / 2);
  }

  setViewport(lowHz: number, highHz: number, width?: number): void {
    const range = constrainViewport(lowHz, highHz, currentBand.value);
    if (!range) return;
    const before = viewport.value;
    const next = { ...before, ...range, ...(width ? { width } : {}) };
    if (next.lowHz === before.lowHz && next.highHz === before.highHz && next.width === before.width) return;
    viewport.value = next;
    this.sendViewport();
  }

  /** The bands, for widgets that want to show them. */
  bandList() {
    return bands.value;
  }

  sendChat(text: string, name: string): void {
    this.client.send({ type: 'chat', text, name });
  }

  requestChatHistory(): void {
    this.client.send({ type: 'chat', history: true });
  }

  setWaterfallWidth(width: number): void {
    this.requestedWaterfallWidth = Math.max(64, Math.min(4096, Math.round(width)));
    const clamped = waterfallRowWidth(Math.min(bandwidthProfile.value.maxWaterfallWidth, Math.round(width)));
    if (clamped === viewport.value.width) return;
    viewport.value = { ...viewport.value, width: clamped };
    this.sendViewport();
  }

  setDsp(patch: Partial<DspSettings>): void {
    dsp.value = { ...dsp.value, ...patch };
    // One message per frame at most, with the latest settings: a slider
    // dragged across its range must not send one for every step it passes.
    if (this.dspTimer !== null) return;
    this.dspTimer = window.requestAnimationFrame(() => {
      this.dspTimer = null;
      this.flushDsp();
    });
  }

  private flushDsp(): void {
    const next = dsp.value;
    this.sendCommand('dsp', {
      type: 'dsp',
      agc: next.agc,
      gain: next.gain,
      nr: next.nr,
      autonotch: next.autonotch,
      squelch: next.squelch,
      auto_squelch: next.autoSquelch,
      notches: next.notches,
      highpass: next.highpass,
      deemphasis: next.deemphasis,
      wfm_deemphasis: wfmDeemphasis(next),
      ctcss_filter: next.ctcssFilter,
      ctcss_squelch: next.ctcssSquelch,
    });
  }

  setBandwidthProfile(profile: BandwidthProfile): void {
    bandwidthProfile.value = profile;
    const width = waterfallRowWidth(Math.min(this.requestedWaterfallWidth, profile.maxWaterfallWidth));
    viewport.value = { ...viewport.value, width, fps: profile.waterfallFps };
    this.client.send(audioCommand(profile));
    this.sendViewport();
  }

  setVolume(value: number): void {
    volume.value = value;
    this.player.setVolume(value);
  }

  setMuted(value: boolean): void {
    muted.value = value;
    this.player.setMuted(value);
  }

  async startAudio(): Promise<void> {
    await this.player.start();
    this.player.setVolume(volume.value);
    this.player.setMuted(muted.value);
  }

  // --- protocol ---

  private queueTune(message: Record<string, unknown>): void {
    this.pendingTune = { ...message, request_id: ++this.revision.tune };
    if (this.sendTimer !== null) return;
    this.sendTimer = window.requestAnimationFrame(() => {
      this.sendTimer = null;
      this.flushTune();
    });
  }

  private flushTune(): void {
    if (!this.pendingTune) return;
    this.client.send(this.pendingTune);
    this.pendingTune = null;
  }

  private cancelPendingTune(): void {
    if (this.sendTimer !== null) window.cancelAnimationFrame(this.sendTimer);
    this.sendTimer = null;
    this.pendingTune = null;
  }

  private sendCommand(kind: 'tune' | 'dsp', message: Record<string, unknown>): void {
    this.client.send({ ...message, request_id: ++this.revision[kind] });
    // Tuning is activity to the receiver too; the question is answered.
    inactivityDeadline.value = null;
  }

  /** Answers the receiver's "still listening?". */
  stillListening(): void {
    this.client.send({ type: 'active' });
    inactivityDeadline.value = null;
  }

  /** After the receiver let this listener's place go: take one again. */
  listenAgain(): void {
    inactiveReason.value = null;
    this.client.connect();
  }

  private sendViewport(): void {
    ++this.revision.viewport;
    if (this.viewportTimer !== null) return;
    // Local drawing reads the new view immediately. The server gets the same
    // frame's latest view, without queueing obsolete pointer positions.
    this.viewportTimer = window.requestAnimationFrame(() => {
      this.viewportTimer = null;
      this.flushTune(); // a cross-band tune must precede its viewport
      this.flushViewport();
    });
  }

  private flushViewport(): void {
    const view = viewport.value;
    this.client.send({
      type: 'viewport',
      codec: this.waterfallCodec,
      step_db: this.waterfallCodec === 'wfc4' || this.waterfallCodec === 'wfc5' ? bandwidthProfile.value.waterfallStepDb : 1,
      request_id: this.revision.viewport,
      enabled: true,
      low: view.lowHz,
      high: view.highHz,
      width: view.width,
      fps: view.fps,
    });
  }

  private restoreSettings(): void {
    this.cancelPendingTune();
    // Replay everything after a reconnect so the user lands back where they
    // were rather than at the middle of the first band.
    const t = tuning.value;
    if (t.band) {
      this.sendCommand('tune', {
        type: 'tune',
        band: t.band,
        freq: Math.round(t.freq),
        mode: t.mode,
        cw_pitch: t.cwPitch,
        // Only restore a passband the user actually has.
        ...(this.hasServerState ? { low: t.low, high: t.high } : {}),
      });
    }
    const d = dsp.value;
    this.sendCommand('dsp', {
      type: 'dsp',
      agc: d.agc,
      gain: d.gain,
      nr: d.nr,
      autonotch: d.autonotch,
      squelch: d.squelch,
      auto_squelch: d.autoSquelch,
      notches: d.notches,
      highpass: d.highpass,
      deemphasis: d.deemphasis,
      wfm_deemphasis: wfmDeemphasis(d),
      ctcss_filter: d.ctcssFilter,
      ctcss_squelch: d.ctcssSquelch,
    });
    this.client.send(audioCommand(bandwidthProfile.value));
    if (viewport.value.highHz > viewport.value.lowHz) this.sendViewport();
  }

  private handleMessage(message: ServerMessage): void {
    switch (message.type) {
      case 'welcome': {
        this.waterfallCodec = message.capabilities?.includes('wfc5') ? 'wfc5' :
          message.capabilities?.includes('wfc4') ? 'wfc4' :
          message.capabilities?.includes('wfc3') ? 'wfc3' : 'wfc2';
        const capabilities = ['meter-v1', 'meter-ctcss', 'nac2', 'nac3', 'audio-discontinuity']
          .filter((name) => message.capabilities?.includes(name));
        if (capabilities.length) this.client.send({ type: 'hello', capabilities });
        site.value = message.site;
        rds.value = null;
        operatorTheme.value = (message.theme as Theme) ?? null;
        applyOperatorTheme((message.theme as Theme) ?? null);
        adoptOperatorPalette((message.theme as Theme) ?? null);
        bands.value = message.bands;
        if (message.bands.length > 0 && this.bandsListed) {
          const listed = this.bandsListed;
          this.bandsListed = null;
          listed();
        }
        notice.value = message.site.notice ?? '';
        decoders.value = Array.isArray(message.decoders) ? message.decoders : [];
        // A remembered band that is off the air by its hours gives way to the
        // band on its input, or the first on the air, as the receiver would
        // put the page there anyway and say so.
        const remembered = message.bands.find((band) => band.id === tuning.value.band);
        if (remembered && offAir(remembered)) {
          const next = message.bands.find((band) => !offAir(band) && remembered.shared_input !== undefined &&
            band.shared_input === remembered.shared_input) ?? message.bands.find((band) => !offAir(band));
          if (next) tuning.value = { ...tuning.value, band: next.id, freq: next.center };
          if (next) viewport.value = { ...viewport.value, lowHz: next.low, highHz: next.high };
        }
        if (!message.bands.some((band) => band.id === tuning.value.band) && message.bands.length > 0) {
          const first = message.bands.find((band) => !offAir(band)) ?? message.bands[0];
          tuning.value = { ...tuning.value, band: first.id, freq: first.center };
          viewport.value = { ...viewport.value, lowHz: first.low, highHz: first.high };
        }
        const band = currentBand.value;
        if (band) {
          tuning.value = { ...tuning.value, freq: Math.max(band.low, Math.min(band.high, tuning.value.freq)) };
          viewport.value = { ...viewport.value, ...constrainViewport(viewport.value.lowHz, viewport.value.highHz, band) };
        }
        this.restoreSettings();
        break;
      }
      // The operator changed the receiver's appearance. Applied to this page
      // immediately: an operator adjusting colours wants to see the result on
      // the receiver, not to tell everyone to reload.
      case 'chat':
        chatLog.value = [...chatLog.value, message as unknown as ChatLine].slice(-120);
        chatRefusal.value = '';
        break;

      case 'chat-history':
        chatLog.value = ((message as { messages?: ChatLine[] }).messages ?? []).slice(-120);
        break;

      case 'chat-refused':
        chatRefusal.value = (message as { reason?: string }).reason ?? '';
        window.setTimeout(() => (chatRefusal.value = ''), 4000);
        break;

      case 'station':
        site.value = { ...(site.value ?? {}), ...message.site };
        notice.value = message.site.notice ?? '';
        if (Array.isArray(message.decoders)) decoders.value = message.decoders;
        break;

      case 'theme':
        operatorTheme.value = (message.theme as Theme) ?? null;
        applyOperatorTheme((message.theme as Theme) ?? null);
        adoptOperatorPalette((message.theme as Theme) ?? null);
        break;

      case 'state':
        this.adoptState(message);
        break;
      case 'inactivity':
        inactivityDeadline.value = performance.now() + Math.max(0, message.seconds) * 1000;
        break;
      case 'meter':
        meter.value = message;
        break;
      case 'rds':
        rds.value = message.ps || message.rt || message.pi ? message : null;
        break;
      case 'band-status': {
        const updates = new Map(message.bands.map((band) => [band.id, band]));
        bands.value = bands.value.map((band) => {
          const update = updates.get(band.id);
          return update ? { ...band, ...update } : band;
        });
        break;
      }
      case 'audio-config':
        if (message.generation !== this.audioGeneration || message.rate !== this.audioRate) {
          this.audioGeneration = message.generation;
          this.audioRate = message.rate;
          this.player.configure(message.rate, message.generation);
        }
        break;
      case 'error':
        errorMessage.value = message.message;
        window.setTimeout(() => {
          if (errorMessage.value === message.message) errorMessage.value = '';
        }, 6000);
        break;
      default:
        break;
    }
  }

  /**
   * The server is authoritative: it clamps what it cannot honour, and the UI
   * shows what is actually happening rather than what was asked for.
   */
  private adoptState(message: StateMessage): void {
    this.hasServerState = true;
    // A reply describes the server when it was sent, which can be several
    // gestures ago. Acknowledgements are independent for each control group.
    if (!message.ack || message.ack.tune >= this.revision.tune) {
      if (Number.isFinite(message.filter_limit) && message.filter_limit! >= 25) filterLimit.value = message.filter_limit!;
      tuning.value = {
        band: message.band,
        freq: message.freq,
        mode: message.mode,
        low: message.low,
        high: message.high,
        cwPitch: message.cw_pitch,
      };
    }
    if (!message.ack || message.ack.dsp >= this.revision.dsp) {
      dsp.value = {
        agc: message.agc,
        gain: message.gain,
        nr: message.nr,
        autonotch: message.autonotch,
        notches: message.notches ?? [],
        squelch: message.squelch,
        autoSquelch: message.auto_squelch ?? false,
        highpass: message.highpass ?? 0,
        deemphasis: message.deemphasis ?? 300,
        // The page's own: null follows the region, which the server's echo
        // of the resolved number would otherwise pin.
        wfmDeemphasis: dsp.value.wfmDeemphasis,
        ctcssFilter: message.ctcss_filter ?? true,
        ctcssSquelch: message.ctcss_squelch ?? 0,
      };
    }
    if (message.viewport && (!message.ack || message.ack.viewport >= this.revision.viewport)) {
      viewport.value = {
        lowHz: message.viewport.low,
        highHz: message.viewport.high,
        width: message.viewport.width,
        fps: message.viewport.fps,
      };
    }
    if (message.note) {
      notice.value = message.note;
      window.setTimeout(() => {
        if (notice.value === message.note) notice.value = site.value?.notice ?? '';
      }, 5000);
    }
  }

  private handleBinary(packet: ServerBinary): void {
    if (packet.kind === 'audio') {
      this.player.feed(packet);
    } else if (packet.kind === 'meter') {
      this.handleMessage(packet);
    } else {
      this.waterfallSink?.(packet);
    }
  }
}

export const controller = new RadioController();
