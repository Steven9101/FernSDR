/**
 * Whether the listener's link has room for what the page can do without:
 * its typeface, and the band plan's data. Both would otherwise come in over
 * the same connection as the first seconds of sound, which on a slow link
 * then start later.
 *
 * `linkHasRoom()` resolves once the waterfall has run at the rate the
 * listener asked for over three seconds, the receiver sending every line,
 * or at once where the browser says the link is fast (Chromium's estimate).
 * It never resolves where the listener asked the browser to save data or the
 * browser knows the link is 2G.
 */
import { bandwidthProfile, meter } from './store';
import { watch } from './reactive.svelte';

const SETTLED_SECONDS = 3;

let room: Promise<void> | null = null;

export function linkHasRoom(): Promise<void> {
  room ??= new Promise((resolve) => {
    const connection = (navigator as Navigator & {
      connection?: { saveData?: boolean; effectiveType?: string; downlink?: number };
    }).connection;
    if (connection?.saveData || /2g/.test(connection?.effectiveType ?? '')) return;
    if (connection?.effectiveType === '4g' && (connection.downlink ?? 0) >= 5) {
      resolve();
      return;
    }
    let steady = 0;
    let last = 0;
    let done = false;
    const stop = watch(() => {
      const fps = meter.value?.waterfall_fps ?? 0;
      const target = bandwidthProfile.value.waterfallFps;
      if (done || fps <= 0) return;
      const now = performance.now();
      // Held back below the rate asked for, the stream is being slowed to
      // fit the link, and anything else would take what the sound needs.
      if (fps >= target - 0.5) steady += last > 0 ? (now - last) / 1000 : 0;
      else steady = 0;
      last = now;
      if (steady >= SETTLED_SECONDS) {
        done = true;
        resolve();
        queueMicrotask(() => stop());
      }
    });
  });
  return room;
}

/** The typeface, when the link has room; the system's sans serif until then. */
export function loadFontWhenTheLinkAllows(): void {
  void linkHasRoom().then(() => import('@fontsource-variable/inter/wght.css'));
}

/**
 * When the link has room, or after `ms` regardless: for what the page shows
 * a little later on a slow link rather than not at all.
 */
export function roomOrLater(ms: number): Promise<void> {
  return Promise.race([linkHasRoom(), new Promise<void>((resolve) => setTimeout(resolve, ms))]);
}
