/** Frequencies the way operators read them: megahertz, to the kilohertz or finer. */
export function megahertz(hz: number, digits = 3): string {
  return (hz / 1e6).toFixed(digits);
}

/**
 * A signed number with a real minus sign. The hyphen a number prints with is shorter than a
 * digit and sits low, so a column of levels looks ragged and -84 reads as a dash and a number.
 */
export function signed(value: number, digits = 1, plus = false): string {
  const text = Math.abs(value).toFixed(digits);
  if (Number(text) === 0) return text;
  return `${value < 0 ? '\u2212' : plus ? '+' : ''}${text}`;
}

/** A level relative to full scale, as the receiver measures everything before calibration. */
export function dbfs(value: number, digits = 1): string {
  return `${signed(value, digits)} dBFS`;
}

/**
 * A small share as a percentage, to two significant digits: clipped samples are worth noticing
 * at one in ten thousand, which a percentage rounded to whole numbers would show as 0%.
 */
export function share(fraction: number): string {
  if (fraction <= 0) return '0%';
  return `${Number((fraction * 100).toPrecision(2))}%`;
}

export function kilohertz(hz: number): string {
  return hz >= 1000 ? `${(hz / 1000).toFixed(hz % 1000 ? 1 : 0)} kHz` : `${hz} Hz`;
}

export function bitrate(bits: number): string {
  if (bits >= 1e6) return `${(bits / 1e6).toFixed(1)} Mbit/s`;
  if (bits >= 1000) return `${(bits / 1000).toFixed(bits >= 100000 ? 0 : 1)} kbit/s`;
  return `${Math.round(bits)} bit/s`;
}

export function duration(seconds: number): string {
  if (seconds < 60) return `${Math.floor(seconds)} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  const hours = Math.floor(seconds / 3600);
  return `${hours} h ${Math.floor((seconds % 3600) / 60)} min`;
}

export function bytes(size: number): string {
  if (size >= 1e9) return `${(size / 1e9).toFixed(1)} GB`;
  if (size >= 1e6) return `${(size / 1e6).toFixed(1)} MB`;
  return `${Math.max(1, Math.round(size / 1000))} kB`;
}

/** "just now", "4 min ago", or a date once it is old enough for the time not to matter. */
export function ago(ms: number | undefined, now = Date.now()): string {
  if (!ms) return 'never';
  const seconds = Math.max(0, (now - ms) / 1000);
  if (seconds < 45) return 'just now';
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return new Date(ms).toLocaleDateString();
}
