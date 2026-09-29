/**
 * Frequencies people mention in chat, turned into somewhere you can go.
 *
 * A receiver's chat is mostly one thing: somebody says what they are hearing
 * and where. "14074 FT8", "7.055 lsb", "10489.540 MHz". Everyone else then
 * types that into the readout by hand, which on a phone is a dozen taps and a
 * mistake. Making those a link is the single highest-value thing this widget
 * can do.
 *
 * Nothing here runs on the server. A message stays text end to end - stored as
 * text, sent as text, put into a text node - and the linking is a reading of
 * that text done fresh in each listener's browser. A chat message is not
 * trusted input, and this way there is nothing to trust: the worst a hostile
 * message can do is name a frequency.
 *
 * The hard part is not finding numbers, it is not finding the wrong ones. A
 * chat window is full of numbers that are not frequencies: 73, 599, 2026, the
 * time, a signal report. Two rules keep them out.
 *
 *  1. A bare number is read the way hams say it: 1000 and over is kHz, below
 *     that is MHz. So "14074" is 14.074 MHz and "7.055" is 7.055 MHz, which is
 *     what both writers meant.
 *  2. The result has to land inside a band this receiver actually covers.
 *     That is what removes 73 and 599 and the year, and it is also honest: a
 *     link to a frequency this receiver cannot hear would be a broken promise.
 */

/** The modes the receiver has, plus the names people write for them. */
const MODE_WORDS: Record<string, string> = {
  usb: 'usb',
  lsb: 'lsb',
  cw: 'cw',
  cwl: 'cwl',
  cwu: 'cw',
  am: 'am',
  sam: 'sam',
  fm: 'nfm',
  nfm: 'nfm',
  dsb: 'dsb',
  // Digital modes are not modes the receiver has; they are a mode plus a
  // convention about where in the passband the signal sits. Naming the one
  // they are heard on is more use than ignoring the word.
  ft8: 'usb',
  ft4: 'usb',
  psk: 'usb',
  psk31: 'usb',
  rtty: 'usb',
  js8: 'usb',
  wspr: 'usb',
  sstv: 'usb',
};

export interface Spot {
  /** Where the text was, so the message can be rebuilt around it. */
  start: number;
  end: number;
  /** The text as it was written. */
  text: string;
  hz: number;
  mode?: string;
}

export interface Segment {
  text: string;
  spot?: Spot;
}

/**
 * A number, optionally with a unit, optionally with a mode either side of it.
 *
 * The mode is allowed before or after because both are written: "USB 14200"
 * and "14200 usb" are the same sentence. A separator between number and unit
 * is optional, so "14.074MHz" is read as well as "14.074 MHz".
 */
const PATTERN = new RegExp(
  String.raw`(?:\b(?<before>[a-z][a-z0-9]{1,4})\s+)?` +
    String.raw`\b(?<number>\d{1,11}(?:[.,]\d{1,6})?)\s*` +
    String.raw`(?<unit>khz|mhz|hz|k|m)?\b` +
    String.raw`(?:[\s,]*(?<after>[a-z][a-z0-9]{1,4})\b)?`,
  'gi',
);

/** Reads one matched number and unit as a frequency in Hz, or null. */
function toHz(number: string, unit: string | undefined): number | null {
  const value = Number(number.replace(',', '.'));
  if (!Number.isFinite(value) || value <= 0) return null;
  const suffix = (unit ?? '').toLowerCase();
  if (suffix === 'hz') return value;
  if (suffix === 'khz' || suffix === 'k') return value * 1e3;
  if (suffix === 'mhz' || suffix === 'm') return value * 1e6;
  // No unit: the way it is said out loud. Anything that reads as a whole
  // number of kHz is kHz; a small number is MHz.
  return value >= 1000 ? value * 1e3 : value * 1e6;
}

/**
 * Words that mean the number before them was not a frequency.
 *
 * "at 14.20 utc" reads as a time to a person and as the middle of 20 m to a
 * regular expression. Modes are not on this list even where they are also
 * English: "14200 am" in a receiver's chat is AM, not twenty past two.
 */
const NOT_A_FREQUENCY = new Set(['utc', 'gmt', 'z', 'local', 'hrs', 'hours', 'uhr']);

export interface Coverage {
  low: number;
  high: number;
}

/**
 * Finds the frequencies in a message that this receiver could tune to.
 *
 * `coverage` is the bands as the receiver reports them; with none, nothing is
 * a link, which is the right answer while the band list is still arriving.
 */
export function findSpots(text: string, coverage: Coverage[]): Spot[] {
  if (coverage.length === 0) return [];
  const spots: Spot[] = [];
  PATTERN.lastIndex = 0;
  let match: RegExpExecArray | null;
  while ((match = PATTERN.exec(text)) !== null) {
    const groups = match.groups ?? {};
    const hz = toHz(groups.number, groups.unit);
    if (hz === null) continue;
    if (!coverage.some((band) => hz >= band.low && hz <= band.high)) continue;

    if (NOT_A_FREQUENCY.has((groups.after ?? '').toLowerCase())) {
      PATTERN.lastIndex = match.index + match[0].length;
      continue;
    }
    // Inside a link, a number is part of an address. The whole token is
    // looked at rather than the character before, because the giveaway can be
    // anywhere in it: example.org/spots/14074.
    const spaceBefore = text.lastIndexOf(' ', match.index);
    const token = text.slice(spaceBefore + 1, match.index);
    if (token.includes('://') || token.includes('/') || token.includes('@')) {
      PATTERN.lastIndex = match.index + match[0].length;
      continue;
    }

    const before = MODE_WORDS[(groups.before ?? '').toLowerCase()];
    const after = MODE_WORDS[(groups.after ?? '').toLowerCase()];
    // A word before the number only counts as a mode if it is one; "on 14074"
    // must not swallow "on", and must not stop the number being found.
    let start = match.index;
    let end = match.index + match[0].length;
    if (groups.before && !before) start += match[0].indexOf(groups.number);
    if (groups.after && !after) end = match.index + match[0].lastIndexOf(groups.after);
    // Whatever is left over on the end is separator, not part of the spot: a
    // link should not carry the space after it.
    while (end > start && /[\s,]/.test(text[end - 1])) end--;

    spots.push({ start, end, text: text.slice(start, end), hz: Math.round(hz), mode: before ?? after });
    // Continue after what was taken, so a trailing word that was not a mode
    // can still begin the next match.
    PATTERN.lastIndex = end;
  }
  return spots;
}

/** The message split into plain text and the spots inside it, in order. */
export function segment(text: string, coverage: Coverage[]): Segment[] {
  const spots = findSpots(text, coverage);
  if (spots.length === 0) return [{ text }];
  const out: Segment[] = [];
  let at = 0;
  for (const spot of spots) {
    if (spot.start > at) out.push({ text: text.slice(at, spot.start) });
    out.push({ text: spot.text, spot });
    at = spot.end;
  }
  if (at < text.length) out.push({ text: text.slice(at) });
  return out;
}

/**
 * How a listener's own tuning is written when they share it.
 *
 * kHz with three decimals, which is how frequencies are quoted on the air and
 * in every cluster spot, plus the mode in the case people write it in.
 */
export function describeTuning(hz: number, mode: string): string {
  return `${(hz / 1000).toFixed(3)} kHz ${mode.toUpperCase()}`;
}
