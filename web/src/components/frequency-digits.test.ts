import { describe, expect, it } from 'vitest';
import { digitsOf, isLeadingZero, placesFor } from './frequency-digits';

/** What the readout actually spells out, separators and all. */
function render(hz: number, topHz: number): string {
  const places = placesFor(topHz);
  const digits = digitsOf(hz, places);
  return places
    .map((place, index) => (place === 1e5 || place === 1e2 ? '.' : '') + digits[index])
    .join('');
}

describe('frequency readout', () => {
  it('shows a VHF frequency without truncating it', () => {
    // The bug a real off-air capture found: 145.9 MHz displayed as 45.900.000,
    // which is not a rounding error, it is a different frequency.
    expect(render(145_900_000, 146_000_000)).toBe('145.900.000');
  });

  it('does not grow a hundreds digit on an HF receiver', () => {
    expect(render(7_100_000, 30_000_000)).toBe('07.100.000');
    expect(placesFor(30_000_000)).toHaveLength(8);
  });

  it('keeps every digit down to 1 Hz', () => {
    expect(render(7_123_456, 30_000_000)).toBe('07.123.456');
    expect(render(145_123_456, 148_000_000)).toBe('145.123.456');
  });

  it('handles the extremes of each range', () => {
    expect(render(0, 30_000_000)).toBe('00.000.000');
    expect(render(99_999_999, 30_000_000)).toBe('99.999.999');
    expect(render(999_999_999, 1_000_000_000)).toBe('999.999.999');
  });

  it('adds the digit as soon as the band can reach it', () => {
    expect(placesFor(99_999_999)).toHaveLength(8);
    expect(placesFor(100_000_000)).toHaveLength(9);
  });
});

describe('leading zeros', () => {
  it('dims only the zeros before the first significant digit, never the megahertz digit', () => {
    const places = placesFor(30e6);
    const dim = (hz: number) => places.map((_, i) => isLeadingZero(digitsOf(hz, places), places, i));
    // 7.040 MHz: the tens digit is dim, the 7 is not.
    expect(dim(7_040_000).slice(0, 2)).toEqual([true, false]);
    // 14.2 MHz: nothing dim.
    expect(dim(14_200_000).slice(0, 2)).toEqual([false, false]);
    // 999 kHz: the tens zero dim, the megahertz zero lit.
    expect(dim(999_000).slice(0, 2)).toEqual([true, false]);
    // Zeros inside the number stay lit.
    expect(dim(10_000_000).every((d) => !d)).toBe(true);
  });
});
