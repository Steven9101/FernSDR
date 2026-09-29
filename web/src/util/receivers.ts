/**
 * The short label shown beside a band's name for what it listens with. A
 * text label rather than a maker's logo: it reads at 10 px, and the page
 * carries nobody's trademark.
 */
const LABELS: Record<string, string> = {
  rx888: 'RX-888',
  rtlsdr: 'RTL-SDR',
  sdrplay: 'SDRplay',
  airspy: 'Airspy',
  hackrf: 'HackRF',
  file: 'Recording',
  stdin: 'Stream',
  pipe: 'Stream',
  udp: 'Network',
  test: 'Test signal',
};

export function receiverLabel(receiver: string | undefined): string | null {
  if (!receiver) return null;
  return LABELS[receiver] ?? receiver.replace(/^fern-/, '').toUpperCase();
}

/**
 * How well a band matches what was typed in the band search, lower first,
 * or null for no match: the name exactly, then a name starting with it (so
 * "2 m" puts 2 m before 12 m), then a name or label containing it, then a
 * frequency in MHz that lies inside the band ("7.1", "145,5").
 */
export function bandRank(query: string, band: { name: string; low: number; high: number; receiver?: string }): number | null {
  const text = query.trim().toLowerCase();
  if (!text) return 0;
  const name = band.name.toLowerCase();
  if (name === text) return 0;
  if (name.startsWith(text)) return 1;
  if (name.includes(text) || (receiverLabel(band.receiver) ?? '').toLowerCase().includes(text)) return 2;
  const mhz = Number(text.replace(',', '.'));
  if (/^[\d.,]+$/.test(text) && Number.isFinite(mhz) && mhz * 1e6 >= band.low && mhz * 1e6 <= band.high) return 3;
  return null;
}
