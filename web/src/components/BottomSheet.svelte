<script lang="ts">
  /**
   * The mobile control surface.
   *
   * A draggable sheet rather than a modal, because on a phone the waterfall is
   * the thing you are looking at and controls should be reachable without losing
   * sight of it. Three snap heights, dragged with the handle or cycled by
   * tapping it; the sheet never covers the frequency readout.
   */
  import type { Snippet } from 'svelte';
  import ControlTabs from './ControlTabs.svelte';

  type SheetSnap = 'peek' | 'half' | 'full';

  /*
   * Heights as a fraction of the viewport. `peek` is just the grip and the tab
   * row: enough that the controls are visible and one tap away, while the
   * waterfall - the thing the user is actually looking at - keeps the screen.
   * `full` deliberately stops well short of covering everything, because a
   * sheet that hides the display it controls is a modal wearing a disguise.
   */
  const SNAP_FRACTIONS: Record<SheetSnap, number> = {
    peek: 0.1,
    half: 0.42,
    full: 0.68,
  };

  interface Props {
    tabs: { id: string; label: string }[];
    activeTab: string;
    onTabChange: (id: string) => void;
    children: Snippet;
  }

  let { tabs, activeTab, onTabChange, children }: Props = $props();

  /**
   * Resistance past the ends of the range.
   *
   * A hard clamp is an invisible wall: the finger keeps moving and the sheet
   * stops dead, which reads as a bug. Real things slow down before they stop, so
   * over-drag is allowed at a third of the rate and the sheet visibly resists.
   */
  function damp(fraction: number): number {
    const LOW = 0.06;
    const HIGH = 0.9;
    if (fraction < LOW) return LOW - (LOW - fraction) / 3;
    if (fraction > HIGH) return HIGH + (fraction - HIGH) / 3;
    return fraction;
  }

  let snap = $state<SheetSnap>('peek');
  let dragOffset = $state<number | null>(null);
  let drag: { startY: number; startFraction: number; lastY: number; lastAt: number; velocity: number } | null = null;

  const fraction = $derived(dragOffset ?? SNAP_FRACTIONS[snap]);

  function onPointerDown(event: PointerEvent & { currentTarget: HTMLElement }) {
    // A second finger landing mid-drag would otherwise jump the sheet to
    // wherever it touched down.
    if (drag) return;
    event.currentTarget.setPointerCapture(event.pointerId);
    drag = {
      startY: event.clientY,
      startFraction: SNAP_FRACTIONS[snap],
      lastY: event.clientY,
      lastAt: event.timeStamp,
      velocity: 0,
    };
  }

  function onPointerMove(event: PointerEvent) {
    if (!drag) return;

    const elapsed = event.timeStamp - drag.lastAt;
    if (elapsed > 0) {
      // Screen pixels per millisecond, smoothed a little so one stuttering
      // frame cannot decide whether a flick counts.
      const instant = (drag.lastY - event.clientY) / elapsed;
      drag.velocity = drag.velocity * 0.6 + instant * 0.4;
      drag.lastY = event.clientY;
      drag.lastAt = event.timeStamp;
    }

    const wanted = drag.startFraction + (drag.startY - event.clientY) / window.innerHeight;
    dragOffset = damp(wanted);
  }

  function onPointerUp() {
    const ended = drag;
    drag = null;
    if (dragOffset === null) {
      // A tap on the handle cycles through the heights, which is quicker than
      // dragging when you just want the controls out of the way.
      if (ended) snap = snap === 'peek' ? 'half' : snap === 'half' ? 'full' : 'peek';
      return;
    }
    const offset = dragOffset;
    // A flick should be enough on its own. Requiring the sheet to be dragged
    // past a midpoint makes it feel heavy: throw it and it should go, which is
    // what every native sheet does. 0.11 px/ms is the threshold that separates
    // a deliberate flick from a slow drag that happened to end while moving.
    const order: SheetSnap[] = ['peek', 'half', 'full'];
    const flick = ended ? ended.velocity : 0;
    if (Math.abs(flick) > 0.11) {
      const nearestIndex = order.reduce(
        (best, key, index) =>
          Math.abs(SNAP_FRACTIONS[key] - offset) <
          Math.abs(SNAP_FRACTIONS[order[best]] - offset)
            ? index
            : best,
        0,
      );
      const step = flick > 0 ? 1 : -1;
      snap = order[Math.max(0, Math.min(order.length - 1, nearestIndex + step))];
      dragOffset = null;
      return;
    }

    // Otherwise, snap to whichever height the sheet ended up nearest.
    let nearest: SheetSnap = 'peek';
    let best = Infinity;
    for (const [key, value] of Object.entries(SNAP_FRACTIONS) as [SheetSnap, number][]) {
      const distance = Math.abs(value - offset);
      if (distance < best) {
        best = distance;
        nearest = key;
      }
    }
    snap = nearest;
    dragOffset = null;
  }

  function onKeyDown(event: KeyboardEvent) {
    if (event.key === 'ArrowUp') { event.preventDefault(); snap = snap === 'peek' ? 'half' : 'full'; }
    if (event.key === 'ArrowDown') { event.preventDefault(); snap = snap === 'full' ? 'half' : 'peek'; }
    if (event.key === ' ' || event.key === 'Enter') {
      event.preventDefault();
      snap = snap === 'peek' ? 'half' : snap === 'half' ? 'full' : 'peek';
    }
  }
</script>

<!-- The stage keeps a minimum height and the top bar and status row take their
     own space, so a raw 68vh sheet does not fit: the grid overflowed by
     95px, the sheet's own scroll container ended up partly below the
     viewport believing it had nothing to scroll, and its last control was
     unreachable. Clamp to what is genuinely left, a banner included. -->
<div
  class="sheet"
  style:height="min({fraction * 100}dvh, calc(100dvh - var(--stage-min) - 5rem - var(--banner-height, 0px)))"
  data-snap={snap}
  data-dragging={dragOffset !== null ? 'true' : 'false'}
>
  <div
    class="sheet__grip"
    role="button"
    tabindex="0"
    aria-label="Controls, currently {snap}. Drag or press to resize."
    style:touch-action="none"
    onpointerdown={onPointerDown}
    onpointermove={onPointerMove}
    onpointerup={onPointerUp}
    onpointercancel={onPointerUp}
    onkeydown={onKeyDown}
  >
    <span class="sheet__grip-bar"></span>
  </div>

  <ControlTabs {tabs} active={activeTab} variant="sheet" onChange={(id) => {
    onTabChange(id);
    if (snap === 'peek') snap = 'half';
  }} />

  <div class="sheet__body">{@render children()}</div>
</div>
