/**
 * Draws archive rows into a canvas, newest at the top, in the listener's own
 * palette and levels, so the archive and the live waterfall are read the
 * same way.
 */
import { buildPaletteTexture, type PaletteId } from './palettes';
import { historyRowRange, type Archive } from '../util/history';

export function paintArchive(
  canvas: HTMLCanvasElement,
  archive: Archive,
  from: number,
  to: number,
  height: number,
  palette: PaletteId,
  floor: number,
  ceiling: number,
): void {
  const width = archive.bins;
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d');
  if (!context) return;
  const colours = buildPaletteTexture(palette);
  const image = context.createImageData(width, height);
  const archiveSpan = archive.ceilingDb - archive.floorDb;
  const viewSpan = Math.max(1, ceiling - floor);
  for (let row = 0; row < archive.times.length; row++) {
    const [firstY, lastY] = historyRowRange(archive.times[row], archive.rowMs, from, to, height);
    for (let y = firstY; y < lastY; y++) {
      for (let x = 0; x < width; x++) {
        const db = archive.floorDb + archiveSpan * (archive.rows[row * width + x] / 255);
        const level = Math.max(0, Math.min(1, (db - floor) / viewSpan));
        const source = Math.round(level * 255) * 4;
        const target = (y * width + x) * 4;
        image.data[target] = colours[source];
        image.data[target + 1] = colours[source + 1];
        image.data[target + 2] = colours[source + 2];
        image.data[target + 3] = 255;
      }
    }
  }
  context.putImageData(image, 0, 0);
}
