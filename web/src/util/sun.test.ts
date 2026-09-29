import { describe, expect, it } from 'vitest';
import { elevation, subsolarPoint, sunriseSunset } from './sun';

describe('sun', () => {
  it('stands over the Tropic of Cancer at the June solstice, near Greenwich at noon', () => {
    const sun = subsolarPoint(Date.UTC(2026, 5, 21, 12, 0));
    expect(sun.lat).toBeCloseTo(23.44, 0);
    expect(Math.abs(sun.lon)).toBeLessThan(2);
  });

  it('is up over Europe at noon and down at midnight', () => {
    expect(elevation(51.5, 7, subsolarPoint(Date.UTC(2026, 8, 28, 11, 0)))).toBeGreaterThan(30);
    expect(elevation(51.5, 7, subsolarPoint(Date.UTC(2026, 8, 28, 23, 0)))).toBeLessThan(-20);
  });

  it('rises and sets at the times an almanac gives', () => {
    // Bonn, 28 September 2026: sunrise about 05:30 UTC, sunset about 17:20 UTC.
    const { rise, set } = sunriseSunset(50.73, 7.1, Date.UTC(2026, 8, 28));
    expect(Math.abs(rise! - Date.UTC(2026, 8, 28, 5, 31)) / 60_000).toBeLessThan(8);
    expect(Math.abs(set! - Date.UTC(2026, 8, 28, 17, 19)) / 60_000).toBeLessThan(8);
    // Tromso in December: no sunrise at all.
    expect(sunriseSunset(69.6, 19, Date.UTC(2026, 11, 21)).rise).toBeNull();
  });
});
