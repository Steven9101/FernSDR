import { describe, expect, it } from 'vitest';
import { DARK_THEME, drawOverlay, type OverlayOptions, dbStepFor } from './overlay';

it('keeps a trace between offscreen native centres and clips after interpolation', () => {
  const paths: { points: number[][]; clipped: boolean }[] = [];
  let points: number[][] = [], clipped = false;
  const context = {
    strokeStyle: '', fillStyle: '', lineWidth: 1, font: '', textBaseline: '', textAlign: '',
    clearRect() {}, fillRect() {}, fillText() {}, closePath() {}, fill() {},
    beginPath() { points = []; },
    moveTo(x: number, y: number) { points.push([x, y]); },
    lineTo(x: number, y: number) { points.push([x, y]); },
    stroke() { if (this.strokeStyle === 'trace') paths.push({ points: [...points], clipped }); },
    save() {}, restore() { clipped = false; }, rect() {}, clip() { clipped = true; },
    measureText() { return { width: 20 }; },
  };
  const options: OverlayOptions = {
    width: 200, height: 500, dpr: 1, viewLowHz: 9, viewHighHz: 11,
    spectrum: Float32Array.of(-160, 0), peakHold: null, spectrumLowHz: 0, spectrumHighHz: 20,
    spectrumHeight: 100, showSpectrum: true, floorDb: -120, ceilingDb: -40,
    tunedHz: 100, passbandLow: 1, passbandHigh: 2, showBandPlan: false, activeEdge: null,
    theme: { ...DARK_THEME, trace: 'trace' },
  };
  drawOverlay(context as unknown as CanvasRenderingContext2D, options);
  expect(paths).toHaveLength(1);
  expect(paths[0].clipped).toBe(true);
  expect(paths[0].points).toHaveLength(2);
  expect(paths[0].points[0]).toEqual([0, 70]);
  expect(paths[0].points[1][0]).toBe(200);
  expect(paths[0].points[1][1]).toBeCloseTo(30, 8);
  paths.length = 0;
  drawOverlay(context as unknown as CanvasRenderingContext2D, { ...options, viewLowHz: 5, viewHighHz: 15 });
  expect(paths).toEqual([{ points: [[0, 150], [200, -50]], clipped: true }]);
});

describe('dB grid step', () => {
  it('keeps labels at least 20 px apart', () => {
    for (const [range, height] of [[60, 150], [60, 70], [120, 70], [30, 40], [200, 50]] as const) {
      const step = dbStepFor(range, height);
      expect((step / range) * height >= 20 || step === 40).toBe(true);
    }
    // A tall desktop spectrum keeps the fine grid.
    expect(dbStepFor(60, 150)).toBe(10);
    expect(dbStepFor(40, 200)).toBe(5);
  });
});
