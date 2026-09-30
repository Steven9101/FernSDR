/**
 * Remembering where you were.
 *
 * Two mechanisms, on purpose:
 *   - the URL fragment carries the tuning, so a link to a signal is a link to
 *     that signal. People share these constantly ("listen to this on 7.130"),
 *     and a receiver that always opens in the same place makes that impossible.
 *   - local storage carries preferences - volume, palette, data profile -
 *     which belong to the person, not to the link.
 *
 * The split matters: sending someone your link should not change their
 * volume, and it should not silently put them on your data plan setting.
 */
import { watch } from './reactive.svelte';
import {
  applyTheme,
  BANDWIDTH_PROFILES,
  bands,
  bandwidthProfile,
  controller,
  display,
  dsp,
  muted,
  theme,
  tuning,
  volume,
  viewport,
  type DisplaySettings,
  type ThemeChoice,
} from './store';
import { signalForCarrier } from '../util/cw';
import { readStored } from '../util/storage';

const STORAGE_KEY = 'fernsdr.preferences.v2';
// Until v2 the default gain control was Slow, stored like any choice, and the
// present engine was called Steady. Both come across as Auto, which is Slow
// now and follows the receiver's recommendation after.
const OLD_STORAGE_KEY = 'fernsdr.preferences.v1';
const bandStorageKey = () => `fernsdr.bands.v1:${location.pathname}`;

export interface SharedTuning {
  band?: string;
  freq?: number;
  mode?: string;
  low?: number;
  high?: number;
  cwPitch?: number;
  viewLow?: number;
  viewHigh?: number;
}

/** Reads a shareable tuning out of the URL fragment. */
export function readUrlTuning(): SharedTuning {
  const hash = location.hash.replace(/^#/, '');
  if (!hash) return {};
  const params = new URLSearchParams(hash);
  const out: SharedTuning = {};

  const band = params.get('band');
  if (band) out.band = band;

  const freq = Number(params.get('f'));
  if (Number.isFinite(freq) && freq > 0) out.freq = freq;

  const mode = params.get('m');
  if (mode) out.mode = mode.toLowerCase();
  const pitch = Number(params.get('pitch'));
  if (Number.isFinite(pitch) && pitch >= 200 && pitch <= 1500) out.cwPitch = pitch;

  const bandwidth = params.get('bw');
  if (bandwidth) {
    const [low, high] = bandwidth.split(',').map(Number);
    if (Number.isFinite(low) && Number.isFinite(high)) {
      out.low = low;
      out.high = high;
    }
  }
  const view = params.get('view')?.split(',').map(Number);
  if (view?.length === 2 && view.every(Number.isFinite) && view[1] > view[0]) {
    [out.viewLow, out.viewHigh] = view;
  }
  return out;
}

/**
 * Opens on a shared link's tuning as soon as the receiver has listed its
 * bands. A shared link wins over anything remembered: the whole point of
 * sending one is that it opens on that signal.
 *
 * It runs inside the welcome, the moment the bands are assigned, before the
 * welcome falls back to the first band and replays the settings. That order
 * matters: the replay then carries the shared tuning. Returns a function that
 * stops waiting.
 */
export function applySharedTuning(shared: SharedTuning): () => void {
  const freq = shared.freq;
  if (!freq) return () => {};
  return controller.whenBandsListed(() => {
    const target = bands.value.find((band) => band.id === shared.band) ??
      bands.value.find((band) => freq >= band.low && freq <= band.high);
    if (target) controller.selectBand(target.id);
    controller.tune(freq, {
      ...(shared.mode ? { mode: shared.mode } : {}),
      ...(shared.cwPitch !== undefined ? { cwPitch: shared.cwPitch } : {}),
      ...(shared.low !== undefined ? { low: shared.low } : {}),
      ...(shared.high !== undefined ? { high: shared.high } : {}),
    });
    // Frame the view around it rather than dropping the user on a band
    // edge with the signal off screen.
    controller.setViewport(shared.viewLow ?? freq - 30_000, shared.viewHigh ?? freq + 30_000);
  });
}

/** The link that would bring someone back to exactly this. */
export function shareUrl(): string {
  const tune = tuning.value;
  const params = new URLSearchParams();
  if (tune.band) params.set('band', tune.band);
  params.set('f', String(Math.round(signalForCarrier(tune.freq, tune.mode, tune.cwPitch))));
  params.set('m', tune.mode);
  if (tune.mode === 'cw' || tune.mode === 'cwl') params.set('pitch', String(Math.round(tune.cwPitch)));
  params.set('bw', `${Math.round(tune.low)},${Math.round(tune.high)}`);
  const view = viewport.value;
  if (view.highHz > view.lowHz) params.set('view', `${Math.round(view.lowHz)},${Math.round(view.highHz)}`);
  return `${location.origin}${location.pathname}#${params.toString()}`;
}

interface StoredPreferences {
  volume: number;
  muted: boolean;
  profile: string;
  display: Partial<DisplaySettings>;
  agc: string;
  nr: number;
  autonotch: boolean;
  highpass: number;
  deemphasis: number;
  wfmDeemphasis?: number;
  ctcssFilter: boolean;
  theme: ThemeChoice;
}

export function loadPreferences(): void {
  try {
    const memory = JSON.parse(readStored(bandStorageKey()) ?? 'null');
    if (memory) controller.loadBandMemories(memory.bands, memory.lastBand);
  } catch {
    // A damaged memory entry must not prevent loading the ordinary controls.
  }
  let stored: Partial<StoredPreferences> | null = null;
  try {
    const raw = readStored(STORAGE_KEY);
    if (raw) {
      stored = JSON.parse(raw) as Partial<StoredPreferences>;
    } else {
      const old = readStored(OLD_STORAGE_KEY);
      if (old) {
        stored = JSON.parse(old) as Partial<StoredPreferences>;
        if (stored && (stored.agc === 'slow' || stored.agc === 'steady')) stored.agc = 'auto';
      }
    }
  } catch {
    // Private browsing, disabled storage, corrupt JSON: defaults are fine.
    return;
  }
  if (!stored) return;

  if (typeof stored.volume === 'number') volume.value = Math.max(0, Math.min(1, stored.volume));
  if (typeof stored.muted === 'boolean') muted.value = stored.muted;

  const profile = BANDWIDTH_PROFILES.find((p) => p.id === stored?.profile);
  if (profile) {
    // The profile's row rate as well as the profile: this runs before the
    // first connection, and the viewport sent then is where the server takes
    // the rate from. Choosing a profile on the page does the same.
    bandwidthProfile.value = profile;
    viewport.value = { ...viewport.value, fps: profile.waterfallFps };
  }

  if (stored.display) {
    display.value = { ...display.value, ...stored.display };
    if (display.value.controls !== 'full') display.value = { ...display.value, controls: 'essential' };
    // Settings saved before the listener could tell their own palette from
    // the default carry one anyway; those follow the operator from now on.
    if (display.value.paletteChosen !== true) display.value = { ...display.value, paletteChosen: false };
  }

  if (stored.theme === 'auto' || stored.theme === 'dark' || stored.theme === 'light') {
    applyTheme(stored.theme);
  }

  dsp.value = {
    ...dsp.value,
    ...(typeof stored.agc === 'string' ? { agc: stored.agc } : {}),
    ...(typeof stored.nr === 'number' ? { nr: stored.nr } : {}),
    ...(typeof stored.autonotch === 'boolean' ? { autonotch: stored.autonotch } : {}),
    ...(typeof stored.highpass === 'number' && stored.highpass >= 0 && stored.highpass <= 1000
      ? { highpass: stored.highpass }
      : {}),
    ...(typeof stored.deemphasis === 'number' && stored.deemphasis >= 0 && stored.deemphasis <= 2000
      ? { deemphasis: stored.deemphasis }
      : {}),
    ...(typeof stored.wfmDeemphasis === 'number' && stored.wfmDeemphasis >= 0 && stored.wfmDeemphasis <= 200
      ? { wfmDeemphasis: stored.wfmDeemphasis }
      : {}),
    ...(typeof stored.ctcssFilter === 'boolean' ? { ctcssFilter: stored.ctcssFilter } : {}),
  };
}

let saveTimer: number | null = null;

/** Starts mirroring preferences to storage and the tuning to the URL. */
export function startPersistence(): () => void {
  let flushStorage = () => {};
  const disposeStorage = watch(() => {
    const tune = tuning.value;
    const view = viewport.value;
    const snapshot: StoredPreferences = {
      volume: volume.value,
      muted: muted.value,
      profile: bandwidthProfile.value.id,
      display: display.value,
      agc: dsp.value.agc,
      nr: dsp.value.nr,
      autonotch: dsp.value.autonotch,
      highpass: dsp.value.highpass,
      deemphasis: dsp.value.deemphasis,
      ...(dsp.value.wfmDeemphasis !== null ? { wfmDeemphasis: dsp.value.wfmDeemphasis } : {}),
      ctcssFilter: dsp.value.ctcssFilter,
      theme: theme.value,
    };
    if (saveTimer !== null) window.clearTimeout(saveTimer);
    flushStorage = () => {
      try {
        controller.bandMemories.remember(tune, view);
        localStorage.setItem(bandStorageKey(), JSON.stringify({
          lastBand: tune.band, bands: controller.bandMemories.snapshot(),
        }));
        localStorage.setItem(STORAGE_KEY, JSON.stringify(snapshot));
      } catch {
        // Nothing to be done; preferences simply will not persist.
      }
    };
    saveTimer = window.setTimeout(flushStorage, 400);
  });

  let urlTimer: number | null = null;
  const disposeUrl = watch(() => {
    // Touch the signals this depends on.
    const tune = tuning.value;
    void tune.freq;
    void viewport.value;
    if (urlTimer !== null) window.clearTimeout(urlTimer);
    // replaceState rather than pushState: tuning should not fill the back
    // button with hundreds of entries.
    urlTimer = window.setTimeout(() => {
      const url = shareUrl();
      if (url !== location.href) history.replaceState(null, '', url);
    }, 500);
  });
  const flushOnLeave = () => flushStorage();
  window.addEventListener('pagehide', flushOnLeave);

  return () => {
    flushStorage();
    if (saveTimer !== null) window.clearTimeout(saveTimer);
    if (urlTimer !== null) window.clearTimeout(urlTimer);
    window.removeEventListener('pagehide', flushOnLeave);
    disposeStorage();
    disposeUrl();
  };
}
