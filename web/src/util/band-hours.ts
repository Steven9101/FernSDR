import type { BandDescription } from '../net/protocol';

type Scheduled = Pick<BandDescription, 'id' | 'name' | 'on_air' | 'next_change' | 'shared_input'>;

/** How long before its hours end a listener on a band is told. */
export const WARN_BEFORE_MS = 10 * 60_000;

/** A band is off the air only when the receiver says so; older ones never do. */
export function offAir(band: Pick<BandDescription, 'on_air'>): boolean {
  return band.on_air === false;
}

/** 18:00 UTC, the way the hours are written. */
export function utcTime(ms: number): string {
  const date = new Date(ms);
  return `${String(date.getUTCHours()).padStart(2, '0')}:${String(date.getUTCMinutes()).padStart(2, '0')} UTC`;
}

/** "On the air from 18:00 UTC" for a band waiting for its hours, or null. */
export function comesOnAt(band: Pick<BandDescription, 'on_air' | 'next_change'>): string | null {
  if (!offAir(band)) return null;
  return band.next_change !== undefined && band.next_change > 0 ? `On the air from ${utcTime(band.next_change)}` : 'Off the air';
}

/** The time alone, "18:00", for a column too narrow for the sentence; "off" without one. */
export function comesOnShort(band: Pick<BandDescription, 'on_air' | 'next_change'>): string | null {
  if (!offAir(band)) return null;
  return band.next_change !== undefined && band.next_change > 0 ? utcTime(band.next_change).replace(' UTC', '') : 'off';
}

/**
 * The band that comes on when `band` goes off: the one on its input that is
 * off the air now and changes at the same moment. Null when there is none.
 */
export function takesOver(band: Scheduled, bands: readonly Scheduled[]): Scheduled | null {
  if (!band.shared_input || band.next_change === undefined || band.next_change <= 0) return null;
  return bands.find((other) => other.id !== band.id && other.shared_input === band.shared_input && offAir(other) &&
    other.next_change === band.next_change) ?? null;
}

/**
 * What a listener on `band` is told in the last minutes of its hours, and
 * while they stay on it once it is off the air; null otherwise.
 */
export function endingNotice(band: Scheduled | undefined, bands: readonly Scheduled[], now: number): string | null {
  if (!band) return null;
  // Still on a band that went off the air: nobody took its input over, so
  // the listener stays and hears nothing until they choose another.
  if (offAir(band)) {
    const back = band.next_change !== undefined && band.next_change > 0 ? ` until ${utcTime(band.next_change)}` : '';
    return `${band.name} is off the air${back}. Choose another band to listen on.`;
  }
  if (band.next_change === undefined || band.next_change <= now) return null;
  if (band.next_change - now > WARN_BEFORE_MS) return null;
  const next = takesOver(band, bands);
  const at = utcTime(band.next_change);
  return next
    ? `${band.name} goes off the air at ${at}; the receiver switches to ${next.name} and you with it.`
    : `${band.name} goes off the air at ${at}.`;
}
