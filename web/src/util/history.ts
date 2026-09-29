export interface Archive {
  bins: number;
  floorDb: number;
  ceilingDb: number;
  lowHz: number;
  highHz: number;
  oldest: number;
  newest: number;
  rowMs: number;
  times: number[];
  rows: Uint8Array;
}

export function decodeHistory(buffer: ArrayBuffer): Archive {
  const invalid = () => new Error('The receiver returned an invalid history record.');
  if (buffer.byteLength < 4 || buffer.byteLength > 2 * 1024 * 1024) throw invalid();
  const size = new DataView(buffer).getUint32(0, true);
  if (size > 256 * 1024 || size > buffer.byteLength - 4) throw invalid();
  const h = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, 4, size)));
  if (!h || !Number.isInteger(h.bins) || h.bins < 1 || h.bins > 8192 ||
      ![h.floor_db, h.ceiling_db, h.low_hz, h.high_hz, h.oldest_ms, h.newest_ms, h.row_interval_ms].every(Number.isFinite) ||
      h.ceiling_db <= h.floor_db || h.high_hz <= h.low_hz || h.row_interval_ms <= 0 ||
      !Array.isArray(h.times) || h.times.length > 4096 ||
      h.times.some((time: unknown, i: number) => !Number.isSafeInteger(time) || (time as number) < 0 ||
        (i > 0 && (time as number) <= h.times[i - 1])) ||
      h.bins * h.times.length !== buffer.byteLength - 4 - size) throw invalid();
  return { bins: h.bins, floorDb: h.floor_db, ceilingDb: h.ceiling_db, lowHz: h.low_hz,
    highHz: h.high_hz, oldest: h.oldest_ms, newest: h.newest_ms, rowMs: h.row_interval_ms,
    times: h.times, rows: new Uint8Array(buffer, 4 + size) };
}

export function historyReach(oldest: number, newest: number, hours: number): number {
  return oldest > 0 && newest >= oldest ? Math.max(0, (newest - oldest) / 3_600_000 - hours) : 0;
}

export function historyRowRange(time: number, rowMs: number, from: number, to: number, height: number) {
  const scale = height / Math.max(1, to - from);
  return [Math.max(0, Math.min(height, Math.floor((to - time - rowMs) * scale))),
    Math.max(0, Math.min(height, Math.ceil((to - time) * scale)))] as const;
}

export function historyWindow(from: number, to: number, oldest: number) {
  // Do not spend most of a young archive's display on the hours before it
  // existed. Keep the requested end so an ongoing outage still appears blank.
  return { from: oldest > from && oldest < to ? oldest : from, to };
}

export interface TimeTick {
  time: number;
  /** From the top of the picture, 0 to 1: the newest recording is at the top. */
  position: number;
  label: string;
  /** A new day starts here, and the label is its date. */
  day: boolean;
}

/** Steps a time axis may use, in minutes. */
const TICK_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 180, 360, 720];
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

/**
 * Time marks for the history's side, on the clock rather than evenly spread:
 * 14:00, 14:15, 14:30 rather than 14:03, 14:21, 14:39, so a listener can say
 * when something was. As many as fit `minSpacingPx` apart in `heightPx`. At
 * midnight the mark gives the date instead, in the chosen zone.
 */
export function timeTicks(from: number, to: number, heightPx: number, zone: 'utc' | 'local', minSpacingPx = 36): TimeTick[] {
  const span = to - from;
  if (!(span > 0) || !(heightPx > 0)) return [];
  const pxPerMinute = heightPx / (span / 60_000);
  const step = (TICK_STEPS.find((minutes) => minutes * pxPerMinute >= minSpacingPx) ?? 1440) * 60_000;
  // Where the zone's clock stands relative to UTC at a time, so marks fall on
  // its own round minutes, summer time included.
  const offset = (time: number) => (zone === 'utc' ? 0 : -new Date(time).getTimezoneOffset() * 60_000);
  const pad = (n: number) => String(n).padStart(2, '0');
  const ticks: TimeTick[] = [];
  let time = Math.ceil((from + offset(from)) / step) * step - offset(from);
  for (let guard = 0; time <= to && guard < 500; guard++, time += step) {
    const clock = new Date(time + offset(time));
    const hours = clock.getUTCHours();
    const minutes = clock.getUTCMinutes();
    const day = hours === 0 && minutes === 0;
    ticks.push({
      time,
      position: (to - time) / span,
      day,
      label: day ? `${WEEKDAYS[clock.getUTCDay()]} ${clock.getUTCDate()}` : `${pad(hours)}:${pad(minutes)}`,
    });
  }
  return ticks;
}
