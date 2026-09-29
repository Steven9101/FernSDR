/**
 * Everything drawn over the waterfall: the spectrum trace, the frequency
 * ruler, the band-plan strip and the passband.
 *
 * Canvas2D rather than WebGL, deliberately. These are vector overlays with
 * text, they change only when the view or the latest line changes, and text
 * rendered by the browser is sharper than anything worth doing in a shader.
 */
import { segmentsIn, spotsIn, type SegmentKind, type SpotFrequency } from '../state/bandplan';
import { interpolateLevel } from '../util/spectrum-samples';

// The page's face, as in the admin panel: canvas labels do not inherit CSS.
const LABEL_FAMILY = '"Inter Variable", ui-sans-serif, system-ui, -apple-system, sans-serif';

export interface OverlayTheme {
  grid: string;
  gridStrong: string;
  text: string;
  textDim: string;
  trace: string;
  traceFill: string;
  peak: string;
  passband: string;
  passbandFaint: string;
  passbandEdge: string;
  passbandEdgeActive: string;
  carrier: string;
  segmentColors: Record<SegmentKind, string>;
}

/*
 * The overlay is monochrome too, with two deliberate exceptions.
 *
 * The waterfall underneath is the only thing in the interface allowed a full
 * colour ramp, because its colour IS the measurement. Everything drawn on top
 * of it - the trace, the grid, the passband - is white at varying opacity, so
 * it reads as instrument glass over the data rather than as more data.
 *
 * The exceptions are the tuning controls at the ruler, which have to be
 * findable against any colour the waterfall happens to be showing, and the
 * band plan, whose colours distinguish kinds of allocation and are
 * information rather than decoration. Those are kept, heavily desaturated,
 * and the services around the amateur bands (broadcast, utility, licence-free
 * radio) fainter than the amateur plan drawn over them.
 */
export const DARK_THEME: OverlayTheme = {
  grid: 'rgba(255, 255, 255, 0.07)',
  gridStrong: 'rgba(255, 255, 255, 0.16)',
  text: 'rgba(255, 255, 255, 0.92)',
  textDim: 'rgba(255, 255, 255, 0.55)',
  trace: 'rgba(255, 255, 255, 0.92)',
  traceFill: 'rgba(255, 255, 255, 0.10)',
  peak: 'rgba(255, 255, 255, 0.34)',
  passband: 'rgba(255, 255, 255, 0.14)',
  passbandFaint: 'rgba(255, 255, 255, 0.06)',
  passbandEdge: '#ead65b',
  passbandEdgeActive: '#fff1a3',
  carrier: '#ead65b',
  segmentColors: {
    cw: 'rgba(150, 180, 210, 0.26)',
    digital: 'rgba(180, 165, 210, 0.26)',
    phone: 'rgba(150, 200, 175, 0.24)',
    beacon: 'rgba(215, 195, 150, 0.26)',
    broadcast: 'rgba(210, 165, 155, 0.20)',
    satellite: 'rgba(165, 205, 215, 0.24)',
    fm: 'rgba(170, 210, 150, 0.22)',
    utility: 'rgba(190, 190, 190, 0.16)',
    personal: 'rgba(220, 180, 200, 0.20)',
    other: 'rgba(200, 200, 200, 0.12)',
  },
};

export interface OverlayOptions {
  width: number;
  height: number;
  dpr: number;
  viewLowHz: number;
  viewHighHz: number;

  spectrum: Float32Array | null;
  peakHold: Float32Array | null;
  spectrumLowHz: number;
  spectrumHighHz: number;
  spectrumHeight: number;
  showSpectrum: boolean;

  floorDb: number;
  ceilingDb: number;

  /** The carrier the receiver is tuned to; the passband is drawn around it. */
  tunedHz: number;
  /**
   * Where the marker belongs. Identical to `tunedHz` except in CW, where the
   * signal sits one pitch away from the carrier and the marker must stand on
   * the signal - which is also the number the readout shows.
   */
  markerHz?: number;
  passbandLow: number;
  passbandHigh: number;

  showBandPlan: boolean;
  /** Highlighted while the user drags a filter edge. */
  activeEdge: 'low' | 'high' | null;
  theme: OverlayTheme;
}

export const BAND_PLAN_HEIGHT = 16;
export const PASSBAND_HEIGHT = 20;
export const RULER_HEIGHT = PASSBAND_HEIGHT + 24;

/** A spectrum shorter than this shows nothing worth reading, and is not drawn. */
export const MIN_TRACE = 28;

export function overlayLayout(height: number, requestedSpectrum: number, showSpectrum: boolean, showBandPlan: boolean,
                              maxShare = 0.34) {
  const bandPlanHeight = showBandPlan && height >= 110 ? BAND_PLAN_HEIGHT : 0;
  const available = Math.max(0, height - bandPlanHeight - RULER_HEIGHT - 48);
  const trace = showSpectrum ? Math.min(requestedSpectrum, height * maxShare, available) : 0;
  const spectrumHeight = trace >= MIN_TRACE ? trace : 0;
  const rulerTop = spectrumHeight + bandPlanHeight;
  return { spectrumHeight, bandPlanHeight, rulerTop, waterfallTop: rulerTop + RULER_HEIGHT };
}

/** Picks a 1-2-5 tick spacing that leaves at least `minPixels` between ticks. */
function chooseTickSpacing(spanHz: number, pixels: number, minPixels: number): number {
  const target = (spanHz * minPixels) / Math.max(pixels, 1);
  const magnitude = 10 ** Math.floor(Math.log10(Math.max(target, 1e-6)));
  for (const step of [1, 2, 5, 10]) {
    if (magnitude * step >= target) return magnitude * step;
  }
  return magnitude * 10;
}

function formatTick(hz: number, spacing: number): string {
  if (spacing >= 1e6) return `${(hz / 1e6).toFixed(0)}M`;
  if (spacing >= 1e3) {
    const mhz = hz / 1e6;
    return spacing >= 1e5 ? `${mhz.toFixed(1)}` : `${mhz.toFixed(3)}`;
  }
  return `${(hz / 1e6).toFixed(6).replace(/0+$/, '')}`;
}

export function drawOverlay(context: CanvasRenderingContext2D, options: OverlayOptions): void {
  const { width, height } = options;
  context.clearRect(0, 0, width, height);

  const span = options.viewHighHz - options.viewLowHz;
  if (span <= 0) return;
  const toX = (hz: number) => ((hz - options.viewLowHz) / span) * width;

  const spectrumHeight = options.showSpectrum ? options.spectrumHeight : 0;
  const bandPlanTop = spectrumHeight;
  const bandPlanHeight = options.showBandPlan ? BAND_PLAN_HEIGHT : 0;
  const rulerTop = bandPlanTop + bandPlanHeight;

  if (options.showSpectrum) {
    drawSpectrum(context, options, spectrumHeight, toX);
  }
  if (options.showBandPlan) {
    drawBandPlan(context, options, bandPlanTop, bandPlanHeight, toX);
  }
  drawRuler(context, options, rulerTop + PASSBAND_HEIGHT, toX);
  drawPassband(context, options, rulerTop, toX);
}

/**
 * The dB grid's step: the smallest of 5, 10 or 20 dB that leaves its labels
 * 20 px apart. On a phone the spectrum is short, and at 5 dB its labels ran
 * into each other.
 */
export function dbStepFor(rangeDb: number, heightPx: number): number {
  for (const step of [5, 10, 20]) if ((step / rangeDb) * heightPx >= 20) return step;
  return 40;
}

function drawSpectrum(
  context: CanvasRenderingContext2D,
  options: OverlayOptions,
  areaHeight: number,
  toX: (hz: number) => number,
): void {
  const { theme, width } = options;
  const range = Math.max(options.ceilingDb - options.floorDb, 1);
  const toY = (db: number) => areaHeight - (db - options.floorDb) / range * areaHeight;

  // Horizontal dB grid.
  context.lineWidth = 1;
  context.font = `10px ${LABEL_FAMILY}`;
  context.textBaseline = 'bottom';
  const dbStep = dbStepFor(range, areaHeight);
  const firstDb = Math.ceil(options.floorDb / dbStep) * dbStep;
  for (let db = firstDb; db <= options.ceilingDb; db += dbStep) {
    const y = Math.round(toY(db)) + 0.5;
    context.strokeStyle = theme.grid;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
    // A label sitting above the topmost gridline has nowhere to go and gets
    // sliced in half by the edge of the canvas; the gridline alone is enough.
    if (y > 12) {
      context.fillStyle = theme.textDim;
      context.fillText(db < 0 ? `\u2212${-db}` : `${db}`, 4, y - 2);
    }
  }

  const spectrum = options.spectrum;
  if (!spectrum || spectrum.length === 0) return;

  const spectrumSpan = options.spectrumHighHz - options.spectrumLowHz;
  if (spectrumSpan <= 0) return;
  const visibleLow = Math.max(options.viewLowHz, options.spectrumLowHz);
  const visibleHigh = Math.min(options.viewHighHz, options.spectrumHighHz);
  if (visibleHigh <= visibleLow) return;
  context.save();
  context.beginPath();
  context.rect(0, 0, width, areaHeight);
  context.clip();

  // The trace is drawn from the line's own span, so it stays aligned with the
  // view even if the user panned since it arrived.
  const buildPath = (data: Float32Array) => {
    context.beginPath();
    const first = (visibleLow - options.spectrumLowHz) / spectrumSpan * data.length - 0.5;
    const last = (visibleHigh - options.spectrumLowHz) / spectrumSpan * data.length - 0.5;
    // A view can fit between two native FFT centres. Its trace still covers
    // the screen; clip the interpolated segment instead of dropping both
    // endpoints because their centres happen to be outside the viewport.
    context.moveTo(toX(visibleLow), toY(interpolateLevel(data, first)));
    for (let i = Math.max(0, Math.floor(first) + 1); i < Math.min(data.length, Math.ceil(last)); i++) {
      const hz = options.spectrumLowHz + (spectrumSpan * (i + 0.5)) / data.length;
      context.lineTo(toX(hz), toY(data[i]));
    }
    context.lineTo(toX(visibleHigh), toY(interpolateLevel(data, last)));
    return true;
  };

  if (options.peakHold) {
    context.strokeStyle = theme.peak;
    context.lineWidth = 1;
    if (buildPath(options.peakHold)) context.stroke();
  }

  if (buildPath(spectrum)) {
    context.lineTo(toX(visibleHigh), areaHeight);
    context.lineTo(toX(visibleLow), areaHeight);
    context.closePath();
    context.fillStyle = theme.traceFill;
    context.fill();

    buildPath(spectrum);
    context.strokeStyle = theme.trace;
    context.lineWidth = 1.25;
    context.stroke();
  }
  context.restore();
}

function drawBandPlan(
  context: CanvasRenderingContext2D,
  options: OverlayOptions,
  top: number,
  height: number,
  toX: (hz: number) => number,
): void {
  const { theme } = options;
  const segments = segmentsIn(options.viewLowHz, options.viewHighHz);

  context.font = `500 10px ${LABEL_FAMILY}`;
  context.textBaseline = 'middle';

  // Where labels already are, so a spot's name is left out rather than drawn
  // over a segment's ("WS40 m digital").
  const taken: [number, number][] = [];

  // Fills first, the services under the amateur plan; within a layer the
  // order segmentsIn returns puts containers before what they contain...
  for (const segment of [...segments.filter((s) => s.layer === 'service'), ...segments.filter((s) => s.layer === 'amateur')]) {
    const x0 = Math.max(0, toX(segment.low));
    const x1 = Math.min(options.width, toX(segment.high));
    if (x1 - x0 < 1) continue;
    // Rows nobody has confirmed against a current source yet are drawn, but
    // at half strength, so nobody takes their edges as settled.
    context.globalAlpha = segment.unverified ? 0.5 : 1;
    context.fillStyle = theme.segmentColors[segment.kind];
    context.fillRect(x0, top, x1 - x0, height);
  }
  context.globalAlpha = 1;

  // ...then names: the amateur plan's before the services', which only get
  // the room left over.
  for (const layer of ['amateur', 'service'] as const) {
    for (const segment of segments) {
      if (segment.layer !== layer) continue;
      const x0 = Math.max(0, toX(segment.low));
      const x1 = Math.min(options.width, toX(segment.high));
      if (x1 - x0 <= 64) continue;
      const label = segment.band ? `${segment.band} ${segment.label}` : segment.label;
      const from = x0 + 5;
      const to = Math.min(x1, from + context.measureText(label).width);
      if (taken.some(([a, b]) => from < b + 4 && to + 4 > a)) continue;
      context.fillStyle = layer === 'amateur' ? theme.text : theme.textDim;
      context.save();
      context.beginPath();
      context.rect(x0, top, x1 - x0, height);
      context.clip();
      context.fillText(label, from, top + height / 2 + 0.5);
      context.restore();
      taken.push([from, to]);
    }
  }

  // Spot markers: the frequencies people are actually looking for.
  const spots: SpotFrequency[] = spotsIn(options.viewLowHz, options.viewHighHz);
  context.textBaseline = 'top';
  for (const spot of spots) {
    const x = toX(spot.hz);
    // A marker without a mode (FM broadcast, ADS-B) is only a signpost; the
    // stronger tick is the one a tap tunes to.
    context.strokeStyle = spot.mode ? theme.gridStrong : theme.grid;
    context.beginPath();
    context.moveTo(Math.round(x) + 0.5, top);
    context.lineTo(Math.round(x) + 0.5, top + height);
    context.stroke();
    if (options.viewHighHz - options.viewLowHz < 300_000) {
      const from = x + 3;
      const to = from + context.measureText(spot.label).width;
      if (taken.some(([a, b]) => from < b + 4 && to + 4 > a)) continue;
      taken.push([from, to]);
      context.fillStyle = theme.textDim;
      context.fillText(spot.label, from, top + 2);
    }
  }
}

function drawRuler(
  context: CanvasRenderingContext2D,
  options: OverlayOptions,
  top: number,
  toX: (hz: number) => number,
): void {
  const { theme, width } = options;
  const span = options.viewHighHz - options.viewLowHz;
  const spacing = chooseTickSpacing(span, width, 90);

  context.font = `11px ${LABEL_FAMILY}`;
  context.textBaseline = 'top';
  context.textAlign = 'center';

  const first = Math.ceil(options.viewLowHz / spacing) * spacing;
  for (let hz = first; hz <= options.viewHighHz; hz += spacing) {
    const x = Math.round(toX(hz)) + 0.5;
    context.strokeStyle = theme.gridStrong;
    context.beginPath();
    context.moveTo(x, top);
    context.lineTo(x, top + 6);
    context.stroke();
    context.fillStyle = theme.textDim;
    context.fillText(formatTick(hz, spacing), x, top + 7);
  }

  // Minor ticks, when there is room for them.
  const minor = spacing / 5;
  if ((minor / span) * width > 8) {
    context.strokeStyle = theme.grid;
    const firstMinor = Math.ceil(options.viewLowHz / minor) * minor;
    for (let hz = firstMinor; hz <= options.viewHighHz; hz += minor) {
      const x = Math.round(toX(hz)) + 0.5;
      context.beginPath();
      context.moveTo(x, top);
      context.lineTo(x, top + 3);
      context.stroke();
    }
  }
  context.textAlign = 'left';
}

function drawPassband(
  context: CanvasRenderingContext2D,
  options: OverlayOptions,
  rulerTop: number,
  toX: (hz: number) => number,
): void {
  const { theme, height } = options;
  const lowHz = options.tunedHz + options.passbandLow;
  const highHz = options.tunedHz + options.passbandHigh;

  const rawX0 = toX(lowHz);
  const rawX1 = toX(highHz);
  const carrierX = Math.round(toX(options.markerHz ?? options.tunedHz)) + 0.5;

  // Keep the filter at its actual frequency width. The marker and handles
  // have their own visible size and must not imply a wider received band.
  const x0 = rawX0;
  const x1 = rawX1;

  const spectrumBottom = options.showSpectrum ? options.spectrumHeight : 0;

  context.fillStyle = theme.passband;
  context.fillRect(x0, 0, x1 - x0, spectrumBottom);

  // Show an alignment guide only while a cutoff is being dragged. Persistent
  // vertical lines can be mistaken for carriers in the recorded spectrum.
  if (options.activeEdge) {
    context.fillStyle = theme.passbandFaint;
    context.fillRect(x0, spectrumBottom, x1 - x0, height - spectrumBottom);
    const x = options.activeEdge === 'low' ? x0 : x1;
    context.strokeStyle = theme.passbandEdgeActive;
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(Math.round(x) + 0.5, 0);
    context.lineTo(Math.round(x) + 0.5, height);
    context.stroke();
  }

  // The carrier tick is separate from the two filter cutoffs. In CW it marks
  // the signal frequency from the readout, not the beat-frequency oscillator.
  context.strokeStyle = theme.carrier;
  context.lineWidth = 1;
  context.beginPath();
  context.moveTo(carrierX, rulerTop + 1);
  context.lineTo(carrierX, rulerTop + PASSBAND_HEIGHT);
  context.stroke();

  // A compact envelope joins the two cutoffs into one control. Its shoulders
  // are a grab affordance, not a plot of the filter response. Each sloped end
  // crosses its exact cutoff at the lane's midpoint; narrow filters are never
  // widened just to make the drawing more visible.
  const shoulder = Math.min(5, Math.max(0, x1 - x0) / 4);
  const top = rulerTop + 3;
  const bottom = rulerTop + PASSBAND_HEIGHT - 3;
  context.beginPath();
  context.moveTo(x0 - shoulder, bottom);
  context.lineTo(x0 + shoulder, top);
  context.lineTo(x1 - shoulder, top);
  context.lineTo(x1 + shoulder, bottom);
  context.strokeStyle = theme.passbandEdge;
  context.lineWidth = 1.5;
  context.stroke();
  if (options.activeEdge) {
    context.beginPath();
    if (options.activeEdge === 'low') {
      context.moveTo(x0 - shoulder, bottom);
      context.lineTo(x0 + shoulder, top);
    } else {
      context.moveTo(x1 - shoulder, top);
      context.lineTo(x1 + shoulder, bottom);
    }
    context.strokeStyle = theme.passbandEdgeActive;
    context.lineWidth = 2;
    context.stroke();
  }

  if (options.activeEdge) {
    const bandwidth = options.passbandHigh - options.passbandLow;
    const cutoff = options.activeEdge === 'low' ? lowHz : highHz;
    const label = `${(cutoff / 1000).toFixed(3)} kHz / ${Math.round(bandwidth)} Hz`;
    context.font = `10px ${LABEL_FAMILY}`;
    context.textAlign = 'left';
    context.textBaseline = 'middle';
    const labelWidth = context.measureText(label).width;
    const edgeX = options.activeEdge === 'low' ? x0 : x1;
    const outside = x1 + 10 + labelWidth < options.width ? x1 + 10
      : x0 - labelWidth - 10 >= 4 ? x0 - labelWidth - 10 : edgeX + 10;
    const labelX = Math.max(4, Math.min(options.width - labelWidth - 4, outside));
    context.fillStyle = 'rgba(7, 10, 14, 0.95)';
    context.fillRect(labelX - 3, rulerTop + 1, labelWidth + 6, PASSBAND_HEIGHT - 2);
    context.fillStyle = theme.text;
    context.fillText(label, labelX, rulerTop + PASSBAND_HEIGHT / 2);
  }
}
