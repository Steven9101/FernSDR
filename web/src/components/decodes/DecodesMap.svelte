<script lang="ts">
  /**
   * Where the stations in the list are, from the locators they sent. FT8
   * carries a four-character locator, a square of two by one degrees, so a
   * dot is a region rather than a place. Each callsign appears once, where
   * its latest locator put it; stations calling CQ are drawn brighter, since
   * those are the ones a listener could answer from a transmitter.
   */
  import type { Decode } from '../../state/decodes';
  import { isCq } from '../../state/decodes';
  import { locatorCentre } from '../../util/locator';

  interface Props {
    decodes: Decode[];
    station: { lat: number; lon: number } | null;
  }

  let { decodes, station }: Props = $props();

  let canvas = $state<HTMLCanvasElement | null>(null);
  let width = $state(0);
  let rings = $state.raw<number[][][] | null>(null);

  $effect(() => {
    let cancelled = false;
    import('../../data/land').then((module) => {
      if (!cancelled) rings = module.landRings();
    });
    return () => {
      cancelled = true;
    };
  });

  // One point per callsign, the latest locator winning; a decode without a
  // callsign of its own (a report, a 73) places nobody.
  const located = $derived.by(() => {
    const byCall = new Map<string, { lat: number; lon: number; cq: boolean }>();
    for (const decode of decodes) {
      const place = locatorCentre(decode.grid);
      if (!decode.call || !place) continue;
      byCall.set(decode.call, { ...place, cq: isCq(decode.message) || (byCall.get(decode.call)?.cq ?? false) });
    }
    return [...byCall.values()];
  });

  function token(name: string, fallback: string): string {
    const value = canvas ? getComputedStyle(canvas).getPropertyValue(name).trim() : '';
    return value || fallback;
  }

  $effect(() => {
    const element = canvas;
    const land = rings;
    const points = located;
    const here = station;
    if (!element || !land || width <= 0) return;
    const ratio = window.devicePixelRatio || 1;
    const w = Math.round(width * ratio);
    const h = Math.round(w / 2);
    element.width = w;
    element.height = h;
    const context = element.getContext('2d');
    if (!context) return;

    context.fillStyle = token('--well', '#222');
    context.fillRect(0, 0, w, h);
    context.fillStyle = token('--track', '#555');
    const scale = w / 720;
    for (const ring of land) {
      context.beginPath();
      ring.forEach(([x, y], index) => (index ? context.lineTo(x * scale, y * scale) : context.moveTo(x * scale, y * scale)));
      context.closePath();
      context.fill();
    }

    const toX = (lon: number) => ((lon + 180) / 360) * w;
    const toY = (lat: number) => ((90 - lat) / 180) * h;
    const radius = Math.max(2, 2.5 * ratio);
    const heard = token('--muted-foreground', '#999');
    const calling = token('--primary', '#4c8dff');
    // Calling stations last, so they sit on top where squares overlap.
    for (const cq of [false, true]) {
      context.fillStyle = cq ? calling : heard;
      for (const point of points) {
        if (point.cq !== cq) continue;
        context.beginPath();
        context.arc(toX(point.lon), toY(point.lat), radius, 0, 2 * Math.PI);
        context.fill();
      }
    }
    if (here) {
      context.fillStyle = token('--signal', '#ff8a4c');
      context.strokeStyle = token('--background', '#000');
      context.lineWidth = 2 * ratio;
      context.beginPath();
      context.arc(toX(here.lon), toY(here.lat), 4.5 * ratio, 0, 2 * Math.PI);
      context.fill();
      context.stroke();
    }
  });
</script>

<div class="greyline decodes__map" bind:clientWidth={width}>
  <div role="img" aria-label="World map with {located.length} located stations{station ? ' and this receiver' : ''}">
    <canvas bind:this={canvas} class="greyline__map" aria-hidden="true"></canvas>
  </div>
  <p class="greyline__caption">
    {located.length.toLocaleString()} {located.length === 1 ? 'station' : 'stations'} placed by locator.
    <span class="greyline__key">
      <span class="greyline__dot decodes__dot is-cq"></span>Calling CQ
      <span class="greyline__dot decodes__dot"></span>Heard
      {#if station}<span class="greyline__dot is-station"></span>Receiver{/if}
    </span>
  </p>
</div>
