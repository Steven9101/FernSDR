<script lang="ts">
  /**
   * The waterfall archive: what this band looked like earlier.
   *
   * A receiver answers "what is on the air"; an archive answers "what was on
   * the air last night", which is the question somebody asks after they missed
   * something. The two are drawn the same way on purpose, so nothing has to be
   * learned twice: frequency across, time down, brightness for signal strength.
   *
   * Only offered when the operator has turned it on. When they have kept it to
   * themselves the server answers 403 and this says so rather than showing an
   * empty box, because "there is nothing here" and "you may not look" are
   * different answers and guessing between them wastes the reader's time.
   */
  import { untrack } from 'svelte';
  import { currentBand, display } from '../state/store';
  import { paintArchive } from '../render/history-paint';
  import HistoryTile from './history/HistoryTile.svelte';
  import { decodeHistory, historyReach, historyWindow, timeTicks, type Archive } from '../util/history';

  /**
   * Spans offered, in hours. A quarter hour is there because it is the one
   * anybody uses first - "what was that, just now" - and because it is the only
   * span a young archive can fill. Anything past a day is a different tool.
   */
  const SPANS = [0.25, 1, 6, 12, 24];

  function spanLabel(hours: number): string {
    return hours < 1 ? `${Math.round(hours * 60)} min` : `${hours} h`;
  }

  /**
   * The clock the side is labelled in. UTC by default: it is what a log, a
   * schedule and another listener's report use. Remembered in this browser.
   */
  const ZONE_KEY = 'fernsdr.history.zone';
  function storedZone(): 'utc' | 'local' {
    try {
      return localStorage.getItem(ZONE_KEY) === 'local' ? 'local' : 'utc';
    } catch {
      return 'utc';
    }
  }

  /**
   * Enlarged, from six hours up, the picture is as tall as the day needs
   * rather than as tall as the window: two pixels a minute, scrolled through
   * like the full-day pages some receivers publish.
   */
  const TALL_FROM_HOURS = 6;
  const PX_PER_HOUR = 120;

  interface Props {
    /** Filling the window: as many rows as there is height, labels between. */
    expanded?: boolean;
    onExpand?: () => void;
    onClose?: () => void;
  }

  let { expanded = false, onExpand, onClose }: Props = $props();

  const band = $derived(currentBand.value);
  // The band's identity and access rather than the band itself: its status
  // updates every few seconds, and none of that is a reason to fetch again.
  const bandId = $derived(band?.id);
  const access = $derived(band?.history ?? 'off');
  const palette = $derived(display.value.palette);
  const floorDb = $derived(display.value.floorDb);
  const ceilingDb = $derived(display.value.ceilingDb);

  let canvas = $state<HTMLCanvasElement | null>(null);
  let canvasHeight = $state(0);
  let canvasWidth = $state(0);
  let hours = $state(0.25);
  let zone = $state<'utc' | 'local'>(storedZone());
  function setZone(next: 'utc' | 'local') {
    zone = next;
    try {
      localStorage.setItem(ZONE_KEY, next);
    } catch {
      // Chosen for this page only.
    }
  }
  const tall = $derived(expanded && hours >= TALL_FROM_HOURS);
  // How far back the shown window ends, in hours before now. Zero is live.
  let back = $state(0);
  let archive = $state.raw<Archive | null>(null);
  let problem = $state('');
  let loading = $state(false);
  let windowRange = $state.raw({ from: 0, to: 1 });
  let refresh = $state(0);
  // How far back the archive reaches grows with the clock, not only when a
  // new download arrives.
  let now = $state(Date.now());
  const reach = $derived(historyReach(archive?.oldest ?? band?.history_from ?? 0, now, hours));
  const shown = $derived(historyWindow(windowRange.from, windowRange.to, archive?.oldest ?? 0));

  $effect(() => {
    void bandId;
    untrack(() => {
      back = 0;
      archive = null;
    });
  });

  $effect(() => {
    const limit = reach;
    untrack(() => {
      back = Math.min(back, limit);
    });
  });

  $effect(() => {
    if (access === 'off') return;
    const timer = window.setInterval(() => (now = Date.now()), 1000);
    return () => window.clearInterval(timer);
  });

  $effect(() => {
    if (back !== 0 || access === 'off') return;
    // History is a disk overview, separate from the live waterfall. Refresh
    // only while visible, with one bounded request per minute.
    const timer = window.setInterval(() => {
      if (!document.hidden) refresh += 1;
    }, 60_000);
    return () => window.clearInterval(timer);
  });

  $effect(() => {
    const id = bandId;
    const span = hours;
    const ago = back;
    void refresh;
    if (id === undefined || access === 'off') return;
    let cancelled = false;
    const abort = new AbortController();
    untrack(() => {
      loading = true;
      problem = '';
    });
    const to = Math.max(0, Math.round(Date.now() - ago * 3600 * 1000));
    const from = Math.max(0, Math.round(to - span * 3600 * 1000));
    // Tall, the tiles fetch the picture; this only needs to know what there is.
    const overview = untrack(() => tall) ? '&rows=64' : '';
    fetch(`/api/history?band=${encodeURIComponent(id)}&from=${from}&to=${to}${overview}`, {
      credentials: 'same-origin',
      signal: abort.signal,
    })
      .then(async (response) => {
        if (response.status === 403) throw new Error('This receiver keeps its history to itself.');
        if (!response.ok) throw new Error('The history could not be read.');
        return response.arrayBuffer();
      })
      .then((buffer) => {
        if (!cancelled) {
          archive = decodeHistory(buffer);
          windowRange = { from, to };
        }
      })
      .catch((failure) => {
        if (!cancelled) {
          archive = null;
          problem = (failure as Error).message;
        }
      })
      .finally(() => {
        if (!cancelled) loading = false;
      });
    return () => {
      cancelled = true;
      abort.abort();
    };
  });

  $effect(() => {
    const element = canvas;
    const current = archive;
    const range = shown;
    void windowRange;
    void loading;
    const paletteId = palette;
    const floor = floorDb;
    const ceiling = ceilingDb;
    if (!element || !current || current.times.length === 0) return;

    // In the panel the picture is small and 512 rows are plenty. Full size,
    // one row of the picture per device pixel of height, so that the time
    // resolution the archive has is what the screen shows.
    const height = expanded
      ? Math.max(256, Math.min(4096, Math.round(canvasHeight * (window.devicePixelRatio || 1))))
      : 512;
    paintArchive(element, current, range.from, range.to, height, paletteId, floor, ceiling);
  });

  // The scrub only offers what the archive actually holds, so it cannot be
  // dragged into a stretch that was never recorded.
  const first = $derived(shown.from);
  const last = $derived(shown.to);
  // One frequency label per 110 px or so, and as many times down the side.
  const labels = $derived(expanded ? Math.max(3, Math.min(9, Math.floor(canvasWidth / 110) + 1)) : 3);
  const between = (from: number, to: number, count: number) =>
    Array.from({ length: count }, (_, i) => from + ((to - from) * i) / (count - 1));
  const ticks = $derived(timeTicks(first, last, tall ? hours * PX_PER_HOUR : canvasHeight, zone));

  // Where the tall picture is scrolled to, so only the pieces near the view
  // are fetched.
  let plot = $state<HTMLDivElement | null>(null);
  let scrollTop = $state(0);
  let viewHeight = $state(0);
  function nearView(index: number): boolean {
    const top = tiles.slice(0, index).reduce((sum, tile) => sum + ((tile.to - tile.from) / 3_600_000) * PX_PER_HOUR, 0);
    const bottom = top + ((tiles[index].to - tiles[index].from) / 3_600_000) * PX_PER_HOUR;
    return viewHeight > 0 && bottom > scrollTop - 200 && top < scrollTop + viewHeight + 200;
  }

  /** The full day in three-hour pieces, newest at the top. */
  const TILE_MS = 3 * 3600 * 1000;
  // On fixed three-hour boundaries of the clock, so a refresh a minute later
  // changes only the newest piece, which is still being recorded; the rest
  // keep what they fetched.
  const tiles = $derived.by(() => {
    if (!tall) return [];
    const pieces: { from: number; to: number; key: string }[] = [];
    let start = Math.floor(last / TILE_MS) * TILE_MS;
    let end = last;
    while (end > first && pieces.length < 64) {
      const from = Math.max(first, start);
      pieces.push({ from, to: end, key: end === last ? `${from}:${end}` : String(from) });
      end = start;
      start -= TILE_MS;
    }
    return pieces;
  });
  const zoneName = $derived(zone === 'utc' ? 'UTC' : 'Local time');
</script>

{#if !band || access === 'off'}
  <p class="history__note">
    This receiver keeps no history. An operator turns it on per band.
  </p>
{:else}
  <div class="history{expanded ? ' history--expanded' : ''}" aria-busy={loading}>
    <div class="history__controls" role="group" aria-label="How far back to show">
      {#each SPANS as span (span)}
        <button
          type="button"
          class="history__span{span === hours ? ' is-active' : ''}"
          aria-pressed={span === hours}
          onclick={() => { hours = span; back = 0; }}
        >
          {spanLabel(span)}
        </button>
      {/each}
    </div>

    <div class="history__navigation">
      <span class="history__note">{loading ? 'Updating…' : back === 0 ? 'Latest recordings' : 'Earlier recordings'}</span>
      {#if back > 0}<button type="button" class="history__action" onclick={() => (back = 0)}>Latest</button>{/if}
      <button type="button" class="history__action" title="The clock the times are in; click for {zone === 'utc' ? 'local time' : 'UTC'}"
        aria-label="Times in {zoneName}; switch to {zone === 'utc' ? 'local time' : 'UTC'}"
        onclick={() => setZone(zone === 'utc' ? 'local' : 'utc')}>{zone === 'utc' ? 'UTC' : 'Local'}</button>
      <button type="button" class="history__action" disabled={loading} onclick={() => (refresh += 1)}>Refresh</button>
      {#if onExpand}<button type="button" class="history__action" onclick={onExpand}>Enlarge</button>{/if}
      {#if onClose}<button type="button" class="history__action" onclick={onClose}>Close</button>{/if}
    </div>

    {#if reach > 0}
      <label class="history__scrub">
        <input
          class="slider"
          type="range"
          min="0"
          max={reach}
          step="any"
          value={reach - back}
          aria-label="How far back to look"
          oninput={(event) =>
            (back = Math.max(0, Math.min(reach, reach - Number(event.currentTarget.value))))}
        />
        <span class="history__when">
          {back === 0 ? 'now' : `${back < 1 ? Math.round(back * 60) + ' min' : back.toFixed(1) + ' h'} ago`}
        </span>
      </label>
    {/if}

    {#if problem}
      <p class="history__note" role="alert">{problem}</p>
    {:else if archive && archive.times.length > 0}
      {#if first > windowRange.from}<p class="history__note">Available since {new Date(first).toLocaleString()}.</p>{/if}
      <div class="history__plot{tall ? ' history__plot--tall' : ''}" style:--plot-height={tall ? `${hours * PX_PER_HOUR}px` : undefined}
        bind:this={plot} bind:clientHeight={viewHeight} onscroll={() => (scrollTop = plot?.scrollTop ?? 0)}>
        <div class="history__frequency" aria-label="Frequency in MHz">
          {#each between(archive.lowHz, archive.highHz, labels) as hz (hz)}
            <span>{Math.abs(hz) < 500 ? '0.000' : (hz / 1e6).toFixed(3)}</span>
          {/each}
        </div>
        <div class="history__times" aria-label="{zoneName}, newest at the top">
          {#each ticks as tick (tick.time)}
            <span class="history__tick{tick.day ? ' is-day' : ''}" style:top="{tick.position * 100}%">{tick.label}</span>
          {/each}
        </div>
        <!-- A picture of the recording, described by its label; nothing on it
             takes input. -->
        <!-- svelte-ignore a11y_no_interactive_element_to_noninteractive_role -->
        {#if tall && bandId !== undefined}
          <div class="history__tiles" role="img" bind:clientWidth={canvasWidth}
            aria-label="Recorded spectrum from {new Date(first).toLocaleString()} to {new Date(last).toLocaleString()}, newest at the top">
            {#each tiles as tile, index (tile.key)}
              <HistoryTile load={nearView(index)} band={bandId} from={tile.from} to={tile.to} heightPx={((tile.to - tile.from) / 3_600_000) * PX_PER_HOUR}
                {palette} floor={floorDb} ceiling={ceilingDb} />
            {/each}
          </div>
        {:else}
          <canvas bind:this={canvas} bind:clientHeight={canvasHeight} bind:clientWidth={canvasWidth} class="history__canvas" role="img"
            aria-label="Recorded spectrum from {new Date(first).toLocaleString()} to {new Date(last).toLocaleString()}, newest at the top"></canvas>
        {/if}
      </div>
      <p class="history__note">{tall
        ? `About one row per ${Math.round(3600 / (PX_PER_HOUR * (window.devicePixelRatio || 1)))} s`
        : `One row per ${Math.round(archive.rowMs / 1000)} s`}. {zoneName}, newest at the top. Empty stretches have no recording. This archive stores the waterfall, not audio.</p>
    {:else if loading}
      <p class="history__note" role="status">Reading recordings…</p>
    {:else}
      <p class="history__note">
        Nothing recorded for this stretch yet. The archive fills as the receiver runs.
      </p>
    {/if}
  </div>
{/if}
