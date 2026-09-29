/**
 * Names for the programme type an FM station sends by RDS. The code means
 * different things on the two sides of the Atlantic: Europe and most of the
 * world use the RDS table (IEC 62106), North America the RBDS one (NRSC-4),
 * where 10 is country music rather than pop. ITU Region 2 stands in for
 * "North America" here, as it does for the de-emphasis.
 */
const RDS = [
  '', 'News', 'Current affairs', 'Information', 'Sport', 'Education', 'Drama', 'Culture',
  'Science', 'Varied', 'Pop music', 'Rock music', 'Easy listening', 'Light classical', 'Serious classical', 'Other music',
  'Weather', 'Finance', "Children's programmes", 'Social affairs', 'Religion', 'Phone-in', 'Travel', 'Leisure',
  'Jazz music', 'Country music', 'National music', 'Oldies music', 'Folk music', 'Documentary', 'Alarm test', 'Alarm',
];

const RBDS = [
  '', 'News', 'Information', 'Sports', 'Talk', 'Rock', 'Classic rock', 'Adult hits',
  'Soft rock', 'Top 40', 'Country', 'Oldies', 'Soft', 'Nostalgia', 'Jazz', 'Classical',
  'Rhythm and blues', 'Soft rhythm and blues', 'Foreign language', 'Religious music', 'Religious talk', 'Personality', 'Public', 'College',
  'Spanish talk', 'Spanish music', 'Hip hop', '', '', 'Weather', 'Emergency test', 'Emergency',
];

/** The programme type's name, or '' for none, unassigned or out of range. */
export function programmeType(code: number | undefined, northAmerica: boolean): string {
  if (code === undefined || !Number.isInteger(code) || code < 0 || code > 31) return '';
  return (northAmerica ? RBDS : RDS)[code];
}

/**
 * The station name as stations send it: eight characters padded with
 * spaces, and some scroll their name or a slogan through them. Trimmed, and
 * inner runs of spaces kept to one.
 */
export function stationName(ps: string | undefined): string {
  return (ps ?? '').trim().replace(/\s+/g, ' ');
}
