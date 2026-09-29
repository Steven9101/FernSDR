import { describe, expect, it } from 'vitest';
import { decodeRecent } from './spectrum';

describe('decodeRecent', () => {
  it('turns the bytes back into levels, oldest line first', () => {
    const bytes = new Uint8Array([0, 255, 128, 51]);
    const lines = decodeRecent({
      width: 2,
      count: 2,
      interval_ms: 500,
      age_ms: 100,
      floor_db: -140,
      ceiling_db: -20,
      lines: btoa(String.fromCharCode(...bytes)),
    });
    expect(lines).toHaveLength(2);
    expect(lines[0][0]).toBeCloseTo(-140);
    expect(lines[0][1]).toBeCloseTo(-20);
    expect(lines[1][0]).toBeCloseTo(-140 + (128 * 120) / 255);
  });

  it('stops at the bytes it was given rather than reading past them', () => {
    const lines = decodeRecent({ width: 4, count: 3, interval_ms: 500, age_ms: 0, floor_db: -140, ceiling_db: -20, lines: btoa('abcd') });
    expect(lines).toHaveLength(1);
  });
});
