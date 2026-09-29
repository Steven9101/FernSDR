<script lang="ts" module>
  /**
   * Signal meter.
   *
   * Shown in dBFS, which is what the receiver actually measures, with S-units
   * derived from a calibration offset the operator sets. An S-meter that has
   * not been calibrated against a known source is a relative indication and is
   * labelled as one - quoting uncalibrated S-units as though they were absolute
   * is how signal reports become fiction.
   */
  import { box, computed, watch } from '../state/reactive.svelte';
  import { meter, currentBand, tuning } from '../state/store';
  import type { CalibrationPoint } from '../net/protocol';
  import { nextPeak } from './smeter-peak';

  /**
   * dBFS that reads as S1 when nobody has calibrated this receiver.
   *
   * A guess, and a deliberately unremarkable one: it puts a typical HF receiver
   * roughly in the right place so the bar moves usefully, and the readout is
   * labelled relative so nobody quotes it as a signal report.
   */
  const S_UNIT_BASE_DBFS = -103;
  const DB_PER_S_UNIT = 6;

  /**
   * S9 in dBm, IARU Region 1 Technical Recommendation R.1: below 30 MHz, S9 is
   * -73 dBm at the receiver input and one S-unit is 6 dB.
   */
  const S9_DBM = -73;

  const level = computed(() => meter.value?.dbfs ?? -140);

  const peakDbfs = box(-140);
  let peakAt = 0;

  /**
   * The last half minute of level, for the face that shows it.
   *
   * A meter answers "how strong now". On HF that is rarely the question: a
   * signal that is S9 and sinking is a different thing from one that is S9 and
   * rising, and neither a bar nor a needle can tell them apart. Thirty seconds
   * at four samples a second is enough to see a fade arrive.
   */
  const HISTORY_SECONDS = 30;
  const HISTORY_HZ = 4;
  const history = box<number[]>([]);
  let historyAt = 0;

  watch(() => {
    const now = level.value;
    const stamp = typeof performance !== 'undefined' ? performance.now() : Date.now();
    const elapsed = peakAt === 0 ? 0 : (stamp - peakAt) / 1000;
    peakAt = stamp;
    peakDbfs.value = nextPeak(peakDbfs.peek(), now, elapsed);

    // Sampled on a clock rather than on every reading, so the trace has an
    // honest time axis whatever rate the telemetry happens to arrive at.
    if (stamp - historyAt >= 1000 / HISTORY_HZ) {
      historyAt = stamp;
      const next = [...history.peek(), now];
      history.value = next.slice(-HISTORY_SECONDS * HISTORY_HZ);
    }
  });

  /**
   * The operator's correction at the frequency in use, interpolated between the
   * points either side and held flat beyond the outermost. Held flat rather than
   * extrapolated on purpose: a line drawn through two points and followed past
   * them produces confident nonsense at the edges of a wide band.
   */
  function offsetAt(points: CalibrationPoint[], hz: number): number {
    if (points.length === 0) return 0;
    if (hz <= points[0].hz) return points[0].offset;
    const last = points[points.length - 1];
    if (hz >= last.hz) return last.offset;
    for (let i = 1; i < points.length; i++) {
      const high = points[i];
      if (hz > high.hz) continue;
      const low = points[i - 1];
      const span = high.hz - low.hz;
      if (span <= 0) return high.offset;
      return low.offset + ((hz - low.hz) / span) * (high.offset - low.offset);
    }
    return last.offset;
  }

  /** The calibration for the band in use, or null when there is none. */
  const calibration = computed(() => {
    const points = currentBand.value?.calibration;
    return points && points.length > 0 ? points : null;
  });

  /** Signal level in dBm, or null on an uncalibrated receiver. */
  const dbm = computed(() => {
    const points = calibration.value;
    if (!points) return null;
    return level.value + offsetAt(points, tuning.value.freq);
  });

  /**
   * `base` is the level that reads as S1: derived from the operator's
   * calibration where there is one, and a guess where there is not. Same curve
   * either way, so a calibrated receiver behaves exactly as this one always has
   * and only the number it is anchored to changes.
   */
  function toSUnits(dbfs: number, base: number): { label: string; fraction: number } {
    const above = dbfs - base;
    const units = above / DB_PER_S_UNIT;
    if (units >= 9) {
      const over = Math.round((units - 9) * DB_PER_S_UNIT);
      return { label: `S9+${Math.max(0, over)}`, fraction: Math.min(1, 0.6 + (units - 9) / 10 / 2.5) };
    }
    const clamped = Math.max(0, Math.min(9, units));
    return { label: `S${Math.max(1, Math.round(clamped))}`, fraction: (clamped / 9) * 0.6 };
  }

  const NEEDLE_MARKS = [
    { at: 0, label: '1' },
    { at: 0.133, label: '3' },
    { at: 0.267, label: '5' },
    { at: 0.4, label: '7' },
    { at: 0.6, label: '9' },
    { at: 0.8, label: '+20' },
    { at: 1, label: '+40' },
  ];

  /** A point on the dial, `t` of the way from S1 to S9+40. */
  function dialPoint(t: number, radius: number): [number, number] {
    const radians = ((-60 + t * 120) - 90) * (Math.PI / 180);
    return [50 + radius * Math.cos(radians), 46 + radius * Math.sin(radians)];
  }

  const unit = (fraction: number) => Math.max(0, Math.min(1, fraction));

  /** The S-units between the numbered ones, and +10 and +30: a printed dial has them. */
  const MINOR_MARKS = [0.075, 0.2, 0.333, 0.467, 0.7, 0.9];
</script>

<script lang="ts">
  import { formatSigned } from '../util/frequency';
  import { dsp, operatorTheme } from '../state/store';
  import { layout, meterFace } from '../state/layout';

  const dbfs = $derived(level.value);
  const absolute = $derived(dbm.value);
  // S1 sits eight S-units below S9. On a calibrated receiver that lands on a
  // real dBm; otherwise it falls back to the guess.
  const base = $derived(
    absolute === null ? S_UNIT_BASE_DBFS : dbfs - (absolute - (S9_DBM - 8 * DB_PER_S_UNIT)),
  );
  const reading = $derived(toSUnits(dbfs, base));
  const peak = $derived(toSUnits(peakDbfs.value, base).fraction);
  const squelchOpen = $derived(meter.value?.squelch_open ?? true);
  const statistic = $derived(meter.value?.squelch_statistic ?? null);
  // SAM's carrier: how far the tracked carrier sits from the dial, which is
  // how a listener sees a station off its nominal frequency, or tunes to it.
  // Present only while the server runs SAM.
  const pllOffset = $derived(meter.value?.pll_offset ?? null);
  const pllLocked = $derived(meter.value?.pll_locked ?? false);
  // Either kind of squelch can mute, so either kind has to be able to say so.
  const squelchActive = $derived(dsp.value.squelch > -190 || dsp.value.autoSquelch);

  // Which face the operator chose. All four read the same calibrated value
  // underneath; the bar is the default and what this receiver has always
  // shown, so an operator who has expressed no preference sees no change.
  const face = $derived(meterFace(layout.value.meter, operatorTheme.value?.meter));
</script>

<!--
  A needle against a printed scale, which is what the instrument this
  imitates actually looks like.

  The scale is drawn once as an arc with its marks on it; only the needle
  moves, and it moves with a settle rather than a snap - a real one has mass.
  Deliberately not on the tuning path: this reads a level that is already
  smoothed over tens of milliseconds, so easing it costs nothing anyone can
  feel, where easing a frequency readout would read as latency.
-->
{#snippet needle(fraction: number, peak: number)}
  <!-- The dial covers 120 degrees, S1 on the left and S9+40 on the right. -->
  {@const angle = -60 + unit(fraction) * 120}
  <svg class="smeter__dial" viewBox="0 0 100 52" aria-hidden="true">
    <path
      d="M {dialPoint(0, 31).join(' ')} A 31 31 0 0 1 {dialPoint(1, 31).join(' ')}"
      class="smeter__arc"
      fill="none"
    />
    <!-- The stretch above S9 drawn as its own arc rather than only as
         coloured ticks: on a real dial that band is printed, and it is the
         one distinction on this scale that means something. -->
    <path
      d="M {dialPoint(0.6, 31).join(' ')} A 31 31 0 0 1 {dialPoint(1, 31).join(' ')}"
      class="smeter__arc smeter__arc--over"
      fill="none"
    />
    {#each NEEDLE_MARKS as mark (mark.label)}
      {@const [x1, y1] = dialPoint(mark.at, 27)}
      {@const [x2, y2] = dialPoint(mark.at, 31)}
      <!-- Outside the arc, where there is room. Inside, on a shorter radius,
           the same angular spacing crowds the low end into an unreadable
           huddle and the needle sits on top of the nine. Upright rather than
           rotated: a dial is read at a glance, and tilted digits are not. -->
      {@const [tx, ty] = dialPoint(mark.at, 38)}
      <g>
        <line {x1} {y1} {x2} {y2}
              class={mark.at >= 0.6 ? 'smeter__mark smeter__mark--over' : 'smeter__mark'} />
        <text x={tx} y={ty + 2}
              class={mark.at >= 0.6 ? 'smeter__numeral smeter__numeral--over' : 'smeter__numeral'}
              text-anchor="middle">
          {mark.label}
        </text>
      </g>
    {/each}
    {#each MINOR_MARKS as at (at)}
      {@const [x1, y1] = dialPoint(at, 29)}
      {@const [x2, y2] = dialPoint(at, 31)}
      <line {x1} {y1} {x2} {y2} class={at >= 0.6 ? 'smeter__mark smeter__mark--over smeter__mark--minor' : 'smeter__mark smeter__mark--minor'} />
    {/each}
    <!-- The trailing pointer: where the signal reached recently. On a real
         movement this is a second, lighter needle held by the first, and it
         is the part that answers "how strong was that". -->
    <!-- Both drawn upright and turned about the pivot: a turn can be
         animated, which is what makes the needle move rather than jump
         between readings. -->
    <g class="smeter__turn" style:transform="rotate({-60 + unit(peak) * 120}deg)">
      <line x1="50" y1={46 - 23} x2="50" y2={46 - 30} class="smeter__peak-needle" />
    </g>
    <g class="smeter__turn smeter__turn--needle" style:transform="rotate({angle}deg)">
      <line x1="50" y1="46" x2="50" y2={46 - 28} class="smeter__needle" />
    </g>
    <circle cx="50" cy="46" r="2.5" class="smeter__pivot" />
    <title>{`${angle.toFixed(0)} degrees`}</title>
  </svg>
{/snippet}

<!--
  The trace: level over the last half minute, with now at the right.

  Drawn against the same scale the other faces use, so a glance across from
  the bar to this one does not need recalibrating.
-->
{#snippet trace(base: number)}
  {@const samples = history.value}
  {#if samples.length < 2}
    <div class="smeter__trace smeter__trace--waiting" aria-hidden="true"></div>
  {:else}
    {@const width = 100}
    {@const height = 26}
    {@const points = samples
      .map((db, index) => {
        const x = (index / (samples.length - 1)) * width;
        const y = height - unit(toSUnits(db, base).fraction) * height;
        return `${x.toFixed(1)},${y.toFixed(1)}`;
      })
      .join(' ')}
    <svg class="smeter__trace" viewBox="0 0 {width} {height}" preserveAspectRatio="none"
         aria-hidden="true">
      <!-- S9, which is the line a report is judged against. -->
      <line x1="0" y1={height - 0.6 * height} x2={width} y2={height - 0.6 * height}
            class="smeter__trace-rule" />
      <polyline {points} class="smeter__trace-line" fill="none" />
    </svg>
  {/if}
{/snippet}

<div class="smeter smeter--{face}" role="meter" aria-valuenow={Math.round(dbfs)}
     aria-valuemin={-140} aria-valuemax={0} aria-label="Signal strength">
  {#if face === 'needle'}
    {@render needle(reading.fraction, peak)}
  {:else if face === 'history'}
    {@render trace(base)}
  {:else if face !== 'numeric'}
    <div class="smeter__bar">
      <div class="smeter__fill" style:width="{unit(reading.fraction) * 100}%"></div>
      <!-- Where the signal reached recently. A meter that only shows now
           cannot answer "how strong was that", which is what a report is. -->
      <span class="smeter__peak" style:left="{unit(peak) * 100}%"></span>
      {#each [1, 3, 5, 7, 9] as s}
        <span class="smeter__tick" style:left="{((s - 1) / 9) * 60}%" data-label="{s}"></span>
      {/each}
      <span class="smeter__tick smeter__tick--plus" style:left="60%" data-label="9"></span>
    </div>
  {/if}
  <div class="smeter__readout">
    <strong class="smeter__units">{reading.label}</strong>
    <span class="smeter__dbfs">
      {absolute === null ? `${formatSigned(dbfs, 1)} dBFS` : `${formatSigned(absolute, 0)} dBm`}
    </span>
    <!-- On the numeric face the peak is a number rather than a marker,
         because a face that exists to be read exactly should not make you
         estimate the one value you would actually write down. -->
    {#if face === 'numeric'}
      <span class="smeter__peak-value">
        pk {absolute === null
          ? `${formatSigned(peakDbfs.value, 1)} dBFS`
          : `${formatSigned(peakDbfs.value + (absolute - dbfs), 0)} dBm`}
      </span>
    {/if}
    {#if squelchActive}
      <!-- What the automatic squelch is deciding on, so the decision is
           visible rather than only audible. 18 opens at once and 5 opens
           on repetition; a reading that hovers just under the threshold
           explains a squelch that will not open. -->
      <span
        class="smeter__squelch{squelchOpen ? ' is-open' : ''}"
        title={statistic === null
          ? undefined
          : `Passband shape ${statistic.toFixed(1)}; noise reads near 0, 18 opens at once`}
      >
        {squelchOpen ? 'open' : 'muted'}
      </span>
    {/if}
    {#if pllOffset !== null}
      <span class="smeter__pll{pllLocked ? ' is-locked' : ''}"
            title="Where SAM finds the carrier, against the dial">
        <span class="smeter__pll-dot" aria-hidden="true"></span>
        SAM <span class="smeter__pll-offset">{pllLocked ? `${pllOffset >= 0 ? '+' : '\u2212'}${Math.abs(pllOffset).toFixed(1)} Hz` : 'searching'}</span>
      </span>
    {/if}
  </div>
</div>
