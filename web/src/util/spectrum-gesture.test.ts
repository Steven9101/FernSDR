import { describe, expect, it } from 'vitest';
import { spectrumDrag } from './spectrum-gesture';

describe('spectrum drag targets', () => {
  it('keeps a narrow CW signal draggable without competing edge targets', () => {
    for (const touch of [false, true]) {
      expect(spectrumDrag(100, 90, 110, 100, touch, false, false)).toBe('tune');
    }
  });
  it('separates both edges and the tuning region', () => {
    expect(spectrumDrag(100, 104.5, 140.5, 100, false, false, false)).toBe('tune');
    expect(spectrumDrag(104.5, 104.5, 140.5, 100, false, false, false)).toBe('edge-low');
    expect(spectrumDrag(103, 100, 200, 150, true, false, false)).toBe('edge-low');
    expect(spectrumDrag(138, 100, 140, 120, false, false, false)).toBe('edge-high');
    expect(spectrumDrag(120, 100, 140, 120, false, false, false)).toBe('tune');
    expect(spectrumDrag(102, 100, 140, 120, false, false, false)).toBe('edge-low');
  });
  it('uses the ruler for tuning and the waterfall background for panning', () => {
    expect(spectrumDrag(400, 100, 140, 120, false, true, false)).toBe('tune');
    expect(spectrumDrag(400, 100, 140, 120, false, false, false)).toBe('pan');
    expect(spectrumDrag(120, 100, 140, 120, false, true, true)).toBe('pan');
  });
});
