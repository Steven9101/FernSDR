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
 *
 * The buffer holds colours, so each row's levels are kept beside it: a new
 * palette, or new floor and ceiling (which automatic levels move all the
 * time), recolours the rows on screen rather than only those still to come,
 * and the same signal keeps one colour down the whole waterfall.
 */
import { buildPaletteTexture, type PaletteId } from './palettes';
import type { WaterfallLine } from './waterfall-gl';
import { interpolateLevel } from '../util/spectrum-samples';

const HISTORY_ROWS = 1024;
const TEXTURE_WIDTH = 2048;
/** Levels are kept in hundredths of a dB: 4 MB for the whole history. */
const STORED_PER_DB = 100;
/**
 * How far the floor or ceiling moves before the rows are recoloured. Under a
 * colour step of the palette on any usual range, and automatic levels creep
 * by less than this a row, so they recolour every few seconds at most rather
 * than on every line.
 */
const RECOLOUR_AFTER_DB = 0.5;

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
  /** The span of the view last drawn, Hz; 0 before the first frame. */
  private viewSpanHz = 0;
  /** Each row's levels as drawn into the buffer, row after row. */
  private readonly stored = new Int16Array(HISTORY_ROWS * TEXTURE_WIDTH);
  /** Counts palette and level changes; each row notes the one it is coloured in. */
  private mapping = 1;
  private readonly rowMapping = new Uint32Array(HISTORY_ROWS);

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
    this.mapping++;
  }

  setLevels(floorDb: number, ceilingDb: number): void {
    if (Math.abs(floorDb - this.floorDb) < RECOLOUR_AFTER_DB && Math.abs(ceilingDb - this.ceilingDb) < RECOLOUR_AFTER_DB) return;
    this.floorDb = floorDb;
    this.ceilingDb = ceilingDb;
    this.mapping++;
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

    const stored = this.stored;
    const start = this.writeRow * TEXTURE_WIDTH;
    const cells = line.width;
    const levels = line.levels;
    const cellsPerColumn = cells / TEXTURE_WIDTH;
    // How many of the line's cells one pixel of the canvas spans, as the
    // WebGL renderer works it out. A wide line arrives already reduced to
    // about a cell a pixel by keeping each cell's peak; blending those again
    // between neighbours, and the blit then picking one column of the buffer
    // per pixel, took a carrier one cell wide down by up to tens of dB
    // depending only on where it fell. So each column keeps the strongest
    // cell under the pixel it will be drawn into, and the smooth blend is
    // kept for zooming in, where a cell is several pixels wide.
    const lineSpan = Math.max(line.highHz - line.lowHz, 1);
    const pixels = Math.max(this.canvas.width, 1) * (this.viewSpanHz > 0 ? lineSpan / this.viewSpanHz : 1);
    const perPixel = cells / pixels;
    const half = 0.5 * Math.max(perPixel, cellsPerColumn);
    // Stretch the line across the full buffer width so the blit needs no
    // horizontal scaling per row.
    for (let x = 0; x < TEXTURE_WIDTH; x++) {
      const centre = (x + 0.5) * cellsPerColumn;
      let db: number;
      if (perPixel < 0.3) {
        db = interpolateLevel(levels, centre - 0.5);
      } else {
        const first = Math.floor(Math.max(centre - half, 0));
        const last = Math.min(Math.max(Math.floor(Math.min(centre + half, cells) - 1e-4), first), cells - 1);
        db = levels[first];
        for (let cell = first + 1; cell <= last; cell++) if (levels[cell] > db) db = levels[cell];
      }
      stored[start + x] = Math.max(-32768, Math.min(32767, Math.round(db * STORED_PER_DB)));
    }
    this.colourRow(this.writeRow);
    this.writeRow = (this.writeRow + 1) % HISTORY_ROWS;
  }

  /** Colours a row of the buffer from its kept levels, in the palette and levels now in use. */
  private colourRow(row: number): void {
    const data = this.rowImage.data;
    const stored = this.stored;
    const start = row * TEXTURE_WIDTH;
    const floor = this.floorDb * STORED_PER_DB;
    const scale = 255 / (Math.max(this.ceilingDb - this.floorDb, 1) * STORED_PER_DB);
    const palette = this.palette;
    for (let x = 0; x < TEXTURE_WIDTH; x++) {
      const index = Math.round(Math.max(0, Math.min(255, (stored[start + x] - floor) * scale))) * 4;
      const offset = x * 4;
      data[offset] = palette[index];
      data[offset + 1] = palette[index + 1];
      data[offset + 2] = palette[index + 2];
      data[offset + 3] = 255;
    }
    this.bufferContext.putImageData(this.rowImage, 0, row);
    this.rowMapping[row] = this.mapping;
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
    this.viewSpanHz = span;
    const rows = Math.min(this.count, Math.ceil(Math.max(0, height - top) / this.pixelRatio));
    for (let age = 0; age < rows; age++) {
      const row = (this.writeRow - 1 - age + HISTORY_ROWS) % HISTORY_ROWS;
      if (this.rowMapping[row] !== this.mapping) this.colourRow(row);
    }
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
