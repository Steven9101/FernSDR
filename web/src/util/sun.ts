/**
 * Where the Sun is: the point it stands overhead, its elevation anywhere,
 * and a station's sunrise and sunset. NOAA's low-precision formulae, good to
 * a fraction of a degree, which is a pixel on any map this draws. The
 * receiver works out the same for its band outlook (space_weather.cpp).
 */
const RAD = Math.PI / 180;

/** The subsolar point in degrees. */
export function subsolarPoint(ms: number): { lat: number; lon: number } {
  const n = ms / 86_400_000 + 2440587.5 - 2451545.0;
  const meanLongitude = (280.46 + 0.9856474 * n) % 360;
  const anomaly = ((357.528 + 0.9856003 * n) % 360) * RAD;
  const ecliptic = (meanLongitude + 1.915 * Math.sin(anomaly) + 0.02 * Math.sin(2 * anomaly)) * RAD;
  const obliquity = (23.439 - 0.0000004 * n) * RAD;
  const rightAscension = Math.atan2(Math.cos(obliquity) * Math.sin(ecliptic), Math.cos(ecliptic)) / RAD;
  const declination = Math.asin(Math.sin(obliquity) * Math.sin(ecliptic)) / RAD;
  const sidereal = (((18.697374558 + 24.06570982441908 * n) % 24) + 24) % 24;
  let lon = rightAscension - sidereal * 15;
  lon = ((((lon + 180) % 360) + 360) % 360) - 180;
  return { lat: declination, lon };
}

/** The Sun's elevation in degrees at a place, given the subsolar point. */
export function elevation(lat: number, lon: number, sun: { lat: number; lon: number }): number {
  return (
    Math.asin(
      Math.sin(lat * RAD) * Math.sin(sun.lat * RAD) +
        Math.cos(lat * RAD) * Math.cos(sun.lat * RAD) * Math.cos((lon - sun.lon) * RAD),
    ) / RAD
  );
}

/**
 * The UTC day's sunrise and sunset at a place, to the minute, as
 * milliseconds; null for either where the Sun does not cross the horizon
 * that day (polar day or night).
 */
export function sunriseSunset(lat: number, lon: number, dayMs: number): { rise: number | null; set: number | null } {
  const start = Math.floor(dayMs / 86_400_000) * 86_400_000;
  let rise: number | null = null;
  let set: number | null = null;
  let before = elevation(lat, lon, subsolarPoint(start));
  for (let minute = 1; minute <= 1440; minute++) {
    const at = start + minute * 60_000;
    // -0.833 degrees: the Sun's radius and refraction, as sunrise is defined.
    const now = elevation(lat, lon, subsolarPoint(at));
    if (before < -0.833 && now >= -0.833 && rise === null) rise = at;
    if (before >= -0.833 && now < -0.833 && set === null) set = at;
    before = now;
  }
  return { rise, set };
}
