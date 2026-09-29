import { describe, expect, it } from 'vitest';
import { dbfs, share, signed } from './format';

describe('signed', () => {
  it('uses a minus sign, not a hyphen', () => {
    expect(signed(-84.83)).toBe('−84.8');
    expect(signed(-84.83)).not.toContain('-');
  });

  it('shows a plus only when asked', () => {
    expect(signed(3.2)).toBe('3.2');
    expect(signed(3.2, 1, true)).toBe('+3.2');
  });

  it('does not sign a value that rounds to zero', () => {
    expect(signed(-0.04)).toBe('0.0');
    expect(signed(0.04, 1, true)).toBe('0.0');
  });
});

describe('dbfs', () => {
  it('rounds to the digits asked for', () => {
    expect(dbfs(-84.83)).toBe('−84.8 dBFS');
    expect(dbfs(-84.83, 0)).toBe('−85 dBFS');
  });
});

describe('share', () => {
  it('keeps two significant digits of a small share', () => {
    expect(share(0.00012)).toBe('0.012%');
    expect(share(0.0034)).toBe('0.34%');
    expect(share(0.1)).toBe('10%');
    expect(share(1)).toBe('100%');
  });
  it('says 0% for none', () => {
    expect(share(0)).toBe('0%');
  });
});
