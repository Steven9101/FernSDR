/**
 * The band plan files and how they combine into the plan one receiver shows.
 *
 * Files (web/src/data/bandplan):
 * - iaru-r{1,2,3}.json: one IARU region's amateur plan, bands, segments and
 *   centres of activity;
 * - country-xx.json: a national plan that replaces the region's bands, and
 *   its segments and spots inside the ranges it `covers`;
 * - activity.json: software dial frequencies (FT8, FT4, WSPR, JS8);
 * - services.json, services-vhf.json: broadcast, utility, time signals,
 *   licence-free radio and the rest, with the regions they apply to.
 *
 * Frequencies in the files are kHz as the sources print them, so a row can be
 * checked against its source line by line; everything here returns Hz.
 */

export type SegmentKind =
  | 'cw'
  | 'digital'
  | 'phone'
  | 'beacon'
  | 'broadcast'
  | 'satellite'
  | 'fm'
  | 'utility'
  | 'personal'
  | 'other';

export interface SourceRef {
  title: string;
  version: string;
  date: string;
  url: string;
}

interface Applicability {
  regions?: number[];
  countries?: string[];
  exceptCountries?: string[];
}

export interface SegmentRow extends Applicability {
  low: number;
  high: number;
  kind: SegmentKind;
  label: string;
  src: string;
  bw?: number;
  note?: string;
  unverified?: boolean;
}

export interface SpotRow extends Applicability {
  khz: number;
  label: string;
  mode?: string;
  src: string;
  span?: [number, number];
  note?: string;
  unverified?: boolean;
}

export interface BandRow {
  name: string;
  low: number;
  high: number;
  status?: 'primary' | 'secondary';
}

export interface PlanFile {
  schema: 1;
  id: string;
  title: string;
  scope: 'region' | 'country' | 'services' | 'activity';
  region?: number;
  country?: string;
  reviewed: string;
  sources: Record<string, SourceRef>;
  covers?: [number, number][];
  bands?: BandRow[];
  segments: SegmentRow[];
  spots: SpotRow[];
}

export type Layer = 'amateur' | 'service';

export interface BandSegment {
  /** Inclusive lower edge, Hz. */
  low: number;
  /** Exclusive upper edge, Hz. */
  high: number;
  label: string;
  kind: SegmentKind;
  layer: Layer;
  /** The amateur band it lies in ("40 m"), for amateur rows. */
  band: string | null;
  unverified: boolean;
  note?: string;
}

export interface SpotFrequency {
  hz: number;
  label: string;
  /** What a click tunes to; null for a marker only (a signal no mode here can play). */
  mode: string | null;
  layer: Layer;
  unverified: boolean;
  note?: string;
}

export interface Band {
  name: string;
  low: number;
  high: number;
}

export interface ResolvedPlan {
  title: string;
  bands: Band[];
  /** Sorted by lower edge, wider first, so a nested row is drawn over its container. */
  segments: BandSegment[];
  spots: SpotFrequency[];
  sources: SourceRef[];
}

type Range = [number, number];

/** The parts of each row that lie inside `keep`, splitting rows that cross an edge. */
function clipTo<T extends { low: number; high: number }>(rows: T[], keep: Range[]): T[] {
  return rows.flatMap((row) =>
    keep
      .filter(([a, b]) => row.low < b && row.high > a)
      .map(([a, b]) => ({ ...row, low: Math.max(row.low, a), high: Math.min(row.high, b) })),
  );
}

/** The parts of each row that lie outside every range in `cut`. */
function cutOut<T extends { low: number; high: number }>(rows: T[], cut: Range[]): T[] {
  let pieces = rows;
  for (const [a, b] of cut) {
    pieces = pieces.flatMap((row) => {
      if (row.high <= a || row.low >= b) return [row];
      const out: T[] = [];
      if (row.low < a) out.push({ ...row, high: a });
      if (row.high > b) out.push({ ...row, low: b });
      return out;
    });
  }
  return pieces;
}

const hz = (khz: number) => Math.round(khz * 1000);

/**
 * The plan a receiver shows.
 *
 * - Bands are the country's when a country is chosen (allocations are
 *   national law), otherwise the region's.
 * - Amateur segments: the region's minus the ranges the country covers, plus
 *   the country's, clipped to the bands; so a country without 4 m needs no
 *   row to remove it.
 * - Amateur spots: the region's outside the covered ranges, the country's
 *   and the software dials, kept only where the signal starts inside a band.
 * - Service rows: those that apply to the region and the country.
 */
export function resolvePlan(region: PlanFile, country: PlanFile | null, shared: PlanFile[]): ResolvedPlan {
  const r = region.region;
  const cc = country?.country;
  const applies = (row: Applicability) =>
    (!row.regions || (r !== undefined && row.regions.includes(r))) &&
    (!row.countries || (cc !== undefined && row.countries.includes(cc))) &&
    !(cc !== undefined && row.exceptCountries?.includes(cc));

  const bandRows = [...(country?.bands ?? region.bands ?? [])].sort((a, b) => a.low - b.low);
  const bandRanges: Range[] = bandRows.map((b) => [b.low, b.high]);
  const bandAt = (khz: number) => bandRows.find((b) => khz >= b.low && khz < b.high) ?? null;
  const covers = country?.covers ?? [];
  const covered = (khz: number) => covers.some(([a, b]) => khz >= a && khz < b);

  const amateurSegments = clipTo([...cutOut(region.segments, covers), ...(country?.segments ?? [])], bandRanges);
  const activity = shared.filter((f) => f.scope === 'activity').flatMap((f) => f.spots);
  const amateurSpots = [
    ...region.spots.filter((s) => !covered(s.khz)),
    ...(country?.spots ?? []),
    ...activity.filter(applies),
  ].filter((s) => bandAt(s.khz + (s.span?.[0] ?? 0) / 1000) !== null);

  const services = shared.filter((f) => f.scope === 'services');
  const serviceSegments = services.flatMap((f) => f.segments).filter(applies);
  const serviceSpots = services.flatMap((f) => f.spots).filter(applies);

  const segment = (row: SegmentRow, layer: Layer): BandSegment => ({
    low: hz(row.low),
    high: hz(row.high),
    label: row.label,
    kind: row.kind,
    layer,
    band: layer === 'amateur' ? (bandAt(row.low)?.name ?? null) : null,
    unverified: row.unverified === true,
    ...(row.note ? { note: row.note } : {}),
  });
  const spot = (row: SpotRow, layer: Layer): SpotFrequency => ({
    hz: hz(row.khz),
    label: row.label,
    mode: row.mode ?? null,
    layer,
    unverified: row.unverified === true,
    ...(row.note ? { note: row.note } : {}),
  });

  return {
    title: country?.title ?? region.title,
    bands: bandRows.map((b) => ({ name: b.name, low: hz(b.low), high: hz(b.high) })),
    segments: [
      ...serviceSegments.map((s) => segment(s, 'service')),
      ...amateurSegments.map((s) => segment(s, 'amateur')),
    ].sort((a, b) => a.low - b.low || b.high - b.low - (a.high - a.low)),
    spots: [...serviceSpots.map((s) => spot(s, 'service')), ...amateurSpots.map((s) => spot(s, 'amateur'))].sort(
      (a, b) => a.hz - b.hz,
    ),
    sources: [region, country, ...shared].flatMap((f) => (f ? Object.values(f.sources) : [])),
  };
}
