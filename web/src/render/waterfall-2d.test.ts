import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { buildPaletteTexture } from './palettes';
import { WaterfallFallbackRenderer } from './waterfall-2d';

/**
 * A 2-D context that keeps what is put into it: the ring buffer's rows by
 * their index, and the canvas's blits.
 */
class FakeContext {
  rows = new Map<number, Uint8ClampedArray>();
  blits: { sy: number; sh: number; dx: number; dw: number }[] = [];
  fillStyle = '';
  imageSmoothingEnabled = true;
  fillRect() {}
  save() {}
  restore() {}
  translate() {}
  scale() {}
  createImageData(width: number, height: number) {
    return { width, height, data: new Uint8ClampedArray(width * height * 4) };
  }
  putImageData(image: { data: Uint8ClampedArray }, _x: number, y: number) {
    this.rows.set(y, image.data.slice());
  }
  drawImage(_source: unknown, _sx: number, sy: number, _sw: number, sh: number, dx: number, _dy: number, dw: number) {
    this.blits.push({ sy, sh, dx, dw });
  }
}

let buffer: FakeContext;
let screen: FakeContext;

function renderer(width = 1000, height = 400) {
  screen = new FakeContext();
  const canvas = { width, height, getContext: () => screen } as unknown as HTMLCanvasElement;
  return new WaterfallFallbackRenderer(canvas);
}

beforeEach(() => {
  buffer = new FakeContext();
  vi.stubGlobal('document', { createElement: () => ({ width: 0, height: 0, getContext: () => buffer }) });
});
afterEach(() => vi.unstubAllGlobals());

const TEXTURE_WIDTH = 2048;

/** The colour the palette gives `db` with the default levels. */
function colourOf(db: number, floor = -115, ceiling = -35, palette = buildPaletteTexture('aurora')) {
  const level = Math.max(0, Math.min(1, (db - floor) / (ceiling - floor)));
  const index = Math.round(level * 255) * 4;
  return [palette[index], palette[index + 1], palette[index + 2]];
}

describe('Canvas2D waterfall', () => {
  it('keeps a peak-held carrier one cell wide at its level wherever it falls', () => {
    // As the viewport sends it: about a cell per pixel of a 1000 px canvas.
    const cells = 992;
    const pixels = 1000;
    const draw = renderer(pixels);
    draw.render(14_000_000, 14_992_000);
    let weakest = Infinity;
    for (let carrier = 0; carrier < cells; carrier += 7) {
      const levels = new Float32Array(cells).fill(-100);
      levels[carrier] = -60;
      draw.pushLine({ lowHz: 14_000_000, highHz: 14_992_000, width: cells, levels });
      const row = buffer.rows.get(0)!;
      buffer.rows.clear();
      draw.clear();
      // The blit draws the buffer's width into the canvas's with nearest
      // sampling: the column each pixel shows.
      let strongest = -Infinity;
      const target = colourOf(-60);
      for (let pixel = 0; pixel < pixels; pixel++) {
        const column = Math.floor(((pixel + 0.5) * TEXTURE_WIDTH) / pixels);
        const rgb = [row[column * 4], row[column * 4 + 1], row[column * 4 + 2]];
        if (rgb.every((value, i) => value === target[i])) strongest = -60;
      }
      weakest = Math.min(weakest, strongest);
    }
    expect(weakest).toBe(-60);
  });

  it('still blends between cells zoomed in, where one cell spans several pixels', () => {
    const draw = renderer(1000);
    draw.render(14_000_000, 14_100_000);
    const levels = new Float32Array(100).fill(-100);
    levels[50] = -60;
    draw.pushLine({ lowHz: 14_000_000, highHz: 14_100_000, width: 100, levels });
    const row = buffer.rows.get(0)!;
    // Between the carrier's cell and its neighbour, a level between the two.
    const column = Math.round(51 * TEXTURE_WIDTH / 100);
    const rgb = [row[column * 4], row[column * 4 + 1], row[column * 4 + 2]];
    expect(rgb).not.toEqual(colourOf(-60));
    expect(rgb).not.toEqual(colourOf(-100));
  });
});
