/**
 * Where a small floating box goes beside an anchor, in viewport pixels for a
 * `position: fixed` box: on the side asked for, or on the opposite side when
 * that one has no room and the other has more, and kept `padding` inside the
 * viewport along the other axis. This is all the page's tooltips and tool
 * panels need, and a positioning library for it was 16 KB of every visit.
 */
export type Side = 'top' | 'bottom' | 'left' | 'right';

export interface Placement {
  x: number;
  y: number;
  side: Side;
}

const OPPOSITE: Record<Side, Side> = { top: 'bottom', bottom: 'top', left: 'right', right: 'left' };

export function place(
  anchor: { left: number; top: number; width: number; height: number },
  box: { width: number; height: number },
  side: Side,
  viewport: { width: number; height: number },
  options: { gap?: number; padding?: number; align?: 'center' | 'start' | 'end' } = {},
): Placement {
  const gap = options.gap ?? 6;
  const padding = options.padding ?? 8;
  const room = (s: Side) =>
    s === 'top' ? anchor.top - gap - padding
      : s === 'bottom' ? viewport.height - (anchor.top + anchor.height) - gap - padding
        : s === 'left' ? anchor.left - gap - padding
          : viewport.width - (anchor.left + anchor.width) - gap - padding;
  const needed = side === 'top' || side === 'bottom' ? box.height : box.width;
  const chosen = room(side) < needed && room(OPPOSITE[side]) > room(side) ? OPPOSITE[side] : side;

  let x: number;
  let y: number;
  if (chosen === 'top' || chosen === 'bottom') {
    x = options.align === 'start' ? anchor.left
      : options.align === 'end' ? anchor.left + anchor.width - box.width
        : anchor.left + (anchor.width - box.width) / 2;
    y = chosen === 'top' ? anchor.top - gap - box.height : anchor.top + anchor.height + gap;
  } else {
    y = options.align === 'start' ? anchor.top
      : options.align === 'end' ? anchor.top + anchor.height - box.height
        : anchor.top + (anchor.height - box.height) / 2;
    x = chosen === 'left' ? anchor.left - gap - box.width : anchor.left + anchor.width + gap;
  }
  // Kept inside the viewport along the side it sits on, as far as it fits.
  const clamp = (value: number, size: number, limit: number) =>
    Math.max(padding, Math.min(value, limit - padding - size));
  if (chosen === 'top' || chosen === 'bottom') x = clamp(x, box.width, viewport.width);
  else y = clamp(y, box.height, viewport.height);
  return { x: Math.round(x), y: Math.round(y), side: chosen };
}

/**
 * Keeps `update` running while a floating box is shown, once a frame: the
 * anchor may be in a sheet being dragged or a list being scrolled, and a
 * frame's arithmetic for one small box costs nothing. Returns the stop.
 */
export function follow(update: () => void): () => void {
  let frame = 0;
  const tick = () => {
    update();
    frame = requestAnimationFrame(tick);
  };
  tick();
  return () => cancelAnimationFrame(frame);
}
