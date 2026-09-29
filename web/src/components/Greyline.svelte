<script lang="ts">
  /**
   * The world by day and night, with this station on it. The greyline, the
   * band of twilight between the two, is where low-band paths open, which is
   * why operators keep a map like this beside a receiver. Drawn here from the
   * Sun's position and a coarse outline of the land, once a minute; nothing
   * is fetched but the outline, which comes with the widget.
   */
  import { site } from '../state/store';
  import { locatorCentre } from '../util/locator';
  import { elevation, subsolarPoint, sunriseSunset } from '../util/sun';

  let canvas = $state<HTMLCanvasElement | null>(null);
  let width = $state(0);
  let now = $state(Date.now());
  let rings = $state.raw<number[][][] | null>(null);

  $effect(() => {
    let cancelled = false;
    import('../data/land').then((module) => {
      if (!cancelled) rings = module.landRings();
    });
    const timer = window.setInterval(() => (now = Date.now()), 60_000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  });

  const station = $derived(locatorCentre(site.value?.grid));
  const day = $derived(station ? sunriseSunset(station.lat, station.lon, now) : null);

  function token(name: string, fallback: string): string {
    const value = canvas ? getComputedStyle(canvas).getPropertyValue(name).trim() : '';
    return value || fallback;
  }

  $effect(() => {
    const element = canvas;
    const land = rings;
    const at = now;
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
    const scale = w / 720; // the outline is in half degrees, 720 across
    for (const ring of land) {
      context.beginPath();
      ring.forEach(([x, y], index) => (index ? context.lineTo(x * scale, y * scale) : context.moveTo(x * scale, y * scale)));
      context.closePath();
      context.fill();
    }

    // Night and twilight, on a grid of a few thousand cells stretched over
    // the map: the terminator is soft anyway, and a cell per pixel would be
    // two hundred thousand trigonometric evaluations a minute for nothing.
    const sun = subsolarPoint(at);
    const cells = document.createElement('canvas');
    cells.width = 144;
    cells.height = 72;
    const shade = cells.getContext('2d');
    if (!shade) return;
    const image = shade.createImageData(cells.width, cells.height);
    for (let y = 0; y < cells.height; y++) {
      const lat = 90 - (y + 0.5) * (180 / cells.height);
      for (let x = 0; x < cells.width; x++) {
        const lon = -180 + (x + 0.5) * (360 / cells.width);
        const e = elevation(lat, lon, sun);
        const i = (y * cells.width + x) * 4;
        // Full night below -12 degrees, day above 0, twilight between.
        const night = e >= 0 ? 0 : e <= -12 ? 1 : -e / 12;
        image.data[i + 3] = Math.round(night * 150);
      }
    }
    shade.putImageData(image, 0, 0);
    context.imageSmoothingEnabled = true;
    context.drawImage(cells, 0, 0, w, h);

    // Where the Sun stands overhead, and the station.
    const toX = (lon: number) => ((lon + 180) / 360) * w;
    const toY = (lat: number) => ((90 - lat) / 180) * h;
    context.fillStyle = token('--warning', '#e8b339');
    context.beginPath();
    context.arc(toX(sun.lon), toY(sun.lat), 4 * ratio, 0, 2 * Math.PI);
    context.fill();
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

  function clock(ms: number | null | undefined): string {
    if (ms === null || ms === undefined) return '–';
    const date = new Date(ms);
    return `${String(date.getUTCHours()).padStart(2, '0')}:${String(date.getUTCMinutes()).padStart(2, '0')}`;
  }
</script>

<div class="greyline" bind:clientWidth={width}>
  <div role="img" aria-label="World map with the night side shaded{station ? ' and this station marked' : ''}">
    <canvas bind:this={canvas} class="greyline__map" aria-hidden="true"></canvas>
  </div>
  <p class="greyline__caption">
    {#if station && day}
      Here: sunrise {clock(day.rise)}, sunset {clock(day.set)} UTC.
    {:else}
      The station has no locator, so it is not on the map.
    {/if}
    <span class="greyline__key"><span class="greyline__dot is-sun"></span>Sun overhead <span class="greyline__dot is-station"></span>Station</span>
  </p>
</div>
