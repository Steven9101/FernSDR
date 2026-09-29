<script lang="ts">
  /**
   * Space weather beside the receiver: NOAA's scales and indices, the solar
   * wind, and what the nearest ionosonde says about each HF band where this
   * station is. The receiver fetches it every ten minutes and hands everyone
   * the same copy, so a listener's browser talks to nobody but the receiver.
   *
   * The band rows are an estimate from one measurement, a hop's MUF and the
   * critical frequency overhead, and the widget says so rather than dressing
   * a rule of thumb up as a forecast.
   */
  import { formatSigned } from '../util/frequency';

  interface Outlook {
    band: string;
    mhz: number;
    dx: 'open' | 'marginal' | 'closed' | 'unknown';
    nearby: boolean;
    note?: string;
  }

  interface Report {
    updated?: number;
    scales?: { r?: number; s?: number; g?: number };
    g_tomorrow?: number;
    kp?: number;
    kp_history?: { time: string; kp: number }[];
    a?: number;
    sfi?: number;
    ssn?: number;
    xray?: string;
    xray_peak?: string;
    wind_speed?: number;
    bz?: number;
    bt?: number;
    sun?: number;
    ionosonde?: { name: string; code: string; km: number; time: string; mufd: number; fof2: number };
    bands?: Outlook[];
  }

  let report = $state<Report | null>(null);
  let failed = $state(false);

  $effect(() => {
    let stopped = false;
    const load = () => {
      if (document.hidden) return;
      fetch('/api/space-weather', { credentials: 'same-origin' })
        .then((response) => (response.ok ? response.json() : Promise.reject(new Error(String(response.status)))))
        .then((value: Report) => {
          if (!stopped) {
            report = value;
            failed = false;
          }
        })
        .catch(() => {
          if (!stopped) failed = true;
        });
    };
    load();
    const timer = window.setInterval(load, 5 * 60_000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  });

  const ready = $derived(report !== null && report.updated !== undefined);

  // NOAA's scales run 0 to 5; 1 and 2 are worth a glance, 3 and up matter.
  const level = (value: number | undefined) => (value === undefined ? '' : value >= 3 ? 'is-severe' : value >= 1 ? 'is-raised' : '');

  function utc(value: string | number): string {
    const date = new Date(typeof value === 'number' ? value : value.endsWith('Z') ? value : `${value}Z`);
    return Number.isNaN(date.getTime())
      ? ''
      : `${String(date.getUTCHours()).padStart(2, '0')}:${String(date.getUTCMinutes()).padStart(2, '0')} UTC`;
  }

  // Kp as bars, storm levels from 5 up in the warning colour. Against 0 to 3,
  // or the day's highest when more: on the full 0 to 9 a quiet day's bars
  // were a few pixels tall and read as a dotted line.
  const bars = $derived.by(() => {
    const points = report?.kp_history ?? [];
    const top = Math.max(3, ...points.map((point) => point.kp));
    return points.map((point) => ({ ...point, height: Math.max(12, (point.kp / top) * 100) }));
  });
</script>

<div class="space">
  {#if !ready}
    <p class="space__source">
      {failed ? 'Space weather is not available from this receiver right now.' : 'The receiver is fetching space weather; it arrives within a minute.'}
    </p>
  {:else if report}
    <div class="space__head">
      <div class="space__scales" aria-label="NOAA space weather scales">
        {#each [['R', 'Radio blackouts', report.scales?.r], ['S', 'Solar radiation storms', report.scales?.s], ['G', 'Geomagnetic storms', report.scales?.g]] as [letter, name, value] (letter)}
          <span class="space__scale {level(value as number | undefined)}" title="{name} (NOAA scale, 0 to 5)">{letter}{value ?? '\u2013'}</span>
        {/each}
        {#if report.g_tomorrow}<span class="space__scale {level(report.g_tomorrow)}" title="Geomagnetic storm forecast for tomorrow">G{report.g_tomorrow} tomorrow</span>{/if}
      </div>
      <span class="space__when">{utc(report.updated ?? 0)}</span>
    </div>

    <dl class="space__stats">
      <div title="Solar flux index, 10.7 cm"><dt>SFI</dt><dd>{report.sfi ?? '\u2013'}</dd></div>
      <div title="Sunspot number"><dt>SSN</dt><dd>{report.ssn ?? '\u2013'}</dd></div>
      <div title="Planetary K index, last three hours; the bars are the last day">
        <dt>Kp</dt>
        <dd class={(report.kp ?? 0) >= 5 ? 'is-raised' : ''}>
          {report.kp?.toFixed(1) ?? '\u2013'}
          {#if bars.length > 0}
            <svg class="space__spark" viewBox="0 0 {bars.length * 4} 12" aria-hidden="true" preserveAspectRatio="none">
              {#each bars as bar, i (bar.time)}
                <rect x={i * 4} y={12 - (bar.height / 100) * 12} width="3" height={(bar.height / 100) * 12} class={bar.kp >= 5 ? 'is-storm' : ''} />
              {/each}
            </svg>
          {/if}
        </dd>
      </div>
      <div title="Planetary A index"><dt>A</dt><dd>{report.a ?? '\u2013'}</dd></div>
      <div title="GOES X-ray flux now; the largest of six hours on hover"><dt>X-ray</dt><dd title={report.xray_peak ? `Peak of six hours: ${report.xray_peak}` : undefined}>{report.xray ?? '\u2013'}</dd></div>
      <div title="Solar wind speed at L1, km/s"><dt>Wind</dt><dd>{report.wind_speed ?? '\u2013'}</dd></div>
      <div title="Interplanetary field north-south, nT; southward lets storms in"><dt>Bz</dt><dd class={report.bz !== undefined && report.bz <= -10 ? 'is-raised' : ''}>{report.bz === undefined ? '\u2013' : formatSigned(report.bz, 0)}</dd></div>
      <div title="Maximum usable frequency for 3000 km, from the nearest ionosonde"><dt>MUF</dt><dd>{report.ionosonde ? report.ionosonde.mufd.toFixed(1) : '\u2013'}</dd></div>
    </dl>

    {#if report.bands}
      <ul class="space__bands" aria-label="Bands from here">
        {#each report.bands as band (band.band)}
          <li
            class="space__band is-{band.dx}"
            title="{band.band}: {band.dx === 'unknown' ? 'no reading' : band.dx + ' for distance'}{band.nearby ? ', open nearby (NVIS)' : ''}{band.note ? ` (${band.note})` : ''}"
          >
            {band.band.replace(' m', '')}{#if band.nearby}<span class="space__nearby" aria-label="open nearby"></span>{/if}
          </li>
        {/each}
      </ul>
      <p class="space__legend">
        <span class="space__key is-open"></span>open
        <span class="space__key is-marginal"></span>marginal
        <span class="space__key is-closed"></span>closed
        <span class="space__nearby is-key"></span>nearby
      </p>
    {/if}

    <p class="space__source">
      NOAA SWPC{#if report.ionosonde}; ionosonde {report.ionosonde.name.split(',')[0]}, {report.ionosonde.km} km, {utc(report.ionosonde.time)}, via KC2G{/if}. An estimate for this station{report.sun !== undefined ? `, by ${report.sun > 0 ? 'day' : 'night'}` : ''}.
    </p>
  {/if}
</div>
