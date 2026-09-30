/**
 * Waterfall colour maps.
 *
 * `mono` and `ember` rise in perceived lightness all the way, so a stronger
 * signal always looks brighter. `classic` and `aurora` do not: near the top
 * their orange stop is darker than the yellow below it (CIE L* about 80 to 56
 * in `classic`, 80 to 69 in `aurora`), so two different levels there can read
 * as the same brightness. `ember` is the one to reach for if you have trouble
 * distinguishing colours: it carries level entirely in lightness and
 * saturation.
 */

export type PaletteId = 'aurora' | 'classic' | 'mono' | 'ember';

export interface PaletteInfo {
  id: PaletteId;
  label: string;
  description: string;
}

export const PALETTES: PaletteInfo[] = [
  { id: 'mono', label: 'Mono', description: 'Grey scale, like the interface around it' },
  { id: 'aurora', label: 'Aurora', description: 'Cool to warm, tuned for long sessions' },
  { id: 'classic', label: 'Classic', description: 'The familiar WebSDR ramp, the default' },
  { id: 'ember', label: 'Ember', description: 'Lightness only; colour-vision safe' },
];

type Stop = [number, number, number, number]; // position, r, g, b

const RAMPS: Record<PaletteId, Stop[]> = {
  aurora: [
    [0.0, 6, 10, 18],
    [0.18, 12, 30, 62],
    [0.36, 10, 74, 110],
    [0.54, 16, 132, 128],
    [0.7, 92, 184, 96],
    [0.84, 226, 196, 70],
    [0.94, 250, 140, 60],
    [1.0, 255, 250, 235],
  ],
  classic: [
    [0.0, 0, 0, 0],
    [0.25, 0, 0, 140],
    [0.45, 0, 90, 190],
    [0.62, 0, 190, 130],
    [0.78, 220, 200, 0],
    [0.9, 230, 90, 0],
    [1.0, 255, 255, 255],
  ],
  mono: [
    [0.0, 4, 6, 9],
    [0.5, 118, 122, 128],
    [1.0, 255, 255, 255],
  ],
  ember: [
    [0.0, 8, 6, 10],
    [0.25, 62, 22, 48],
    [0.5, 140, 44, 52],
    [0.72, 214, 106, 40],
    [0.88, 246, 182, 84],
    [1.0, 255, 248, 226],
  ],
};

/** Builds a 256-entry RGBA lookup table for a palette. */
export function buildPaletteTexture(id: PaletteId): Uint8Array {
  const stops = RAMPS[id] ?? RAMPS.aurora;
  const out = new Uint8Array(256 * 4);

  for (let i = 0; i < 256; i++) {
    const t = i / 255;
    let lower = stops[0];
    let upper = stops[stops.length - 1];
    for (let s = 0; s < stops.length - 1; s++) {
      if (t >= stops[s][0] && t <= stops[s + 1][0]) {
        lower = stops[s];
        upper = stops[s + 1];
        break;
      }
    }
    const span = upper[0] - lower[0];
    const f = span > 0 ? (t - lower[0]) / span : 0;
    out[i * 4 + 0] = Math.round(lower[1] + (upper[1] - lower[1]) * f);
    out[i * 4 + 1] = Math.round(lower[2] + (upper[2] - lower[2]) * f);
    out[i * 4 + 2] = Math.round(lower[3] + (upper[3] - lower[3]) * f);
    out[i * 4 + 3] = 255;
  }
  return out;
}

/** CSS gradient for the same palette, used for the legend. */
export function paletteGradient(id: PaletteId): string {
  const stops = RAMPS[id] ?? RAMPS.aurora;
  const parts = stops.map(
    ([position, r, g, b]) => `rgb(${r} ${g} ${b}) ${(position * 100).toFixed(1)}%`,
  );
  return `linear-gradient(90deg, ${parts.join(', ')})`;
}

/** The colour a given normalised level maps to; used by the 2-D overlays. */
export function samplePalette(id: PaletteId, level: number): [number, number, number] {
  const table = buildPaletteTexture(id);
  const index = Math.max(0, Math.min(255, Math.round(level * 255))) * 4;
  return [table[index], table[index + 1], table[index + 2]];
}
