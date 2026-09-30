/**
 * A time zone's offset as a clock shows it: UTC+2, UTC−3:30, UTC+5:45.
 * `minutesEast` is minutes ahead of UTC, the negative of
 * Date.getTimezoneOffset(). Hours and minutes rather than a decimal, which
 * turns Nepal's 5:45 into 5.8.
 */
export function utcOffsetLabel(minutesEast: number): string {
  if (minutesEast === 0) return 'UTC';
  const total = Math.abs(Math.round(minutesEast));
  const hours = Math.floor(total / 60);
  const minutes = total % 60;
  return `UTC${minutesEast > 0 ? '+' : '−'}${hours}${minutes ? `:${String(minutes).padStart(2, '0')}` : ''}`;
}
