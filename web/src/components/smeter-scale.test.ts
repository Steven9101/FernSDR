import { describe, expect, it } from 'vitest';
import { BAR_TICKS, MINOR_MARKS, NEEDLE_MARKS, S9_AT, toSUnits } from './smeter-scale';

// A calibrated receiver: the level that reads as S1 is eight S-units under
// S9, which IARU R.1 puts at -73 dBm.
const S1_DBM = -73 - 8 * 6;

describe('S-meter scale', () => {
  it('reads the IARU reference levels as their S-units', () => {
    expect(toSUnits(-121, S1_DBM)).toEqual({ label: 'S1', fraction: 0 });
    expect(toSUnits(-73, S1_DBM)).toEqual({ label: 'S9', fraction: S9_AT });
    expect(toSUnits(-97, S1_DBM).label).toBe('S5');
    expect(toSUnits(-53, S1_DBM).label).toBe('S9+20');
    expect(toSUnits(-33, S1_DBM).label).toBe('S9+40');
    expect(toSUnits(-150, S1_DBM)).toEqual({ label: 'S1', fraction: 0 });
    expect(toSUnits(0, S1_DBM).fraction).toBe(1);
  });

  it('puts every reading on the mark that carries its label', () => {
    const at = (label: string) => NEEDLE_MARKS.find((m) => m.label === label)!.at;
    for (const s of [1, 3, 5, 7, 9]) expect(toSUnits(-121 + (s - 1) * 6, S1_DBM).fraction).toBeCloseTo(at(String(s)), 9);
    expect(toSUnits(-53, S1_DBM).fraction).toBeCloseTo(at('+20'), 9);
    expect(toSUnits(-33, S1_DBM).fraction).toBeCloseTo(at('+40'), 9);
    for (const s of [3, 5, 7]) expect(toSUnits(-121 + (s - 1) * 6, S1_DBM).fraction).toBeCloseTo(BAR_TICKS.find((t) => t.label === String(s))!.at, 9);
    expect(MINOR_MARKS[4]).toBeCloseTo(toSUnits(-63, S1_DBM).fraction, 9);
  });

  it('prints S9 once on the bar', () => {
    expect(BAR_TICKS.map((t) => t.label)).toEqual(['1', '3', '5', '7']);
  });
});
