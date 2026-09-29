import { describe, expect, it } from 'vitest';
import { programmeType, stationName } from './rds';

describe('rds', () => {
  it('names the programme type by the table the region uses', () => {
    expect(programmeType(10, false)).toBe('Pop music');
    expect(programmeType(10, true)).toBe('Country');
    expect(programmeType(1, true)).toBe('News');
    expect(programmeType(0, false)).toBe('');
    expect(programmeType(27, true)).toBe('');
    expect(programmeType(32, false)).toBe('');
    expect(programmeType(undefined, false)).toBe('');
  });

  it('tidies the station name', () => {
    expect(stationName(' FERN  FM')).toBe('FERN FM');
    expect(stationName('        ')).toBe('');
    expect(stationName(undefined)).toBe('');
  });
});
