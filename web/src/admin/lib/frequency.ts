import { readKey } from './ini';

/*
Correcting a band's frequency axis against a carrier whose frequency is known.

One crystal clocks both the tuner and the converter in most receivers, so an error in it
stretches every frequency on the radio's side of any upconverter by the same factor, 1 + ppm/1e6.
A carrier really at `truth` that the band shows at `shown`, with the correction `ppm` already in
force, therefore needs the factor scaled by (truth - offset) / (shown - offset).
*/

export function correctedPpm(truth: number, shown: number, ppm: number, offset: number): number {
  const now = 1 + ppm * 1e-6;
  return (now * ((truth - offset) / (shown - offset)) - 1) * 1e6;
}

/** The mean of the readings and how far they spread, both in hertz. */
export function summarise(readings: number[]): { mean: number; spread: number } {
  const mean = readings.reduce((sum, value) => sum + value, 0) / readings.length;
  const spread = Math.sqrt(readings.reduce((sum, value) => sum + (value - mean) ** 2, 0) / readings.length);
  return { mean, spread };
}

/** The furthest a crystal can plausibly be off; the server holds `ppm` to it as well. */
export const MAX_PPM = 500;

const UNITS: Record<string, number> = { k: 1e3, m: 1e6, g: 1e9, h: 1 };

/** A number as the server's configuration reads one, "7.1M" and "14074 kHz" included; null if it cannot. */
export function configNumber(text: string): number | null {
  const match = /^\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)\s*(.*?)\s*$/.exec(text);
  if (!match) return null;
  const value = Number(match[1]);
  if (match[2] === '') return value;
  const unit = UNITS[match[2][0].toLowerCase()];
  return unit === undefined ? null : value * unit;
}

/**
 * Why a correction worked out now must not be written, or null. The readings are taken on the
 * axis in force, so they are only right against what the receiver runs with; a correction the
 * configuration holds but the receiver has not started with yet would be counted twice, once as
 * written and once in whatever is worked out and written beside it.
 */
export function pendingCorrection(
  text: string,
  bandId: string,
  running: { ppm: number; offset: number }
): string | null {
  // Read as the server reads them: what it cannot read counts as 0, and ppm is held to MAX_PPM.
  const keys = [
    { key: 'ppm', running: running.ppm, tolerance: 0.005, digits: 2, limit: MAX_PPM },
    { key: 'frequency_offset', running: running.offset, tolerance: 0.05, digits: 1, limit: Infinity }
  ];
  for (const { key, running: value, tolerance, digits, limit } of keys) {
    const configured = readKey(text, `band:${bandId}`, key);
    const read = configured === null ? 0 : (configNumber(configured) ?? 0);
    const number = Number.isFinite(read) ? Math.min(limit, Math.max(-limit, read)) : 0;
    if (Math.abs(number - value) <= tolerance) continue;
    return (
      `The configuration already sets ${key} = ${configured ?? '0'} for this band, and the receiver still runs ` +
      `with ${value.toFixed(digits)}. Restart the receiver so that applies, then measure again.`
    );
  }
  return null;
}
