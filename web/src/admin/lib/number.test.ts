import { describe, expect, it } from 'vitest';
import { typedNumber } from './number';

describe('typedNumber', () => {
  it('reads what a person typed', () => {
    expect(typedNumber('38.6')).toBe(38.6);
    expect(typedNumber(' -73 ')).toBe(-73);
    expect(typedNumber(0)).toBe(0);
    expect(typedNumber(12)).toBe(12);
  });

  it('has no number for an empty or cleared field, rather than zero', () => {
    expect(typedNumber(null)).toBeNull();
    expect(typedNumber(undefined)).toBeNull();
    expect(typedNumber('')).toBeNull();
    expect(typedNumber('   ')).toBeNull();
  });

  it('has no number for text that is not one', () => {
    expect(typedNumber('auto')).toBeNull();
    expect(typedNumber(Number.NaN)).toBeNull();
    expect(typedNumber(Number.POSITIVE_INFINITY)).toBeNull();
    expect(typedNumber(true)).toBeNull();
  });
});
