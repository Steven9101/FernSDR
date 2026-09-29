import { expect, it } from 'vitest';
import { waterfallLevels } from './waterfall-levels';

it('retains the sorted percentile levels across narrow and wide spectra', () => {
  const counts = new Uint32Array(301);
  let seed = 7;
  for (const width of [16, 64, 512, 1024, 4096]) {
    for (let line = 0; line < 10; line++) {
      const levels = Float32Array.from({ length: width }, () => {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return seed % 301 - 200;
      });
      const sorted = levels.slice().sort();
      expect(waterfallLevels(levels, counts)).toEqual([
        sorted[Math.floor((width - 1) * 0.25)], sorted[Math.floor((width - 1) * 0.995)],
      ]);
    }
  }
  expect(waterfallLevels(new Float32Array(4096).fill(-103), counts)).toEqual([-103, -103]);
});

it('counts fractional interpolated levels at the same 1 dB precision', () => {
  const counts = new Uint32Array(301);
  const values = Float32Array.from([-110.2, -100.8, -95.1, -90.8, -80.1, -70.9, -50.1, -40.1]);
  expect(waterfallLevels(values, counts)).toEqual([-101, -50]);
  expect(counts.reduce((a, b) => a + b, 0)).toBe(values.length);
});
