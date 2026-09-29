import { describe, expect, it } from 'vitest';
import { locatorCentre, locatorFor } from './locator';

describe('locatorCentre', () => {
  it('finds the middle of a square', () => {
    // JO31: 6 to 8 degrees east, 51 to 52 north.
    expect(locatorCentre('JO31')).toEqual({ lat: 51.5, lon: 7 });
    // DN41: Northern Utah, 112 to 110 west, 41 to 42 north.
    expect(locatorCentre('dn41')).toEqual({ lat: 41.5, lon: -111 });
    // JO31ne: a subsquare, 5 by 2.5 minutes.
    const sub = locatorCentre('JO31ne')!;
    expect(sub.lon).toBeCloseTo(6 + 13 * 5 / 60 + 2.5 / 60, 9);
    expect(sub.lat).toBeCloseTo(51 + 4 * 2.5 / 60 + 1.25 / 60, 9);
  });

  it('refuses what is not a locator', () => {
    for (const bad of ['', 'JO3', 'ZZ99', 'JO31zz', 'Bremen', undefined]) expect(locatorCentre(bad)).toBeNull();
  });
});

describe('locatorFor', () => {
  it('names the square a place is in', () => {
    expect(locatorFor(52.52, 13.4)).toBe('JO62qm');
    expect(locatorFor(41.5, -111.9, 4)).toBe('DN41');
    expect(locatorFor(-33.87, 151.21)).toBe('QF56od');
    expect(locatorFor(90, 180)).toBe('RR99xx');
    expect(locatorFor(-90, -180)).toBe('AA00aa');
  });

  it('comes back to the square whose centre it was given', () => {
    for (const grid of ['JO31ne', 'DN41xa', 'QF56od', 'AA00aa', 'RR99xx', 'IO91wm']) {
      const centre = locatorCentre(grid)!;
      expect(locatorFor(centre.lat, centre.lon)).toBe(grid.slice(0, 4) + grid.slice(4).toLowerCase());
    }
  });
});
