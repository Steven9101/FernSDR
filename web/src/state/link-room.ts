/**
 * Whether the listener's link has room for what the page can do without:
 * its typeface, and the band plan's data. Both would otherwise come in over
 * the same connection as the first seconds of sound, which on a slow link
 * then start later.
 *
 * `linkHasRoom()` resolves as soon as the stream runs where the page's own
 * script came in at a megabit a second or more, and never where it came in
 * slower. With nothing measured, the script from the cache, it resolves once
 * the waterfall has run at the rate the listener asked for over eight
 * seconds. It never resolves where the listener asked the browser to save
 * data or the browser knows the link is 2G. The browser's estimate of a fast
 * link is not taken: Chromium calls a fresh page's link 4G at 10 Mbit/s
 * before it has measured anything, a 24 kbit/s one included; and the
 * stream's first seconds say little, since the receiver sends at the full
 * rate until it has seen the link fall behind.
 */
import { bandwidthProfile, meter } from './store';
import { watch } from './reactive.svelte';

/** Seconds of the full waterfall rate that count as room, where nothing else says. */
const SETTLED_SECONDS = 8;
/** What the page's own script came in at, from which on the link counts as roomy. */
const ROOMY_BPS = 1_000_000;

/**
 * How fast this link delivered the page's largest script, in bits a second,
 * from the browser's own timing of it: the time between its first and last
 * byte, which leaves out connecting and the server's thinking. Null when it
 * came from the cache or cannot be timed.
 */
export function measuredDownlink(entries: readonly { initiatorType: string; transferSize: number; responseStart: number; responseEnd: number }[]): number | null {
  let largest: (typeof entries)[number] | null = null;
  for (const entry of entries) {
    if (entry.initiatorType !== 'script' || entry.transferSize < 20_000) continue;
    if (!largest || entry.transferSize > largest.transferSize) largest = entry;
  }
  if (!largest || largest.responseStart <= 0) return null;
  const seconds = (largest.responseEnd - largest.responseStart) / 1000;
  return seconds <= 0 ? Number.POSITIVE_INFINITY : (largest.transferSize * 8) / seconds;
}

let room: Promise<void> | null = null;

export function linkHasRoom(): Promise<void> {
  room ??= new Promise((resolve) => {
    const connection = (navigator as Navigator & { connection?: { saveData?: boolean; effectiveType?: string } }).connection;
    if (connection?.saveData || /2g/.test(connection?.effectiveType ?? '')) return;
    let downlink: number | null = null;
    try {
      downlink = measuredDownlink(performance.getEntriesByType('resource') as PerformanceResourceTiming[]);
    } catch {
      // No resource timing: the stream decides alone.
    }
    // Measured slow: the sound needs all of it, for good.
    if (downlink !== null && downlink < ROOMY_BPS) return;
    let steady = 0;
    let last = 0;
    let done = false;
    const stop = watch(() => {
      const fps = meter.value?.waterfall_fps ?? 0;
      const target = bandwidthProfile.value.waterfallFps;
      if (done || fps <= 0) return;
      // Measured fast: as soon as the stream is running.
      const needed = downlink !== null ? 0 : SETTLED_SECONDS;
      const now = performance.now();
      // Held back below the rate asked for, the stream is being slowed to
      // fit the link, and anything else would take what the sound needs.
      if (fps >= target - 0.5) steady += last > 0 ? (now - last) / 1000 : 0;
      else steady = 0;
      last = now;
      if (steady >= needed) {
        done = true;
        resolve();
        queueMicrotask(() => stop());
      }
    });
  });
  return room;
}

const FONT_KEY = 'fernsdr.font';

/**
 * The typeface, when the link has room; the system's sans serif until then.
 * Once it has been loaded in this browser it is in the browser's cache, costs
 * the link nothing, and is taken at once, so that only a first visit sees the
 * letterforms change.
 */
export function loadFontWhenTheLinkAllows(): void {
  let cached = false;
  try {
    cached = localStorage.getItem(FONT_KEY) === '1';
  } catch {
    // No storage: the first-visit rule every time.
  }
  const load = () => {
    void import('../styles/inter.css');
    try {
      localStorage.setItem(FONT_KEY, '1');
    } catch {
      // Kept for this page only.
    }
  };
  if (cached) load();
  else void linkHasRoom().then(load);
}

/**
 * When the link has room, or after `ms` regardless: for what the page shows
 * a little later on a slow link rather than not at all.
 */
export function roomOrLater(ms: number): Promise<void> {
  return Promise.race([linkHasRoom(), new Promise<void>((resolve) => setTimeout(resolve, ms))]);
}
