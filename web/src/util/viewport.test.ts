import { describe, expect, it } from 'vitest';
import { anchoredViewport, constrainViewport, wheelPixels, wheelZoomFactor, zoomViewport } from './viewport';

const band = { low: 14_000_000, high: 14_350_000 };
const view = { lowHz: 14_100_000, highHz: 14_200_000 };

describe('frequency navigation', () => {
  it('keeps the frequency under an off-centre pointer fixed', () => {
    const next = zoomViewport(view, 0.2, 0.5, band);
    expect(next.lowHz + (next.highHz - next.lowHz) * 0.2).toBe(14_120_000);
  });

  it('does not creep sideways when zooming repeatedly at the minimum span', () => {
    const minimum = { lowHz: 14_100_000, highHz: 14_102_000 };
    let next = minimum;
    for (let i = 0; i < 100; i++) next = zoomViewport(next, 0.1, 0.5, band);
    expect(next).toEqual(minimum);
  });

  it('moves the pinch anchor with the midpoint of the fingers', () => {
    const next = anchoredViewport(14_120_000, 0.3, 50_000, band);
    expect(next.lowHz + 0.3 * 50_000).toBe(14_120_000);
  });

  it('handles bands narrower than the normal minimum and rejects invalid input', () => {
    expect(constrainViewport(20, 30, { low: 0, high: 100 })).toEqual({ lowHz: 0, highHz: 100 });
    expect(constrainViewport(NaN, 100, band)).toBeNull();
    expect(constrainViewport(0, Infinity, band)).toBeNull();
    expect(constrainViewport(100, 0, band)).toBeNull();
    expect(constrainViewport(0, 1e9, band)).toEqual({ lowHz: band.low, highHz: band.high });
  });

  it('normalises line and page wheels and preserves small trackpad movements', () => {
    expect(wheelPixels(3, 1, 800)).toBe(48);
    expect(wheelPixels(1, 2, 800)).toBe(800);
    expect(wheelZoomFactor(0)).toBe(1);
    expect(wheelZoomFactor(0.5)).toBeLessThan(1.001);
    expect(wheelZoomFactor(20) * wheelZoomFactor(-20)).toBeCloseTo(1, 12);
    expect(wheelZoomFactor(10) ** 10).toBeCloseTo(wheelZoomFactor(100), 12);
  });
});
