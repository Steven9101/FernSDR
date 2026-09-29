/** How fast the S-meter's peak marker falls back. */

/**
 * The strongest reading lately, falling back slowly.
 *
 * A meter that only shows now cannot answer "how strong was that peak", which
 * is the question a signal report is.
 *
 * The rate is derived rather than picked. Two things bound it. A report is
 * quoted in S-units, so a burst should still be within one of its peak a
 * second after it, which puts the ceiling at 6 dB a second. And a marker that
 * takes longer than about ten seconds to cross the scale reads as stuck; the
 * scale is S1 to S9+40, which is 88 dB, so that puts the floor near 9. Eight
 * sits between them: a second after a burst the marker is still inside the
 * same S-unit and a bit, and it crosses the whole scale in eleven seconds.
 */
export const PEAK_FALL_DB_PER_SECOND = 8;

/**
 * The marker's next position: never below the level, and otherwise falling at
 * the rate above. Pure, so the rate can be asserted rather than watched.
 *
 * A gap in the readings is clamped. Telemetry pauses while a band rebuilds
 * its channel, and without the clamp the marker treats the pause as elapsed
 * time and drops the whole way in one step, which looks like the hold not
 * working at all.
 */
export function nextPeak(current: number, level: number, elapsedSeconds: number): number {
  const elapsed = Math.max(0, Math.min(elapsedSeconds, 0.5));
  return Math.max(level, current - PEAK_FALL_DB_PER_SECOND * elapsed);
}
