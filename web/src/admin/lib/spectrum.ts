import type { RecentSpectrum } from '../api';

/** The band's recent lines as levels in dB, oldest first. */
export function decodeRecent(recent: RecentSpectrum): number[][] {
  const bytes = Uint8Array.from(atob(recent.lines), (character) => character.charCodeAt(0));
  const step = (recent.ceiling_db - recent.floor_db) / 255;
  const lines: number[][] = [];
  for (let row = 0; row < recent.count && (row + 1) * recent.width <= bytes.length; row++) {
    const line = new Array<number>(recent.width);
    for (let cell = 0; cell < recent.width; cell++) line[cell] = recent.floor_db + bytes[row * recent.width + cell] * step;
    lines.push(line);
  }
  return lines;
}
