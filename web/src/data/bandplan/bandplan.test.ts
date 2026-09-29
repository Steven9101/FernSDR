/**
 * The band plan files checked the way a reviewer would: every row inside its
 * bands, no overlaps where the plans do not overlap, every row citing a
 * source, and the combined plans making sense at the frequencies people ask
 * about. A plan that quietly draws a wrong edge misleads every listener.
 */
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { resolvePlan, type PlanFile } from '../../state/bandplan-resolve';
import { describeFrequency, planFor, spotNear, spotsIn } from '../../state/bandplan';

const here = fileURLToPath(new URL('.', import.meta.url));
const files = readdirSync(here).filter((f) => f.endsWith('.json')).sort();
const plans = new Map<string, PlanFile>(files.map((f) => {
  const plan = JSON.parse(readFileSync(`${here}/${f}`, 'utf8')) as PlanFile;
  return [plan.id, plan];
}));

const KINDS = new Set(['cw', 'digital', 'phone', 'beacon', 'broadcast', 'satellite', 'fm', 'utility', 'personal', 'other']);
const MODES = new Set(['usb', 'lsb', 'cw', 'am', 'nfm']);
const ISO = /^\d{4}-\d{2}-\d{2}$/;

function overlaps(rows: { low: number; high: number; label: string }[]): string[] {
  const sorted = [...rows].sort((a, b) => a.low - b.low);
  const found: string[] = [];
  for (let i = 1; i < sorted.length; i++) {
    if (sorted[i].low < sorted[i - 1].high) found.push(`${sorted[i - 1].low}-${sorted[i - 1].high} ${sorted[i - 1].label} / ${sorted[i].low}-${sorted[i].high} ${sorted[i].label}`);
  }
  return found;
}

const shared = [plans.get('services')!, plans.get('services-vhf')!, plans.get('activity')!];
const resolved = (region: string, country: string | null = null) =>
  resolvePlan(plans.get(region)!, country ? plans.get(country)! : null, shared);

describe('band plan files', () => {
  it('are all there', () => {
    expect(files).toHaveLength(12);
  });

  it.each(files)('%s is well formed and cites its sources', (file) => {
    const plan = [...plans.values()].find((p) => `${p.id}.json` === file)!;
    const problems: string[] = [];
    if (plan.schema !== 1) problems.push('schema must be 1');
    if (!ISO.test(plan.reviewed)) problems.push('reviewed is not a date');
    for (const [key, source] of Object.entries(plan.sources)) {
      if (!ISO.test(source.date)) problems.push(`source ${key} has no date`);
      if (!source.url.startsWith('https://')) problems.push(`source ${key} is not https`);
    }
    for (const s of plan.segments) {
      if (!(s.low < s.high)) problems.push(`segment ${s.low}-${s.high} is empty`);
      if (!KINDS.has(s.kind)) problems.push(`segment ${s.low} kind ${s.kind}`);
      if (!plan.sources[s.src]) problems.push(`segment ${s.low} cites ${s.src}`);
    }
    const seen = new Set<string>();
    for (const s of plan.spots) {
      if (s.mode !== undefined && !MODES.has(s.mode)) problems.push(`spot ${s.khz} mode ${s.mode}`);
      if (!plan.sources[s.src]) problems.push(`spot ${s.khz} cites ${s.src}`);
      const key = `${s.khz}|${s.label}|${JSON.stringify(s.regions ?? null)}`;
      if (seen.has(key)) problems.push(`spot ${s.khz} ${s.label} twice`);
      seen.add(key);
    }
    if (plan.scope === 'region' || plan.scope === 'country') {
      problems.push(...overlaps(plan.segments).map((o) => `overlap ${o}`));
      const bands = plan.bands ?? [];
      problems.push(...overlaps(bands.map((b) => ({ ...b, label: b.name }))).map((o) => `band overlap ${o}`));
      for (const s of plan.segments) {
        if (!bands.some((b) => s.low >= b.low && s.high <= b.high)) problems.push(`segment ${s.low}-${s.high} outside the bands`);
      }
      for (const s of plan.spots) {
        if (!bands.some((b) => s.khz >= b.low && s.khz < b.high)) problems.push(`spot ${s.khz} outside the bands`);
      }
    }
    expect(problems).toEqual([]);
  });

  it('has service rows that nest but never cross, in every region and country', () => {
    const problems: string[] = [];
    const countries: Record<number, (string | undefined)[]> = { 1: [undefined, 'gb', 'de'], 2: [undefined, 'us', 'ca'], 3: [undefined, 'au', 'jp'] };
    for (const r of [1, 2, 3]) {
      for (const cc of countries[r]) {
        const rows = shared.flatMap((f) => f.segments).filter((s) =>
          (!s.regions || s.regions.includes(r)) &&
          (!s.countries || (cc !== undefined && s.countries.includes(cc))) &&
          !(cc !== undefined && s.exceptCountries?.includes(cc)));
        for (const a of rows) for (const b of rows) {
          if (a.low < b.low && b.low < a.high && a.high < b.high) problems.push(`R${r} ${cc ?? ''}: ${a.label} crosses ${b.label}`);
        }
      }
    }
    expect(problems).toEqual([]);
  });

  it.each([
    ['iaru-r1', null], ['iaru-r1', 'country-gb'], ['iaru-r1', 'country-de'],
    ['iaru-r2', null], ['iaru-r2', 'country-us'], ['iaru-r2', 'country-ca'],
    ['iaru-r3', null], ['iaru-r3', 'country-au'], ['iaru-r3', 'country-jp'],
  ] as const)('%s with %s resolves without overlapping amateur segments', (region, country) => {
    if (country) expect(plans.get(country)!.region).toBe(plans.get(region)!.region);
    const plan = resolved(region, country);
    expect(overlaps(plan.segments.filter((s) => s.layer === 'amateur'))).toEqual([]);
    expect(plan.bands.length).toBeGreaterThan(10);
  });

  it('was reviewed within the last year', () => {
    // Plans change at IARU conferences and with national rules. Past a year,
    // compare the files with their sources again and move `reviewed`.
    const stale = [...plans.values()].filter((p) => Date.now() - Date.parse(p.reviewed) > 400 * 24 * 3600 * 1000);
    expect(stale.map((p) => p.id)).toEqual([]);
  });
});

describe('the plan at the frequencies people ask about', () => {
  it('ends 40 m where the region ends it', () => {
    expect(describeFrequency(7_150_000, resolved('iaru-r1'))).toMatch(/^40 m/);
    expect(describeFrequency(7_250_000, resolved('iaru-r1'))).toMatch(/broadcast/i);
    expect(describeFrequency(7_250_000, resolved('iaru-r2'))).toMatch(/^40 m/);
  });

  it('follows a country: the US 60 m channels, no 60 m in Australia', () => {
    expect(describeFrequency(5_357_000, resolved('iaru-r2', 'country-us'))).toMatch(/^60 m/);
    expect(describeFrequency(5_357_000, resolved('iaru-r3', 'country-au')) ?? '').not.toMatch(/^60 m/);
    expect(resolved('iaru-r3', 'country-au').spots.some((s) => s.hz === 5_357_000 && s.label === 'FT8')).toBe(false);
  });

  it('names the IBP beacon slot on 20 m', () => {
    expect(describeFrequency(14_100_000, resolved('iaru-r1'))).toMatch(/beacon/i);
  });

  it('keeps the dials listeners look for when a wide view has too many markers', () => {
    const plan = resolved('iaru-r1');
    const shown = spotsIn(0, 30_000_000, 24, plan);
    expect(shown).toHaveLength(24);
    // Far more than 24 markers below 30 MHz: those shown are the software
    // dials and beacons listeners look for, not whatever came first.
    expect(shown.every((s) => /^(FT8|FT4|WSPR|JS8|NCDXF|IBP)/.test(s.label))).toBe(true);
    expect(shown.some((s) => s.label === 'FT8')).toBe(true);
  });

  it('picks the plan from the choice, and the region from the grid for auto', () => {
    expect(planFor('none', 'JO30')).toBeNull();
    expect(planFor('gb', undefined)).toEqual({ region: 1, country: 'gb' });
    expect(planFor('auto', 'FN20')).toEqual({ region: 2, country: null });
    expect(planFor('auto', 'PM95')).toEqual({ region: 3, country: null });
    expect(planFor(undefined, 'JO30qu')).toEqual({ region: 1, country: null });
  });

  it('finds the marker a tap on the strip means, and only markers with a mode', () => {
    const plan = resolved('iaru-r1');
    expect(spotNear(14_074_300, 500, 14_000_000, 14_350_000, plan)).toMatchObject({ hz: 14_074_000, label: 'FT8', mode: 'usb' });
    expect(spotNear(14_160_000, 500, 14_000_000, 14_350_000, plan)).toBeNull();
    // FM broadcast, ADS-B and the like have no mode here: never a tune target.
    const markerOnly = resolved('iaru-r1').spots.find((spot) => spot.mode === null)!;
    expect(markerOnly).toBeDefined();
    expect(spotNear(markerOnly.hz, 1, markerOnly.hz - 1, markerOnly.hz + 1, resolved('iaru-r1'))).toBeNull();
  });
});
