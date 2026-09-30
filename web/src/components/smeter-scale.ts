/**
 * The S-meter's one scale, shared by the reading and every face drawn from it.
 *
 * S1 sits at the left end and S9 at 60% of the way across; the rest, S9 to
 * S9+40, fills the last 40%. The label, the bar, the needle's marks and the
 * trace all place a level through the functions here, so a needle on the
 * "+20" mark reads S9+20.
 */

export const DB_PER_S_UNIT = 6;

/** Where S9 sits on every face. */
export const S9_AT = 0.6;

/** How far above S9 the scale goes, dB. */
const OVER_DB = 40;

/** Where S-unit `s` (1 to 9) sits on the scale. */
export function sUnitAt(s: number): number {
  return ((s - 1) / 8) * S9_AT;
}

/** Where `db` dB over S9 sits on the scale. */
export function overS9At(db: number): number {
  return S9_AT + (Math.min(db, OVER_DB) / OVER_DB) * (1 - S9_AT);
}

/**
 * The reading for a level, given `base`, the level that reads as S1 in the
 * same units.
 */
export function toSUnits(dbfs: number, base: number): { label: string; fraction: number } {
  const units = 1 + (dbfs - base) / DB_PER_S_UNIT;
  if (units >= 9) {
    const over = (units - 9) * DB_PER_S_UNIT;
    const rounded = Math.round(over);
    return { label: rounded > 0 ? `S9+${rounded}` : 'S9', fraction: overS9At(over) };
  }
  const clamped = Math.max(1, Math.min(9, units));
  return { label: `S${Math.round(clamped)}`, fraction: sUnitAt(clamped) };
}

export const NEEDLE_MARKS = [
  ...[1, 3, 5, 7, 9].map((s) => ({ at: sUnitAt(s), label: String(s) })),
  { at: overS9At(20), label: '+20' },
  { at: overS9At(40), label: '+40' },
];

/** The S-units between the numbered ones, and +10 and +30: a printed dial has them. */
export const MINOR_MARKS = [...[2, 4, 6, 8].map(sUnitAt), overS9At(10), overS9At(30)];

/** The bar's numbered ticks below S9; S9 has its own, marking where "over" starts. */
export const BAR_TICKS = [1, 3, 5, 7].map((s) => ({ at: sUnitAt(s), label: String(s) }));
