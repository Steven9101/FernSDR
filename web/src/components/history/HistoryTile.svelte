<script lang="ts">
  /**
   * A few hours of the full-day picture, fetched once the view says it has
   * scrolled near (`load`) and not before: a day of archive at full width is
   * megabytes, and most people look at the last hour or two of it. Fetched
   * once: a later resize redraws what arrived.
   */
  import { paintArchive } from '../../render/history-paint';
  import type { PaletteId } from '../../render/palettes';
  import { decodeHistory } from '../../util/history';

  interface Props {
    band: string;
    from: number;
    to: number;
    heightPx: number;
    palette: PaletteId;
    floor: number;
    ceiling: number;
    load: boolean;
  }

  let { band, from, to, heightPx, palette, floor, ceiling, load }: Props = $props();

  let element = $state<HTMLCanvasElement | null>(null);
  let width = $state(0);
  let requested = false;
  let archive = $state.raw<ReturnType<typeof decodeHistory> | null>(null);
  let failed = $state(false);

  $effect(() => {
    if (!load || width <= 0 || requested) return;
    requested = true;
    const ratio = window.devicePixelRatio || 1;
    const rows = Math.min(4096, Math.ceil(heightPx * ratio));
    const columns = Math.ceil(width * ratio);
    const abort = new AbortController();
    fetch(`/api/history?band=${encodeURIComponent(band)}&from=${Math.round(from)}&to=${Math.round(to)}&rows=${rows}&width=${columns}`,
      { credentials: 'same-origin', signal: abort.signal })
      .then((response) => (response.ok ? response.arrayBuffer() : Promise.reject(new Error(String(response.status)))))
      .then((buffer) => (archive = decodeHistory(buffer)))
      .catch((problem) => {
        if ((problem as Error).name === 'AbortError') requested = false;
        else failed = true;
      });
    return () => abort.abort();
  });

  $effect(() => {
    const canvas = element;
    const current = archive;
    if (!canvas || !current) return;
    const ratio = window.devicePixelRatio || 1;
    paintArchive(canvas, current, from, to, Math.max(1, Math.round(heightPx * ratio)), palette, floor, ceiling);
  });
</script>

<canvas bind:this={element} bind:clientWidth={width} class="history__tile{failed ? ' is-failed' : ''}"
  style:height="{heightPx}px" aria-hidden="true"></canvas>
