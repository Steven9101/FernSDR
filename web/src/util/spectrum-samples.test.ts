import { expect, it } from 'vitest';
import { interpolateLevel, resampleLevels } from './spectrum-samples';
import { waterfallLevels } from './waterfall-levels';

it('interpolates between bin centres and clamps only at the row edges', () => {
  const levels = new Float32Array([-120, -80]);
  expect(interpolateLevel(levels, -0.5)).toBe(-120);
  expect(interpolateLevel(levels, 0.5)).toBe(-100);
  expect(interpolateLevel(levels, 1.5)).toBe(-80);
  const view = new Float32Array(4);
  resampleLevels(levels, 7000000, 7000020, 7000009, 7000011, view);
  expect(Array.from(view)).toEqual([-103, -101, -99, -97]);
});

it('excludes an offscreen guard-bin peak from visible auto levels', () => {
  const levels = new Float32Array([-110, -110, -110, -20]);
  const visible = new Float32Array(1024);
  resampleLevels(levels, 0, 40, 12, 18, visible);
  expect(waterfallLevels(visible, new Uint32Array(301))).toEqual([-110, -110]);
});
