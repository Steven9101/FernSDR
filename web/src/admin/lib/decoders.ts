/*
What the Decoders page needs beyond the API: where the digital modes sit on each band, which of
those a receiver's bands can hear, and the [decoder:<id>] section a form turns into.
*/

/** Dial frequencies in Hz by mode, from the IARU and ARRL band plans. */
export const DIALS: Record<string, number[]> = {
  ft8: [1_840_000, 3_573_000, 5_357_000, 7_074_000, 10_136_000, 14_074_000, 18_100_000, 21_074_000, 24_915_000,
    28_074_000, 50_313_000, 144_174_000],
  ft4: [3_575_000, 7_047_500, 10_140_000, 14_080_000, 18_104_000, 21_140_000, 24_919_000, 28_180_000, 50_318_000,
    144_170_000],
};

export interface Channel {
  band: string;
  dial: number;
}

export interface BandSpan {
  id: string;
  name: string;
  low: number;
  high: number;
}

/**
 * The dials of `mode` a band covers with room for a channel of `width` Hz starting at the dial,
 * which is what the receiver checks before it accepts the section.
 */
export function suggestedChannels(bands: BandSpan[], mode: string, width = 4000): Channel[] {
  const out: Channel[] = [];
  for (const dial of DIALS[mode] ?? []) {
    const band = bands.find((b) => dial >= b.low && dial + width <= b.high);
    if (band) out.push({ band: band.id, dial });
  }
  return out;
}

/** A frequency as the receiver reads it: Hz, or with k, M or G. */
export function parseFrequency(text: string): number | null {
  const match = /^\s*(\d+(?:\.\d+)?)\s*([kKmMgG]?)(?:[hH][zZ])?\s*$/.exec(text);
  if (!match) return null;
  const scale = { '': 1, k: 1e3, m: 1e6, g: 1e9 }[match[2].toLowerCase() as '' | 'k' | 'm' | 'g'];
  return Math.round(Number(match[1]) * scale);
}

/** `channels = ` as written, or null where an entry is not `<band>:<frequency>`. */
export function parseChannels(text: string): Channel[] | null {
  const out: Channel[] = [];
  for (const token of text.split(/[\s,]+/).filter(Boolean)) {
    const colon = token.indexOf(':');
    const dial = colon > 0 ? parseFrequency(token.slice(colon + 1)) : null;
    if (dial === null) return null;
    out.push({ band: token.slice(0, colon), dial });
  }
  return out;
}

export const channelKey = (channel: Channel) => `${channel.band}:${channel.dial}`;

export function formatDial(hz: number): string {
  const khz = hz / 1000;
  return `${Number.isInteger(khz) ? khz.toLocaleString('en-US') : khz.toLocaleString('en-US', { maximumFractionDigits: 1 })} kHz`;
}

/** A decoder's id: what [decoder:<id>] allows. */
export const validDecoderId = (id: string) => /^[a-z0-9-]{1,32}$/.test(id);

export interface DecoderForm {
  module: string;
  channels: Channel[];
  public: boolean;
}

/**
 * The section for `form`, keeping every key the form does not own (mode, width, offset, the
 * module's own settings) as it was.
 */
export function decoderSection(form: DecoderForm, previous: Map<string, string> | null): Map<string, string> {
  const values = new Map<string, string>();
  values.set('module', form.module);
  values.set('channels', form.channels.map(channelKey).join(' '));
  values.set('public', form.public ? 'yes' : 'no');
  for (const [key, value] of previous ?? []) {
    if (!values.has(key)) values.set(key, value);
  }
  return values;
}

/** A module setting as its manifest describes it. */
export interface SettingSpec {
  key: string;
  type: 'string' | 'number' | 'boolean' | 'choice';
  label: string;
  min?: number;
  max?: number;
  choices?: string[];
  default?: string | number | boolean;
  /** One line on what it does, shown under it. */
  help?: string;
}

/**
 * `values` with the module's settings as `module.<key>`: a value the operator chose is written,
 * one left at the module's default or empty is left out, so the file keeps only what differs.
 * Throws with a sentence for a value the setting cannot take.
 */
export function withModuleSettings(values: Map<string, string>, specs: SettingSpec[],
                                   chosen: Record<string, string>): Map<string, string> {
  const out = new Map(values);
  for (const spec of specs) {
    const key = `module.${spec.key}`;
    const text = (chosen[spec.key] ?? '').trim();
    if (spec.type === 'number' && text !== '') {
      const n = Number(text);
      if (!Number.isFinite(n) || (spec.min !== undefined && n < spec.min) || (spec.max !== undefined && n > spec.max)) {
        throw new Error(`${spec.label} goes from ${spec.min ?? 'any'} to ${spec.max ?? 'any'}.`);
      }
    }
    if (spec.type === 'choice' && text !== '' && !(spec.choices ?? []).includes(text)) {
      throw new Error(`${spec.label} is one of ${(spec.choices ?? []).join(', ')}.`);
    }
    if (text === '' || (spec.default !== undefined && text === String(spec.default))) out.delete(key);
    else out.set(key, text);
  }
  return out;
}
