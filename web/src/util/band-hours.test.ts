import { describe, expect, it } from 'vitest';
import { comesOnAt, comesOnShort, endingNotice, offAir, takesOver, utcTime } from './band-hours';

const six = Date.UTC(2026, 8, 29, 18, 0);
const day = { id: 'day', name: '20 m', on_air: true, next_change: six, shared_input: 'day' };
const night = { id: 'night', name: '40 m', on_air: false, next_change: six, shared_input: 'day' };
const other = { id: 'vhf', name: '2 m', on_air: true, next_change: -1 };

describe('band hours', () => {
  it('treats a receiver without hours as always on the air', () => {
    expect(offAir({})).toBe(false);
    expect(comesOnAt({ next_change: six })).toBeNull();
  });

  it('writes times in UTC, as the hours are', () => {
    expect(utcTime(six)).toBe('18:00 UTC');
    expect(comesOnAt(night)).toBe('On the air from 18:00 UTC');
    expect(comesOnAt({ on_air: false, next_change: -1 })).toBe('Off the air');
    expect(comesOnShort(night)).toBe('18:00');
    expect(comesOnShort({ on_air: false, next_change: -1 })).toBe('off');
    expect(comesOnShort(day)).toBeNull();
  });

  it('finds the band that takes the input over', () => {
    expect(takesOver(day, [day, night, other])?.id).toBe('night');
    expect(takesOver(other, [day, night, other])).toBeNull();
    // One on the same input that comes on at another time is not it.
    expect(takesOver(day, [day, { ...night, next_change: six + 3_600_000 }])).toBeNull();
  });

  it('warns in the last ten minutes only', () => {
    const bands = [day, night, other];
    expect(endingNotice(day, bands, six - 11 * 60_000)).toBeNull();
    expect(endingNotice(day, bands, six - 9 * 60_000)).toBe(
      '20 m goes off the air at 18:00 UTC; the receiver switches to 40 m and you with it.');
    expect(endingNotice({ ...day, shared_input: undefined }, bands, six - 60_000)).toBe('20 m goes off the air at 18:00 UTC.');
    expect(endingNotice(other, bands, six)).toBeNull();
    expect(endingNotice(day, bands, six + 1)).toBeNull();
    expect(endingNotice(undefined, bands, six)).toBeNull();
    // Left on a band off the air: said for as long as they stay.
    expect(endingNotice(night, bands, six - 5 * 3_600_000)).toBe('40 m is off the air until 18:00 UTC. Choose another band to listen on.');
  });
});
