import { describe, expect, it } from 'vitest';
import { BandMemories } from './band-memory';
import type { BandDescription } from '../net/protocol';
import type { Tuning } from './store';

const tune: Tuning = { band: '20m', freq: 14_215_000, mode: 'usb', low: 300, high: 2700, cwPitch: 700 };
const band = { id: '20m', low: 14_000_000, high: 14_350_000 } as BandDescription;
const view = { lowHz: 14_200_000, highHz: 14_230_000 };

describe('band memories', () => {
  it('restores a saved narrow view and filter after serialisation', () => {
    const memory = new BandMemories();
    memory.remember(tune, view);
    const reloaded = new BandMemories();
    reloaded.load(JSON.parse(JSON.stringify(memory.snapshot())));
    expect(reloaded.recall(band)).toEqual({ tuning: tune, ...view });
  });

  it('clamps memories when an operator changes the receiver coverage', () => {
    const memory = new BandMemories();
    memory.remember(tune, view);
    const smaller = { ...band, low: 14_240_000, high: 14_250_000 };
    expect(memory.recall(smaller)).toMatchObject({ tuning: { freq: smaller.low }, lowHz: smaller.low, highHz: smaller.high });
  });

  it('ignores corrupt stored entries and bounds the number retained', () => {
    const memory = new BandMemories();
    memory.load([null, 1, {}, { tuning: { ...tune, freq: null }, ...view },
      { tuning: { ...tune, mode: 'invalid' }, ...view }, { tuning: tune, ...view }]);
    expect(memory.snapshot()).toHaveLength(1);
    for (let i = 0; i < 100; i++) memory.remember({ ...tune, band: String(i) }, view);
    expect(memory.snapshot()).toHaveLength(64);
    expect(memory.last('20m')).toBeUndefined();
    expect(memory.last('99')).toBeDefined();
  });
});
