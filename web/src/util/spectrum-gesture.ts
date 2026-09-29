export type DragKind = 'pan' | 'tune' | 'edge-low' | 'edge-high';

export function spectrumDrag(x: number, low: number, high: number, marker: number,
  touch: boolean, ruler: boolean, panOnly: boolean): DragKind {
  if (panOnly) return 'pan';
  const width = high - low;
  const markerDistance = Math.abs(x - marker);
  if (markerDistance <= 6 && markerDistance <= Math.min(Math.abs(x - low), Math.abs(x - high))) return 'tune';
  if (width >= (touch ? 80 : 32)) {
    // Leave a tuning target between the handles even on a narrow filter.
    // Choosing the nearest edge also avoids the low edge winning overlaps.
    const radius = Math.min(touch ? 18 : 8, width / 4);
    const a = Math.abs(x - low), b = Math.abs(x - high);
    if (Math.min(a, b) <= radius) return a <= b ? 'edge-low' : 'edge-high';
  }
  if (ruler || (x >= low && x <= high) || Math.abs(x - marker) <= (touch ? 28 : 10)) return 'tune';
  return 'pan';
}
