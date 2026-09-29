import { describe, expect, it } from 'vitest';
import { bandRank, receiverLabel } from './receivers';

describe('band labels and search', () => {
  it('names what a band listens with', () => {
    expect(receiverLabel('rx888')).toBe('RX-888');
    expect(receiverLabel('stdin')).toBe('Stream');
    expect(receiverLabel('fern-kiwi')).toBe('KIWI');
    expect(receiverLabel(undefined)).toBeNull();
  });

  it('finds a band by name, label or a frequency inside it', () => {
    const band = { name: '40 m', low: 7_000_000, high: 7_200_000, receiver: 'rtlsdr' };
    expect(bandRank('40', band)).toBe(1);
    expect(bandRank('rtl', band)).toBe(2);
    expect(bandRank('7.1', band)).toBe(3);
    expect(bandRank('7,15', band)).toBe(3);
    expect(bandRank('14.2', band)).toBeNull();
    expect(bandRank('', band)).toBe(0);
  });

  it('puts 2 m before 12 m for "2 m"', () => {
    const two = { name: '2 m', low: 144e6, high: 146e6 };
    const twelve = { name: '12 m', low: 24.89e6, high: 24.99e6 };
    expect(bandRank('2 m', two)).toBeLessThan(bandRank('2 m', twelve)!);
  });
});
