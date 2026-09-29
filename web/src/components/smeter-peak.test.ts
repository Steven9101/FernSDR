import { describe, expect, it } from 'vitest';
import { PEAK_FALL_DB_PER_SECOND, nextPeak } from './smeter-peak';

describe('the peak marker', () => {
  it('never sits below the level', () => {
    expect(nextPeak(-100, -40, 1)).toBe(-40);
    expect(nextPeak(-40, -40, 10)).toBe(-40);
  });

  it('falls at the rate the scale asks for', () => {
    // A report is quoted in S-units, so a burst should still be within one of
    // its peak a second later: 6 dB is the ceiling. A marker that takes more
    // than about ten seconds to cross the S1 to S9+40 scale, which is 88 dB,
    // reads as stuck: 9 dB is the floor. Eight sits between them.
    expect(PEAK_FALL_DB_PER_SECOND).toBeGreaterThanOrEqual(6);
    expect(PEAK_FALL_DB_PER_SECOND).toBeLessThanOrEqual(9);

    // One second after a 40 dB burst, still inside the same S-unit and a bit.
    let peak = -30;
    for (let i = 0; i < 10; i++) peak = nextPeak(peak, -70, 0.1);
    expect(peak).toBeCloseTo(-38, 1);
    // And across the whole scale in about eleven seconds.
    expect(88 / PEAK_FALL_DB_PER_SECOND).toBeCloseTo(11, 0);
  });

  it('clamps a gap in the readings', () => {
    // Telemetry stops while a band rebuilds its channel. Treating the pause as
    // elapsed time drops the marker the whole way in one step, which looks
    // exactly like the hold not working - and did, when it was measured
    // through a retune.
    expect(nextPeak(-30, -90, 6)).toBe(-34);
    expect(nextPeak(-30, -90, 0.1)).toBeCloseTo(-30.8, 5);
  });
});
