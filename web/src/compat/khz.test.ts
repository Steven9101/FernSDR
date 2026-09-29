import { describe, expect, it } from 'vitest';
import { formatKhz, parseKhz } from './khz';

describe('kHz as a WebSDR page carries it', () => {
  it('reads what such a program writes, a decimal comma included', () => {
    expect(parseKhz('7074')).toBe(7_074_000);
    expect(parseKhz(' 7040,5 ')).toBe(7_040_500);
    expect(parseKhz('14074.000')).toBe(14_074_000);
    expect(parseKhz('')).toBeNull();
    expect(parseKhz('abc')).toBeNull();
    expect(parseKhz('-5')).toBeNull();
    expect(parseKhz(undefined)).toBeNull();
  });

  it('writes two decimals, as the field shows it', () => {
    expect(formatKhz(7_039_620)).toBe('7039.62');
    expect(formatKhz(14_074_000)).toBe('14074.00');
  });
});
