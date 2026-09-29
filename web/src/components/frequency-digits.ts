/**
 * Which digits the frequency readout shows, and what they are.
 *
 * Kept apart from the component so the readout's rules can be tested
 * without a browser.
 */

/**
 * 100 MHz down to 1 Hz.
 *
 * Nine digits, not eight. With eight the readout silently truncated anything
 * at or above 100 MHz - a real off-air capture at 145.9 MHz displayed as
 * 45.900.000, which is not a rounding error but a different frequency. Any
 * receiver on VHF, or an RX-888 covering the FM broadcast band, hit it.
 * The hundreds digit only appears on a receiver that can reach it. Showing
 * 007.100.000 on an HF band would fix the bug by making the common case worse.
 */
const PLACES = [1e8, 1e7, 1e6, 1e5, 1e4, 1e3, 1e2, 1e1, 1] as const;

/** The places worth showing for a receiver whose highest frequency is `topHz`. */
export function placesFor(topHz: number): readonly number[] {
  return PLACES.filter((place) => place <= 1e7 || topHz >= place);
}

export function digitsOf(hz: number, places: readonly number[]): number[] {
  const rounded = Math.max(0, Math.round(hz));
  return places.map((place) => Math.floor(rounded / place) % 10);
}

/**
 * Whether the digit at `index` is a leading zero, drawn dim: a zero with only
 * zeros before it, above the 1 MHz place. The 1 MHz digit always shows, so
 * 999 kHz reads 00.999.000 with the second zero lit, like a dial.
 */
export function isLeadingZero(digits: readonly number[], places: readonly number[], index: number): boolean {
  return places[index] > 1e6 && digits.slice(0, index + 1).every((d) => d === 0);
}
