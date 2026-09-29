/** Percentiles at the codec's 1 dB precision, without sorting a copy. */
export function waterfallLevels(levels: Float32Array, counts: Uint32Array): [number, number] {
  counts.fill(0);
  for (let i = 0; i < levels.length; i++) counts[Math.max(0, Math.min(counts.length - 1, Math.round(levels[i]) + 200))]++;
  const noiseRank = Math.floor((levels.length - 1) * 0.25);
  const peakRank = Math.floor((levels.length - 1) * 0.995);
  let seen = 0;
  let noise = -200;
  for (let i = 0; i < counts.length; i++) {
    const next = seen + counts[i];
    if (seen <= noiseRank && next > noiseRank) noise = i - 200;
    if (next > peakRank) return [noise, i - 200];
    seen = next;
  }
  return [-200, -200];
}
