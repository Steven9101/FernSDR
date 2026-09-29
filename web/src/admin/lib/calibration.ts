/*
The S-meter calibration as measurements: the server stores "14.1M:-23.50, 28M:-29.00", and this is
the same thing as points, with the rule the server uses to draw a line between them.
*/
export interface Point {
  hz: number;
  offsetDb: number;
}

export function parseCalibration(text: string): Point[] {
  return text
    .split(',')
    .map((item) => item.split(':'))
    .filter((parts) => parts.length === 2)
    .map((parts) => {
      const raw = parts[0].trim();
      const scale = /m$/i.test(raw) ? 1e6 : /k$/i.test(raw) ? 1e3 : 1;
      return { hz: parseFloat(raw) * scale, offsetDb: parseFloat(parts[1]) };
    })
    .filter((point) => Number.isFinite(point.hz) && Number.isFinite(point.offsetDb))
    .sort((a, b) => a.hz - b.hz);
}

export function formatCalibration(points: Point[]): string {
  return points
    .filter((point) => Number.isFinite(point.hz) && Number.isFinite(point.offsetDb))
    .sort((a, b) => a.hz - b.hz)
    .map((point) => `${(point.hz / 1e6).toFixed(4).replace(/0+$/, '').replace(/\.$/, '')}M:${point.offsetDb}`)
    .join(', ');
}

/**
 * The correction at one frequency: interpolated between the points either
 * side, held flat beyond the outermost. The same rule the server applies, so
 * the drawing and the receiver agree.
 */
export function offsetAt(points: Point[], hz: number): number {
  if (points.length === 0) return 0;
  if (hz <= points[0].hz) return points[0].offsetDb;
  const last = points[points.length - 1];
  if (hz >= last.hz) return last.offsetDb;
  for (let i = 1; i < points.length; i++) {
    if (hz > points[i].hz) continue;
    const low = points[i - 1];
    const span = points[i].hz - low.hz;
    if (span <= 0) return points[i].offsetDb;
    return low.offsetDb + ((hz - low.hz) / span) * (points[i].offsetDb - low.offsetDb);
  }
  return last.offsetDb;
}
