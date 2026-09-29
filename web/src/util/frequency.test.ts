import { describe, expect, it } from 'vitest';
import { formatBitrate, formatSpan, parseFrequency } from './frequency';

describe('parseFrequency', () => {
  it('reads the units people actually type', () => {
    expect(parseFrequency('14.074')).toBe(14_074_000);
    expect(parseFrequency('14074')).toBe(14_074_000);
    expect(parseFrequency('7.1M')).toBe(7_100_000);
    expect(parseFrequency('7100 kHz')).toBe(7_100_000);
    expect(parseFrequency('3690000')).toBe(3_690_000);
    expect(parseFrequency('500Hz')).toBe(500);
    expect(parseFrequency(' 21.2 ')).toBe(21_200_000);
  });

  it('treats a bare number as whichever unit makes it a real frequency', () => {
    // Both of these obviously mean 40 m to the person typing them.
    expect(parseFrequency('7.1')).toBe(parseFrequency('7100'));
    expect(parseFrequency('14.074')).toBe(parseFrequency('14074'));
  });

  it('rejects nonsense rather than tuning somewhere surprising', () => {
    expect(parseFrequency('')).toBeNull();
    expect(parseFrequency('abc')).toBeNull();
    expect(parseFrequency('14.0.7')).toBeNull();
    expect(parseFrequency('7.1 furlongs')).toBeNull();
    expect(parseFrequency('--5')).toBeNull();
  });
});

describe('formatSpan', () => {
  it('picks a readable unit', () => {
    expect(formatSpan(500)).toBe('500 Hz');
    expect(formatSpan(12_000)).toBe('12.0 kHz');
    expect(formatSpan(150_000)).toBe('150 kHz');
    expect(formatSpan(2_400_000)).toBe('2.40 MHz');
  });
});

describe('formatBitrate', () => {
  it('reports kilobits for the rates this system actually uses', () => {
    expect(formatBitrate(48_000)).toBe('48.0 kbit/s');
    expect(formatBitrate(1_250_000)).toBe('1.25 Mbit/s');
  });
});
