/** Sample positions use bin centres: index 0 is the centre of the first bin. */
export function interpolateLevel(levels: Float32Array, position: number): number {
  const clamped = Math.max(0, Math.min(levels.length - 1, position));
  const left = Math.floor(clamped);
  const fraction = clamped - left;
  return levels[left] * (1 - fraction) + levels[Math.min(left + 1, levels.length - 1)] * fraction;
}

export function resampleLevels(levels: Float32Array, low: number, high: number,
  viewLow: number, viewHigh: number, out: Float32Array): void {
  const scale = levels.length / (high - low);
  const step = (viewHigh - viewLow) / out.length;
  for (let i = 0; i < out.length; i++) {
    out[i] = interpolateLevel(levels, (viewLow + (i + 0.5) * step - low) * scale - 0.5);
  }
}
