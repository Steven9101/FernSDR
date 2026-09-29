import { describe, expect, it } from 'vitest';
import { regionAt } from './itu-region';

const places: [string, number, number, 1 | 2 | 3][] = [
  ['Berlin', 52.5, 13.4, 1],
  ['London', 51.5, -0.1, 1],
  ['Reykjavik', 64.1, -21.9, 1],
  ['Azores', 38.7, -27.2, 1],
  ['Cape Verde', 15.0, -23.5, 1],
  ['Johannesburg', -26.2, 28.0, 1],
  ['Moscow', 55.8, 37.6, 1],
  ['Novosibirsk', 55.0, 82.9, 1],
  ['Vladivostok', 43.1, 131.9, 1],
  ['Ulaanbaatar', 47.9, 106.9, 1],
  ['Khovd, western Mongolia', 48.0, 91.6, 1],
  ['Dalanzadgad, southern Mongolia', 43.6, 104.4, 1],
  ['Hohhot, Inner Mongolia', 40.8, 111.7, 3],
  ['Almaty', 43.2, 76.9, 1],
  ['Tashkent', 41.3, 69.2, 1],
  ['Bishkek', 42.9, 74.6, 1],
  ['Naryn', 41.4, 76.0, 1],
  ['Kashgar', 39.5, 76.0, 3],
  ['Urumqi', 43.8, 87.6, 3],
  ['Tbilisi', 41.7, 44.8, 1],
  ['Erzurum', 39.9, 41.3, 1],
  ['Riyadh', 24.7, 46.7, 1],
  ['Muscat', 23.6, 58.4, 1],
  ['New York', 40.7, -74.0, 2],
  ['Anchorage', 61.2, -149.9, 2],
  ['Adak, Aleutians', 51.9, -176.6, 2],
  ['Honolulu', 21.3, -157.9, 2],
  ['Nuuk', 64.2, -51.7, 2],
  ['Sao Paulo', -23.5, -46.6, 2],
  ['Easter Island', -27.1, -109.4, 2],
  ['Tehran', 35.7, 51.4, 3],
  ['Kabul', 34.5, 69.2, 3],
  ['Delhi', 28.6, 77.2, 3],
  ['Beijing', 39.9, 116.4, 3],
  ['Harbin', 45.8, 126.6, 3],
  ['Tokyo', 35.7, 139.7, 3],
  ['Sydney', -33.9, 151.2, 3],
  ['Auckland', -36.8, 174.8, 3],
  ['Tahiti', -17.6, -149.4, 3],
  ['Kiritimati', 1.9, -157.4, 3],
  ['Galapagos', -0.7, -90.3, 2],
  ['Fiji', -18.1, 178.4, 3],
];

describe('ITU region from a position', () => {
  it.each(places)('%s', (_, lat, lon, region) => {
    expect(regionAt(lat, lon)).toBe(region);
  });
});
