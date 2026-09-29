/**
 * Bands to offer for a radio, from what its module says it can do (the
 * manifest's `tuning`) and the band plan of the station's region. The
 * operator picks from these instead of typing a centre frequency and a
 * sample rate they may not know the meaning of; a custom band stays possible.
 */

export interface Tuning {
  /** Centre frequencies the radio tunes, Hz. */
  ranges: [number, number][];
  /** Sample rates worth offering, the first the default. */
  rates: number[];
  /** A real signal covers 0 Hz to half the rate; IQ covers the rate around the centre. */
  signal: 'iq' | 'real';
}

export type SuggestionGroup = 'amateur' | 'broadcast' | 'aviation' | 'other' | 'everything';

export interface Target {
  name: string;
  group: SuggestionGroup;
  /** Hz */
  low: number;
  high: number;
}

export interface Suggestion {
  /** A band id for the configuration: letters, digits, '-', as the receiver takes them. */
  id: string;
  name: string;
  group: SuggestionGroup;
  center: number;
  rate: number;
  signal: 'iq' | 'real';
  /** What the band shows, Hz. */
  low: number;
  high: number;
  /** Whether that is all of the target, or a slice of it. */
  whole: boolean;
}

/** The share of the sample rate a band shows; the receiver's usable_fraction. */
export const USABLE = 0.8;

/** A band id from a name: "40 m" becomes "40m", "Airband (AM)" "airband-am". */
export function bandId(name: string): string {
  const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(\d)-m\b/g, '$1m').replace(/^-+|-+$/g, '');
  return id.slice(0, 48) || 'band';
}

/** `id`, or `id-2`, `id-3` and so on, whichever the configuration does not have yet. */
export function freeId(id: string, taken: readonly string[]): string {
  if (!taken.includes(id)) return id;
  for (let n = 2; ; n++) if (!taken.includes(`${id}-${n}`)) return `${id}-${n}`;
}

const MHZ = 1e6;

function covered(tuning: Tuning, hz: number): boolean {
  return tuning.ranges.some(([low, high]) => hz >= low && hz <= high);
}

/**
 * The suggestion for one target on an IQ radio: the smallest offered rate
 * that shows all of it, or the widest rate centred on it when none does.
 * Null when the radio does not tune there at all.
 */
export function fit(tuning: Tuning, target: Target): Suggestion | null {
  if (tuning.signal !== 'iq') return null;
  const middle = (target.low + target.high) / 2;
  if (!covered(tuning, middle)) return null;
  const width = target.high - target.low;
  const rates = [...tuning.rates].sort((a, b) => a - b);
  const rate = rates.find((r) => r * USABLE >= width) ?? rates[rates.length - 1];
  const half = (rate * USABLE) / 2;
  // Whole: centred. A slice: from the target's bottom, where the calling
  // frequencies and beacons of most bands are, as far as the rate reaches.
  const whole = rate * USABLE >= width;
  let center = whole ? middle : target.low + half;
  // Rounded to 5 kHz, so the configuration reads like a radio's dial.
  center = Math.round(center / 5000) * 5000;
  return {
    id: bandId(target.name),
    name: target.name,
    group: target.group,
    center,
    rate,
    signal: 'iq',
    low: Math.max(target.low, center - half),
    high: Math.min(target.high, center + half),
    whole,
  };
}

/**
 * What a radio with a real signal offers: everything from 0 Hz up, once for
 * each rate, because there is nothing to centre.
 */
export function everything(tuning: Tuning): Suggestion[] {
  if (tuning.signal !== 'real') return [];
  return [...tuning.rates].sort((a, b) => a - b).map((rate) => {
    // Kept a little inside half the rate, where the converter's filter still is flat.
    const top = Math.floor((rate / 2) * 0.93 / MHZ) * MHZ;
    const name = top <= 32 * MHZ ? `Shortwave, 0 to ${top / MHZ} MHz` : `0 to ${top / MHZ} MHz, with 6 m`;
    return { id: bandId(name), name, group: 'everything' as const, center: 0, rate, signal: 'real' as const, low: 0, high: top, whole: true };
  });
}

/** The service allocations worth offering, by the plan's labels. */
const SERVICES: [RegExp, SuggestionGroup, string?][] = [
  [/^\d+ m (broadcast|tropical)$/, 'broadcast'],
  [/^MW broadcast$/, 'broadcast', 'Medium wave'],
  [/^LW broadcast$/, 'broadcast', 'Long wave'],
  [/^FM broadcast$/, 'broadcast'],
  [/^Airband \(AM\)$/, 'aviation', 'Airband'],
  [/^Marine VHF$/, 'aviation'],
  [/^Weather and other satellites$/, 'other', 'Weather satellites'],
  [/^Weather radio$/, 'other'],
  [/^PMR446$/, 'other'],
  [/^CB$/, 'other', 'CB radio'],
  [/^SRD 433 MHz/, 'other', '433 MHz devices'],
];

/**
 * The targets in a band plan: its amateur bands, and the broadcast,
 * aviation and other services above. `bands` and `segments` are the
 * resolved plan's (Hz).
 */
export function targetsFrom(
  bands: readonly { name: string; low: number; high: number }[],
  segments: readonly { label: string; low: number; high: number; layer: string }[],
): Target[] {
  const out: Target[] = bands.map((band) => ({ name: band.name, group: 'amateur' as const, low: band.low, high: band.high }));
  const seen = new Set<string>();
  for (const segment of segments) {
    if (segment.layer !== 'service') continue;
    const match = SERVICES.find(([pattern]) => pattern.test(segment.label));
    if (!match || seen.has(segment.label)) continue;
    seen.add(segment.label);
    out.push({ name: match[2] ?? segment.label, group: match[1], low: segment.low, high: segment.high });
  }
  return out;
}

/** Every suggestion for a radio, in the order of their frequencies, everything first. */
export function suggestionsFor(tuning: Tuning, targets: readonly Target[]): Suggestion[] {
  const fitted = targets.map((target) => fit(tuning, target)).filter((s): s is Suggestion => s !== null);
  fitted.sort((a, b) => a.low - b.low);
  return [...everything(tuning), ...fitted];
}

/**
 * Hours for bands that share one radio: the lower frequencies at night,
 * where they carry, and the higher by day. Two bands only; more share by
 * hours the operator sets.
 */
export function dayAndNight(a: Suggestion, b: Suggestion): [string, string] {
  return a.center < b.center ? ['sunset-sunrise', 'sunrise-sunset'] : ['sunrise-sunset', 'sunset-sunrise'];
}

/** The configuration section for a band on a module's radio. */
export function bandSection(
  suggestion: Suggestion,
  module: string,
  device: string,
  hours?: string,
): Map<string, string> {
  const values = new Map<string, string>([
    ['name', suggestion.name],
    ['source', 'module'],
    ['module', module],
    ['sample_rate', String(suggestion.rate)],
    ['center', String(suggestion.center)],
    ['signal', suggestion.signal],
  ]);
  if (suggestion.signal === 'real') {
    values.set('low', '0');
    values.set('high', String(suggestion.high));
  }
  if (device) values.set('module.device', device);
  // A zero-IF radio has a spike at its centre; the RTL-SDR's is the one
  // everybody notices.
  if (module === 'rtlsdr') values.set('dc_remove', 'yes');
  if (hours) values.set('hours', hours);
  return values;
}
