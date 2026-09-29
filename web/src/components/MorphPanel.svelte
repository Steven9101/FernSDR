<script lang="ts">
  /**
   * A panel swap that morphs instead of cutting.
   *
   * The control panels are very different heights - Receive is a rack of eight
   * controls, Connection is a short statistics table - so switching tabs used to
   * resize the rail, or the sheet, in a single frame. On the phone that is the
   * whole lower half of the screen jumping, which reads as a reload rather than
   * a change of view.
   *
   * The anatomy is Rare UI's family-drawer: the container animates to the
   * measured height of the incoming content while the two views cross-fade, and
   * the fade is timed by how far the height actually travelled. A short hop gets
   * a quick fade; a tall one gets a longer fade so the movement and the fade
   * finish together. A fixed duration makes the small changes feel sluggish and
   * the large ones feel rushed, which is why the timing is derived rather than
   * chosen.
   *
   * Ported rather than installed: the original is React on `motion` and
   * `react-use-measure`, both of which this client will not take on. The
   * constants below - the 0.15-0.27s clamp, the divisor, the curve and the 0.96
   * entry scale - are the source's, so the feel is the same.
   */
  import type { Snippet } from 'svelte';

  /**
   * Rare UI's timing: the source computes `heightDifference / 500` in seconds,
   * which is 2ms of fade per pixel travelled, clamped to 150-270ms. So a swap
   * of 75px or less takes the floor, one of 135px or more takes the ceiling,
   * and the panels here - 364 to 451px - land in between.
   */
  const MIN_MS = 150;
  const MAX_MS = 270;
  const MS_PER_PX = 2;

  interface Props {
    /** Changing this is what triggers the morph. */
    viewKey: string;
    children: Snippet;
  }

  let { viewKey, children }: Props = $props();

  let box: HTMLDivElement;
  let lastKey: string | null = null;
  let lastHeight: number | null = null;
  // Set each time the view changes, so the incoming content can be given its
  // entry animation without animating on every unrelated update.
  let entering = $state(false);

  // The view on screen. {#key} below replaces it whenever viewKey changes, and
  // effects run after that, so it is always the current one here.
  const view = () => box.firstElementChild as HTMLElement | null;

  $effect(() => {
    const key = viewKey;
    const inner = view();
    if (!inner) return;

    const next = inner.offsetHeight;
    const changed = lastKey !== null && key !== lastKey;
    lastKey = key;

    if (!changed || lastHeight === null) {
      // First paint: adopt the height without animating.
      lastHeight = next;
      box.style.height = `${next}px`;
      return;
    }

    const previous = lastHeight;
    lastHeight = next;
    const ms = Math.min(Math.max(Math.abs(next - previous) * MS_PER_PX, MIN_MS), MAX_MS);
    box.style.setProperty('--morph-duration', `${ms}ms`);

    // Start from where the old panel ended, so the transition has something to
    // travel from: setting the target height alone would be a jump.
    box.style.height = `${previous}px`;
    void box.offsetHeight; // flush, so the two heights are separate styles
    box.style.height = `${next}px`;
    entering = true;
  });

  $effect(() => {
    if (!entering) return;
    const id = setTimeout(() => (entering = false), MAX_MS);
    return () => clearTimeout(id);
  });

  // The panel's own content can change height while it is open - a station
  // list loading, a notice appearing - and the container is holding an
  // explicit pixel height, so it has to follow. That is not a view change and
  // does not animate.
  $effect(() => {
    void viewKey;
    const inner = view();
    if (!inner) return;
    const observer = new ResizeObserver(() => {
      if (view() !== inner || !inner.isConnected) return;
      const next = inner.offsetHeight;
      if (next === lastHeight) return;
      lastHeight = next;
      box.style.height = `${next}px`;
    });
    observer.observe(inner);
    return () => observer.disconnect();
  });
</script>

<div bind:this={box} class="morph">
  {#key viewKey}
    <div class="morph__view{entering ? ' is-entering' : ''}">
      {@render children()}
    </div>
  {/key}
</div>
