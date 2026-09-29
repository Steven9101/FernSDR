/**
 * Which ITU region a place is in, for a band plan that follows the station
 * without the operator having to know. From the lines in the Radio
 * Regulations (RR 5.3 to 5.9, as amended at WRC-23):
 *
 * - Line A, between Regions 1 and 3: meridian 40 E from the pole to 40 N,
 *   an arc to 60 E on the Tropic of Cancer, then meridian 60 E south.
 * - Line B, between Regions 1 and 2: meridian 10 W from the pole to 72 N,
 *   arcs to 50 W 40 N and to 20 W 10 S, then meridian 20 W south.
 * - Line C, between Regions 2 and 3: from the pole to the Bering Strait at
 *   65.5 N, arcs to 165 E 50 N and to 170 W 10 N, along 10 N to 120 W, then
 *   meridian 120 W south.
 *
 * The regulations also put whole countries on the other side of a line: the
 * former Soviet states, Mongolia and Turkey east of line A are Region 1, and
 * since WRC-23 all of Iran is Region 3. Those are boxes here, good to a grid
 * square or so; near a border the operator picks the plan instead, which is
 * why the admin panel shows what "auto" worked out.
 */

export type ItuRegion = 1 | 2 | 3;

type Vec = [number, number, number];

function vector(lat: number, lon: number): Vec {
  const φ = (lat * Math.PI) / 180;
  const λ = (lon * Math.PI) / 180;
  return [Math.cos(φ) * Math.cos(λ), Math.cos(φ) * Math.sin(λ), Math.sin(φ)];
}

function cross(a: Vec, b: Vec): Vec {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function dot(a: Vec, b: Vec): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/**
 * Which side of the great circle through `a` and `b` (as [lat, lon]) the
 * point is on, as the same sign as `reference` or not.
 */
function sameSide(a: [number, number], b: [number, number], point: [number, number], reference: [number, number]): boolean {
  const normal = cross(vector(...a), vector(...b));
  return Math.sign(dot(normal, vector(...point))) === Math.sign(dot(normal, vector(...reference)));
}

/*
 * A side test against a great circle only means something near its arc: far
 * away, the other half of the same circle (the one over Europe, for line C)
 * decides. So places clearly away from a line are settled by longitude, and
 * the arcs are consulted only within the longitudes they span.
 */

/** West of line B: the Americas' side. */
function westOfLineB(lat: number, lon: number): boolean {
  if (lon < -60) return true;
  if (lon > -5) return false;
  if (lat >= 72) return lon < -10;
  const arcs: [[number, number], [number, number]][] = [
    [[72, -10], [40, -50]],
    [[40, -50], [-10, -20]],
  ];
  for (const [a, b] of arcs) {
    if (lat <= a[0] && lat >= b[0]) return sameSide(a, b, [lat, lon], [(a[0] + b[0]) / 2, -100]);
  }
  return lon < -20;
}

/** East of line C: the Americas' side again, seen from the Pacific. */
function eastOfLineC(lat: number, lon: number): boolean {
  // Longitudes east of the date line near the Aleutians are written as
  // positive; measured from 180 W they are the continuation of the Americas.
  const west = lon > 0 ? lon - 360 : lon;
  if (lat < 10) return west > -120;
  if (west > -160) return true;
  if (west < -205) return false;
  if (lat >= 65.5) return west > -168.97;
  const arcs: [[number, number], [number, number]][] = [
    [[65.5, -168.97], [50, 165 - 360]],
    [[50, 165 - 360], [10, -170]],
  ];
  for (const [a, b] of arcs) {
    if (lat <= a[0] && lat >= b[0]) {
      return sameSide([a[0], a[1]], [b[0], b[1]], [lat, west], [(a[0] + b[0]) / 2, -100]);
    }
  }
  return west > -170;
}

/** East of line A: Asia and Oceania's side. */
function eastOfLineA(lat: number, lon: number): boolean {
  if (lon < 0) return false;
  if (lat >= 40) return lon > 40;
  if (lat <= 23.44) return lon > 60;
  return sameSide([40, 40], [23.44, 60], [lat, lon], [31.7, 90]);
}

type Box = { south: number; north: number; west: number; east: number };
const inside = (lat: number, lon: number, box: Box) => lat >= box.south && lat < box.north && lon >= box.west && lon < box.east;

/** Region 1 east of line A: Russia, the Caucasus and Central Asia, Mongolia, eastern Turkey. */
const REGION_1_EAST: Box[] = [
  { south: 50, north: 90, west: 40, east: 180 },
  { south: 42.3, north: 50, west: 130.5, east: 146 },
  { south: 36, north: 50, west: 40, east: 80 },
  { south: 44, north: 50, west: 80, east: 87.5 },
  // Mongolia, narrower to the south, where China's Xinjiang and Inner
  // Mongolia run along it.
  { south: 46, north: 50, west: 87.5, east: 120 },
  { south: 43.5, north: 46, west: 95, east: 112 },
  { south: 42.5, north: 43.5, west: 97, east: 110 },
];
/** Region 3 inside the Region 1 boxes: north-east China, Iran, Afghanistan, Xinjiang. */
const REGION_3_EXCEPT: Box[] = [
  { south: 50, north: 53.6, west: 119, east: 127.5 },
  { south: 25, north: 39.8, west: 44, east: 63.5 },
  { south: 29, north: 38.5, west: 60, east: 75 },
  { south: 35, north: 40.5, west: 75, east: 80 },
];

export function regionAt(lat: number, lon: number): ItuRegion {
  const normalised = ((((lon + 180) % 360) + 360) % 360) - 180;
  const inAmericasLongitudes = normalised < -5 || normalised > 160;
  if (inAmericasLongitudes && westOfLineB(lat, normalised) && eastOfLineC(lat, normalised)) return 2;
  if (REGION_3_EXCEPT.some((box) => inside(lat, normalised, box))) return 3;
  if (REGION_1_EAST.some((box) => inside(lat, normalised, box))) return 1;
  if (eastOfLineA(lat, normalised)) return 3;
  // East of line C across the date line and west of line B in the far
  // Pacific is still Region 3 (Kiribati, French Polynesia west of 120 W).
  if (inAmericasLongitudes && !westOfLineB(lat, normalised)) return 1;
  if (inAmericasLongitudes) return 3;
  return 1;
}
