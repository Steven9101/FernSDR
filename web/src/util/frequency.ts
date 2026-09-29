/**
 * Frequency parsing and formatting. Pure functions, kept apart from the
 * components so they can be tested without a DOM.
 */

/**
 * Accepts what people actually type. A bare number is megahertz if it looks
 * like megahertz and kilohertz if it looks like kilohertz, because "7100" and
 * "7.1" obviously mean the same thing to the person typing them.
 */
export function parseFrequency(text: string): number | null {
  const trimmed = text.trim().toLowerCase().replace(/\s+/g, '');
  if (!trimmed) return null;

  const match = trimmed.match(/^([0-9]*\.?[0-9]+)(hz|khz|k|mhz|m|ghz|g)?$/);
  if (!match) return null;

  const value = Number.parseFloat(match[1]);
  if (!Number.isFinite(value)) return null;

  switch (match[2]) {
    case 'hz': return value;
    case 'k':
    case 'khz': return value * 1e3;
    case 'm':
    case 'mhz': return value * 1e6;
    case 'g':
    case 'ghz': return value * 1e9;
    default: break;
  }

  // No unit. Below 100 is megahertz (7.1, 14.074); 100 up to a million is
  // kilohertz (7100, 14074); above that, hertz.
  if (value < 100) return value * 1e6;
  if (value < 1e6) return value * 1e3;
  return value;
}

/** "14.074.000" style grouping, for compact readouts. */
export function formatFrequency(hz: number, decimals = 3): string {
  const mhz = hz / 1e6;
  return `${mhz.toFixed(decimals)} MHz`;
}

/** A span, in the largest unit that keeps it readable. */
export function formatSpan(hz: number): string {
  if (hz >= 1e6) return `${(hz / 1e6).toFixed(hz >= 1e7 ? 1 : 2)} MHz`;
  if (hz >= 1e3) return `${(hz / 1e3).toFixed(hz >= 1e5 ? 0 : 1)} kHz`;
  return `${Math.round(hz)} Hz`;
}

/** Bitrate for the status readout. */
export function formatBitrate(bitsPerSecond: number): string {
  if (bitsPerSecond >= 1e6) return `${(bitsPerSecond / 1e6).toFixed(2)} Mbit/s`;
  return `${(bitsPerSecond / 1e3).toFixed(1)} kbit/s`;
}

/**
 * A number with a real minus sign (U+2212) when negative: the hyphen is
 * shorter and sits lower, and next to the SAM offset, which has always used
 * the minus, the two looked like different symbols.
 */
export function formatSigned(value: number, decimals = 1): string {
  const text = Math.abs(value).toFixed(decimals);
  return value < 0 && Number(text) !== 0 ? `−${text}` : text;
}
