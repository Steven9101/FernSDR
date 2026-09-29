/**
 * A band's `hours` as the panel offers them. The receiver reads and checks
 * the text (server/src/core/band_hours.h); these only name the common ones
 * and pair a band's hours with those of the band sharing its input.
 */

export const DAYLIGHT = 'sunrise-sunset';
export const NIGHT = 'sunset-sunrise';

const CLOCK_RANGE = /^(\d{2}):(\d{2})-(\d{2}):(\d{2})$/;

/** How the hours read in a row: "In daylight", "06:00 to 18:00 UTC". */
export function describeHours(hours: string | undefined): string {
  const text = (hours ?? '').trim().toLowerCase();
  if (!text || text === 'always') return 'Always';
  if (text === DAYLIGHT) return 'In daylight at the station';
  if (text === NIGHT) return 'At night at the station';
  const clock = CLOCK_RANGE.exec(text);
  if (clock) return `${clock[1]}:${clock[2]} to ${clock[3]}:${clock[4]} UTC`;
  return text;
}

/** One range of clock times, as the custom sheet edits it; null for anything else. */
export function clockRange(hours: string | undefined): { from: string; until: string } | null {
  const clock = CLOCK_RANGE.exec((hours ?? '').trim());
  return clock ? { from: `${clock[1]}:${clock[2]}`, until: `${clock[3]}:${clock[4]}` } : null;
}

/**
 * The hours that leave off where these begin and begin where they leave
 * off, for the band taking turns with this one; null when there is no such
 * single range (always, or several ranges).
 */
export function complement(hours: string): string | null {
  const text = hours.trim().toLowerCase();
  if (text === DAYLIGHT) return NIGHT;
  if (text === NIGHT) return DAYLIGHT;
  const clock = clockRange(text);
  return clock ? `${clock.until}-${clock.from}` : null;
}

/** Whether the hours depend on where the station is. */
export function usesSun(hours: string | undefined): boolean {
  return /sunrise|sunset/i.test(hours ?? '');
}

/** "18:00 UTC" from UTC milliseconds. */
export function utcClock(ms: number): string {
  const date = new Date(ms);
  return `${String(date.getUTCHours()).padStart(2, '0')}:${String(date.getUTCMinutes()).padStart(2, '0')} UTC`;
}

/** Where the band stands now: "On the air until 17:58 UTC", or null for a band without hours. */
export function hoursNow(onAir: boolean | undefined, nextChange: number | undefined): string | null {
  if (onAir === undefined || nextChange === undefined) return null;
  if (nextChange < 0) return onAir ? null : 'Off the air';
  return `${onAir ? 'On the air' : 'Off the air'} until ${utcClock(nextChange)}`;
}
