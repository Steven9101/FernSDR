/**
 * The band plan drawn over the waterfall and named in the readout.
 *
 * Users orient themselves by the band plan, not by absolute frequency, and
 * the plan differs by ITU region and by country: 40 m ends at 7200 kHz in
 * Europe and at 7300 in the Americas, where Europe has broadcasters. So the
 * operator says which plan applies ([site] band_plan), "auto" works it out
 * from the station's grid square, and only the files that plan needs are
 * loaded, after the page is up: 7 to 12 kB, never in the first load.
 */
import { box } from './reactive.svelte';
import { resolvePlan, type Band, type BandSegment, type PlanFile, type ResolvedPlan, type SpotFrequency } from './bandplan-resolve';
import { locatorCentre } from '../util/locator';
import { regionAt } from '../util/itu-region';

export type { BandSegment, SegmentKind, SpotFrequency } from './bandplan-resolve';

export type BandPlanChoice = 'auto' | 'none' | 'r1' | 'r2' | 'r3' | 'us' | 'ca' | 'gb' | 'de' | 'au' | 'jp';

const COUNTRY_REGION: Record<string, 1 | 2 | 3> = { us: 2, ca: 2, gb: 1, de: 1, au: 3, jp: 3 };

type Loader = () => Promise<{ default: unknown }>;
const FILES: Record<string, Loader> = {
  'iaru-r1': () => import('../data/bandplan/iaru-r1.json'),
  'iaru-r2': () => import('../data/bandplan/iaru-r2.json'),
  'iaru-r3': () => import('../data/bandplan/iaru-r3.json'),
  'country-us': () => import('../data/bandplan/country-us.json'),
  'country-ca': () => import('../data/bandplan/country-ca.json'),
  'country-gb': () => import('../data/bandplan/country-gb.json'),
  'country-de': () => import('../data/bandplan/country-de.json'),
  'country-au': () => import('../data/bandplan/country-au.json'),
  'country-jp': () => import('../data/bandplan/country-jp.json'),
  activity: () => import('../data/bandplan/activity.json'),
  services: () => import('../data/bandplan/services.json'),
  'services-vhf': () => import('../data/bandplan/services-vhf.json'),
};

/** The plan in use; null while loading, with none chosen, or when it failed to load. */
export const bandPlan = box<ResolvedPlan | null>(null);

/**
 * Which plan a choice means: the region and the country, if any. "auto"
 * follows the grid square, and Region 1 without one, where most receivers
 * are; the admin panel says so and asks for the square.
 */
export function planFor(choice: string | undefined, grid: string | undefined): { region: 1 | 2 | 3; country: string | null } | null {
  if (choice === 'none') return null;
  if (choice === 'r1' || choice === 'r2' || choice === 'r3') return { region: Number(choice[1]) as 1 | 2 | 3, country: null };
  if (choice && COUNTRY_REGION[choice]) return { region: COUNTRY_REGION[choice], country: choice };
  const here = locatorCentre(grid);
  return { region: here ? regionAt(here.lat, here.lon) : 1, country: null };
}

let generation = 0;

/**
 * Loads the plan for the receiver's choice. `highestHz` is the top of the
 * highest band: the VHF and UHF services are loaded only for a receiver that
 * reaches them.
 */
export async function loadBandPlan(choice: string | undefined, grid: string | undefined, highestHz: number): Promise<void> {
  const current = ++generation;
  const plan = planFor(choice, grid);
  if (!plan) {
    bandPlan.value = null;
    return;
  }
  const names = [`iaru-r${plan.region}`, ...(plan.country ? [`country-${plan.country}`] : []), 'activity', 'services'];
  if (highestHz > 30e6) names.push('services-vhf');
  try {
    const files = (await Promise.all(names.map((name) => FILES[name]()))).map((module) => module.default as PlanFile);
    if (current !== generation) return;
    const [region, ...rest] = files;
    const country = plan.country ? rest.shift()! : null;
    bandPlan.value = resolvePlan(region, country, rest);
  } catch {
    // A page open across an update asks for files by their old names. The
    // plan is an aid, not the receiver: go without it until a reload.
    if (current === generation) bandPlan.value = null;
  }
}

/** Segments overlapping a span, for drawing the band-plan strip. */
export function segmentsIn(lowHz: number, highHz: number, plan = bandPlan.value): BandSegment[] {
  return plan ? plan.segments.filter((segment) => segment.high > lowHz && segment.low < highHz) : [];
}

/**
 * How much a listener is looking for a marker: the software dials and
 * beacons, then time and standard-frequency stations, then the rest.
 */
function priority(spot: SpotFrequency): number {
  if (spot.layer === 'amateur' && /^(FT8|FT4|WSPR|JS8|NCDXF|IBP)/.test(spot.label)) return 0;
  if (/^(WWV|WWVH|DCF|MSF|JJY|RWM|BPM|HLA|CHU|YVTO|LOL|TDF)/.test(spot.label)) return 1;
  return spot.layer === 'amateur' ? 2 : 3;
}

/** Spot markers inside a span; beyond `maxCount`, the ones listeners look for first. */
export function spotsIn(lowHz: number, highHz: number, maxCount = 24, plan = bandPlan.value): SpotFrequency[] {
  if (!plan) return [];
  const inside = plan.spots.filter((spot) => spot.hz >= lowHz && spot.hz <= highHz);
  if (inside.length <= maxCount) return inside;
  return inside
    .map((spot, index) => ({ spot, index }))
    .sort((a, b) => priority(a.spot) - priority(b.spot) || a.index - b.index)
    .slice(0, maxCount)
    .sort((a, b) => a.index - b.index)
    .map(({ spot }) => spot);
}

/** The amateur band a frequency is in, if any. */
export function bandAt(hz: number, plan = bandPlan.value): Band | null {
  return plan?.bands.find((band) => hz >= band.low && hz < band.high) ?? null;
}

/**
 * What the plan calls a frequency: the band and its segment ("40 m, CW"),
 * or the narrowest service allocation ("31 m broadcast"), or null.
 */
export function describeFrequency(hz: number, plan = bandPlan.value): string | null {
  if (!plan) return null;
  const containing = plan.segments.filter((s) => hz >= s.low && hz < s.high);
  const amateur = containing.find((s) => s.layer === 'amateur');
  if (amateur) return amateur.band ? `${amateur.band}, ${amateur.label}` : amateur.label;
  const band = bandAt(hz, plan);
  if (band) return band.name;
  const service = containing.filter((s) => s.layer === 'service').sort((a, b) => a.high - a.low - (b.high - b.low))[0];
  return service ? service.label : null;
}

/**
 * The marker a tap at `hz` means, among those drawn in the view: the nearest
 * one within `toleranceHz` that has a mode to tune to.
 */
export function spotNear(hz: number, toleranceHz: number, lowHz: number, highHz: number, plan = bandPlan.value): SpotFrequency | null {
  let best: SpotFrequency | null = null;
  for (const spot of spotsIn(lowHz, highHz, 24, plan)) {
    if (!spot.mode || Math.abs(spot.hz - hz) > toleranceHz) continue;
    if (!best || Math.abs(spot.hz - hz) < Math.abs(best.hz - hz)) best = spot;
  }
  return best;
}
