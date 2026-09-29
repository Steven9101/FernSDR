/** Frequencies as the programs that drive a WebSDR page write and read them. */

/** A frequency in kHz as such a program writes it ("7074", "7074.5", "7074,5"). */
export function parseKhz(text: unknown): number | null {
  const value = Number(String(text ?? '').trim().replace(',', '.'));
  return Number.isFinite(value) && value > 0 && value < 1e8 ? Math.round(value * 1000) : null;
}

/** kHz with two decimals, as WebSDR's frequency field shows it. */
export function formatKhz(hz: number): string {
  return (hz / 1000).toFixed(2);
}
