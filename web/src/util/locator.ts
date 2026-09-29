/**
 * The centre of a Maidenhead locator square, for pointing a map at the
 * receiver: JO31 is a square of 2 by 1 degrees, JO31ne one of 5 by 2.5
 * minutes. Null for anything that is not a locator of 4 or 6 characters.
 */
export function locatorCentre(grid: string | undefined): { lat: number; lon: number } | null {
  const text = (grid ?? '').trim().toUpperCase();
  if (!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(text)) return null;
  let lon = (text.charCodeAt(0) - 65) * 20 - 180 + Number(text[2]) * 2;
  let lat = (text.charCodeAt(1) - 65) * 10 - 90 + Number(text[3]);
  if (text.length === 6) {
    lon += (text.charCodeAt(4) - 65) * (5 / 60) + 2.5 / 60;
    lat += (text.charCodeAt(5) - 65) * (2.5 / 60) + 1.25 / 60;
  } else {
    lon += 1;
    lat += 0.5;
  }
  return { lat, lon };
}

/**
 * The locator of a place, four or six characters, the subsquare in lower
 * case as it is usually written: 52.52 N, 13.40 E is JO62qm. Latitude 90 and
 * longitude 180 fall in the last square rather than off the grid.
 */
export function locatorFor(lat: number, lon: number, length: 4 | 6 = 6): string {
  const x = Math.min(Math.max(lon + 180, 0), 360 - 1e-9);
  const y = Math.min(Math.max(lat + 90, 0), 180 - 1e-9);
  const field = String.fromCharCode(65 + Math.floor(x / 20), 65 + Math.floor(y / 10));
  const square = `${Math.floor((x % 20) / 2)}${Math.floor(y % 10)}`;
  if (length === 4) return field + square;
  const sub = String.fromCharCode(97 + Math.floor(((x % 2) * 60) / 5), 97 + Math.floor(((y % 1) * 60) / 2.5));
  return field + square + sub;
}
