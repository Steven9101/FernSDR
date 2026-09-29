/**
 * Canvas2D fallback for the waterfall.
 *
 * Used when WebGL2 is unavailable - older iOS, locked-down browsers, some
 * remote-desktop setups. It keeps the same ring-buffer structure so scrolling
 * stays two blits per frame rather than a redraw of the whole history.
 *
 * Adjacent rows with the same frequency range are drawn together. A settled
 * view needs at most two blits, while panning keeps old rows at the frequency
 * where they were received instead of clearing the display on every update.
 */
import { buildPaletteTexture, type PaletteId } from './palettes';
import type { WaterfallLine } from './waterfall-gl';
import { interpolateLevel } from '../util/spectrum-samples';

const HISTORY_ROWS = 1024;
const TEXTURE_WIDTH = 2048;

export class WaterfallFallbackRenderer {
  private readonly buffer: HTMLCanvasElement;
  private readonly bufferContext: CanvasRenderingContext2D;
  private readonly context: CanvasRenderingContext2D;
  private readonly rowImage: ImageData;
  private palette = buildPaletteTexture('aurora');
  private paletteId: PaletteId = 'aurora';

  private writeRow = 0;
  private floorDb = -115;
  private ceilingDb = -35;
  private referenceHz = 0;
  private count = 0;
  private lows = new Float64Array(HISTORY_ROWS);
  private highs = new Float64Array(HISTORY_ROWS);
  private pixelRatio = 1;

  constructor(private readonly canvas: HTMLCanvasElement) {
    const context = canvas.getContext('2d', { alpha: false });
    if (!context) throw new Error('2-D canvas is not available');
    this.context = context;

    this.buffer = document.createElement('canvas');
    this.buffer.width = TEXTURE_WIDTH;
    this.buffer.height = HISTORY_ROWS;
    const bufferContext = this.buffer.getContext('2d', { alpha: false, willReadFrequently: false });
    if (!bufferContext) throw new Error('2-D canvas is not available');
    this.bufferContext = bufferContext;
    this.bufferContext.fillStyle = '#080b10';
    this.bufferContext.fillRect(0, 0, TEXTURE_WIDTH, HISTORY_ROWS);

    this.rowImage = this.bufferContext.createImageData(TEXTURE_WIDTH, 1);
  }

  setPalette(id: PaletteId): void {
    if (id === this.paletteId) return;
    this.paletteId = id;
    this.palette = buildPaletteTexture(id);
  }

  setLevels(floorDb: number, ceilingDb: number): void {
    this.floorDb = floorDb;
    this.ceilingDb = ceilingDb;
  }

  setPixelRatio(ratio: number): void {
    this.pixelRatio = Math.max(1, ratio);
  }

  setReference(hz: number): void {
    if (hz === this.referenceHz) return;
    this.referenceHz = hz;
    this.clear();
  }

  setEmptyColor(): void {
    // The fallback always fills.
  }

  clear(): void {
    this.bufferContext.fillStyle = '#080b10';
    this.bufferContext.fillRect(0, 0, TEXTURE_WIDTH, HISTORY_ROWS);
    this.writeRow = 0;
    this.count = 0;
  }

  pushLine(line: WaterfallLine): void {
    this.lows[this.writeRow] = line.lowHz;
    this.highs[this.writeRow] = line.highHz;
    this.count = Math.min(HISTORY_ROWS, this.count + 1);

    const data = this.rowImage.data;
    const range = Math.max(this.ceilingDb - this.floorDb, 1);
    // Stretch the line across the full buffer width so the blit needs no
    // horizontal scaling per row.
    for (let x = 0; x < TEXTURE_WIDTH; x++) {
      const db = interpolateLevel(line.levels, (x + 0.5) * line.width / TEXTURE_WIDTH - 0.5);
      const level = Math.max(0, Math.min(1, (db - this.floorDb) / range));
      const index = Math.round(level * 255) * 4;
      const offset = x * 4;
      data[offset] = this.palette[index];
      data[offset + 1] = this.palette[index + 1];
      data[offset + 2] = this.palette[index + 2];
      data[offset + 3] = 255;
    }
    this.bufferContext.putImageData(this.rowImage, 0, this.writeRow);
    this.writeRow = (this.writeRow + 1) % HISTORY_ROWS;
  }

  render(viewLowHz: number, viewHighHz: number, top = 0): void {
    const width = this.canvas.width;
    const height = this.canvas.height;
    if (width === 0 || height === 0) return;

    this.context.imageSmoothingEnabled = false;
    this.context.fillStyle = '#080b10';
    this.context.fillRect(0, 0, width, height);

    const span = viewHighHz - viewLowHz;
    if (span <= 0) return;
    const rows = Math.min(this.count, Math.ceil(Math.max(0, height - top) / this.pixelRatio));
    this.context.save();
    this.context.translate(0, height);
    this.context.scale(1, -1);
    for (let age = 0; age < rows;) {
      const row = (this.writeRow - 1 - age + HISTORY_ROWS) % HISTORY_ROWS;
      const low = this.lows[row];
      const high = this.highs[row];
      let count = 1;
      while (age + count < rows && row - count >= 0 &&
             this.lows[row - count] === low && this.highs[row - count] === high) count++;
      const x = (low - viewLowHz) / span * width;
      const drawnWidth = (high - low) / span * width;
      this.context.drawImage(this.buffer, 0, row - count + 1, TEXTURE_WIDTH, count,
        x, height - top - (age + count) * this.pixelRatio, drawnWidth, count * this.pixelRatio);
      age += count;
    }
    this.context.restore();
  }

  dispose(): void {
    // Nothing retained beyond the canvases, which the GC handles.
  }
}
