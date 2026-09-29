import { describe, expect, it } from 'vitest';
import { place } from './place';

const viewport = { width: 400, height: 300 };
const box = { width: 100, height: 30 };

describe('place', () => {
  it('centres above the anchor with the gap', () => {
    expect(place({ left: 150, top: 100, width: 100, height: 20 }, box, 'top', viewport)).toEqual({ x: 150, y: 64, side: 'top' });
  });

  it('goes below when there is no room above', () => {
    expect(place({ left: 150, top: 10, width: 100, height: 20 }, box, 'top', viewport)).toEqual({ x: 150, y: 36, side: 'bottom' });
  });

  it('stays inside the viewport at the edges', () => {
    expect(place({ left: 0, top: 100, width: 20, height: 20 }, box, 'top', viewport).x).toBe(8);
    expect(place({ left: 390, top: 100, width: 10, height: 20 }, box, 'bottom', viewport).x).toBe(292);
  });

  it('aligns to the start for panels, and sits beside for left and right', () => {
    expect(place({ left: 50, top: 200, width: 40, height: 20 }, box, 'top', viewport, { gap: 8, align: 'start' })).toEqual({ x: 50, y: 162, side: 'top' });
    expect(place({ left: 380, top: 100, width: 10, height: 20 }, box, 'right', viewport).side).toBe('left');
  });
});
