<script lang="ts">
  /**
   * Where this receiver is and what it listens with: the part of the world
   * around it with the station marked, and the station's own details. Drawn
   * from the land outline that comes with the day and night map; nothing is
   * fetched but that outline.
   */
  import { bands, site } from '../state/store';
  import { locatorCentre } from '../util/locator';
  import { receiverLabel } from '../util/receivers';

  let canvas = $state<HTMLCanvasElement | null>(null);
  let width = $state(0);
  let rings = $state.raw<number[][][] | null>(null);

  $effect(() => {
    let cancelled = false;
    import('../data/land').then((module) => {
      if (!cancelled) rings = module.landRings();
    });
    return () => {
      cancelled = true;
    };
  });

  const station = $derived(locatorCentre(site.value?.grid));
  const kinds = $derived([...new Set(bands.value.map((band) => receiverLabel(band.receiver)).filter(Boolean))]);
  // Sixty degrees of longitude and thirty of latitude around the station: a
  // country and its neighbours, enough to place it without a label.
  const SPAN_LON = 60;
  const SPAN_LAT = 30;

  function token(name: string, fallback: string): string {
    const value = canvas ? getComputedStyle(canvas).getPropertyValue(name).trim() : '';
    return value || fallback;
  }

  $effect(() => {
    const element = canvas;
    const land = rings;
    const here = station;
    if (!element || !land || !here || width <= 0) return;
    const ratio = window.devicePixelRatio || 1;
    const w = Math.round(width * ratio);
    const h = Math.round(w / 2);
    element.width = w;
    element.height = h;
    const context = element.getContext('2d');
    if (!context) return;
    const west = Math.max(-180, Math.min(180 - SPAN_LON, here.lon - SPAN_LON / 2));
    const north = Math.min(90, Math.max(-90 + SPAN_LAT, here.lat + SPAN_LAT / 2));
    // The outline is in half degrees from 180 W and 90 N, 720 by 360.
    const scale = w / (SPAN_LON * 2);
    const x0 = (west + 180) * 2;
    const y0 = (90 - north) * 2;
    context.fillStyle = token('--well', '#222');
    context.fillRect(0, 0, w, h);
    context.fillStyle = token('--track', '#555');
    for (const ring of land) {
      context.beginPath();
      ring.forEach(([x, y], index) => {
        const px = (x - x0) * scale;
        const py = (y - y0) * scale;
        if (index) context.lineTo(px, py);
        else context.moveTo(px, py);
      });
      context.closePath();
      context.fill();
    }
    const sx = (here.lon - west) * 2 * scale;
    const sy = (north - here.lat) * 2 * scale;
    context.fillStyle = token('--signal', '#ff8a4c');
    context.strokeStyle = token('--background', '#000');
    context.lineWidth = 2 * ratio;
    context.beginPath();
    context.arc(sx, sy, 5 * ratio, 0, 2 * Math.PI);
    context.fill();
    context.stroke();
  });

  const degrees = (value: number, positive: string, negative: string) =>
    `${Math.abs(value).toFixed(1)}° ${value >= 0 ? positive : negative}`;
</script>

<div class="station-card">
  {#if station}
    <div role="img" aria-label="Map around the station at {degrees(station.lat, 'N', 'S')}, {degrees(station.lon, 'E', 'W')}" bind:clientWidth={width}>
      <canvas bind:this={canvas} class="station-card__map" aria-hidden="true"></canvas>
    </div>
  {/if}
  <dl class="station-card__facts">
    {#if site.value?.operator}<div><dt>Operator</dt><dd>{site.value.operator}</dd></div>{/if}
    {#if site.value?.location}<div><dt>Location</dt><dd>{site.value.location}</dd></div>{/if}
    {#if site.value?.grid}
      <div><dt>Locator</dt><dd>{site.value.grid.toUpperCase()}{#if station}<span class="station-card__coords">&ensp;{degrees(station.lat, 'N', 'S')}, {degrees(station.lon, 'E', 'W')}</span>{/if}</dd></div>
    {/if}
    {#if site.value?.antenna}<div><dt>Antenna</dt><dd>{site.value.antenna}</dd></div>{/if}
    {#if kinds.length > 0}<div><dt>Receiver</dt><dd>{kinds.join(', ')}</dd></div>{/if}
  </dl>
</div>
