/** Frequency geometry shared by wheel, touch and keyboard navigation. */
export interface FrequencyRange { lowHz: number; highHz: number }
export interface BandRange { low: number; high: number }

export function clampSpan(span: number, band: BandRange | null): number {
  const maximum = band ? band.high - band.low : Number.MAX_SAFE_INTEGER;
  return Math.max(Math.min(2000, maximum), Math.min(maximum, span));
}

export function constrainViewport(lowHz: number, highHz: number, band: BandRange | null): FrequencyRange | null {
  if (!Number.isFinite(lowHz) || !Number.isFinite(highHz) || highHz <= lowHz) return null;
  const span = clampSpan(highHz - lowHz, band);
  let low = (lowHz + highHz - span) / 2;
  if (band) low = Math.max(band.low, Math.min(band.high - span, low));
  return { lowHz: low, highHz: low + span };
}

/** Clamp the span BEFORE anchoring it, so reaching a zoom limit cannot pan. */
export function zoomViewport(view: FrequencyRange, fraction: number, factor: number, band: BandRange | null): FrequencyRange {
  const anchor = view.lowHz + (view.highHz - view.lowHz) * fraction;
  return anchoredViewport(anchor, fraction, (view.highHz - view.lowHz) * factor, band);
}

export function anchoredViewport(anchor: number, fraction: number, span: number, band: BandRange | null): FrequencyRange {
  const bounded = clampSpan(span, band);
  const low = anchor - bounded * fraction;
  return constrainViewport(low, low + bounded, band) ?? { lowHz: low, highHz: low + bounded };
}

/** Wheel units may be pixels, lines or pages; trackpads often emit subpixels. */
export function wheelPixels(delta: number, mode: number, pageSize: number): number {
  return delta * (mode === 1 ? 16 : mode === 2 ? pageSize : 1);
}

export function wheelZoomFactor(pixels: number): number {
  return Math.exp(Math.max(-240, Math.min(240, pixels)) * 0.00165);
}
