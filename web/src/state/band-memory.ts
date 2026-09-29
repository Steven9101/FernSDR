import type { BandDescription } from '../net/protocol';
import type { Tuning, Viewport } from './store';
import { constrainViewport } from '../util/viewport';

interface BandMemory {
  tuning: Tuning;
  lowHz: number;
  highHz: number;
}

const MODES = new Set(['usb', 'lsb', 'cw', 'cwl', 'am', 'sam', 'nfm', 'dsb', 'wfm']);

export class BandMemories {
  private entries = new Map<string, BandMemory>();

  remember(tuning: Tuning, viewport: Pick<Viewport, 'lowHz' | 'highHz'>): void {
    if (!tuning.band || viewport.highHz <= viewport.lowHz) return;
    this.entries.delete(tuning.band);
    this.entries.set(tuning.band, { tuning: { ...tuning }, lowHz: viewport.lowHz, highHz: viewport.highHz });
    while (this.entries.size > 64) this.entries.delete(this.entries.keys().next().value!);
  }

  recall(band: BandDescription): BandMemory | undefined {
    const saved = this.entries.get(band.id);
    if (!saved) return;
    const range = constrainViewport(saved.lowHz, saved.highHz, band);
    if (!range) return;
    return { tuning: { ...saved.tuning, freq: Math.max(band.low, Math.min(band.high, saved.tuning.freq)) }, ...range };
  }

  last(id: string): BandMemory | undefined { return this.entries.get(id); }

  snapshot(): BandMemory[] { return [...this.entries.values()]; }

  load(value: unknown): void {
    this.entries.clear();
    if (!Array.isArray(value)) return;
    for (const saved of value.slice(-64)) {
      const tune = saved?.tuning;
      if (!tune || typeof tune.band !== 'string' || tune.band.length > 200 || !MODES.has(tune.mode) ||
          ![tune.freq, tune.low, tune.high, tune.cwPitch, saved.lowHz, saved.highHz].every(Number.isFinite) ||
          tune.freq < 0 || Math.abs(tune.low) > 100_000 || Math.abs(tune.high) > 100_000 ||
          tune.low >= tune.high || tune.cwPitch < 200 || tune.cwPitch > 1500 || saved.highHz <= saved.lowHz) continue;
      this.remember({ band: tune.band, freq: tune.freq, mode: tune.mode, low: tune.low,
        high: tune.high, cwPitch: tune.cwPitch }, saved);
    }
  }
}
