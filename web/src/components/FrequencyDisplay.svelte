<script lang="ts">
  /**
   * The frequency readout, and the primary way to tune.
   *
   * Every digit is independently draggable, scrollable and focusable, so you
   * change the place you mean rather than nudging a single step size up and
   * down a band. This is the interaction good radios have had for decades and
   * most web receivers do not, and it is the one that makes the difference
   * between hunting for a signal and going to it.
   *
   * Typing works too: click the readout, type a frequency in almost any form
   * (14074, 14.074, 7.1M, 3690 kHz) and press Enter.
   */
  import { controller, currentBand, rds, signalFreq, site, tuning } from '../state/store';
  import { describeFrequency, planFor } from '../state/bandplan';
  import { programmeType, stationName } from '../util/rds';
  import { layout } from '../state/layout';
  import ToolsRow from './ToolsRow.svelte';
  import { parseFrequency } from '../util/frequency';
  import { digitsOf, isLeadingZero, placesFor } from './frequency-digits';

  /** Vertical travel that moves a digit by one step. */
  const DRAG_PIXELS_PER_STEP = 14;

  let editing = $state(false);
  let draft = $state('');
  let input = $state<HTMLInputElement | null>(null);
  let drag: { place: number; startY: number; startHz: number; applied: number; moved: boolean } | null = null;

  $effect(() => {
    if (editing && input) {
      input.focus();
      input.select();
    }
  });

  // The signal, not the carrier: in CW the two differ by the pitch, and the
  // number an operator reads, types, drags and shares is the signal.
  const frequency = $derived(signalFreq.value);
  const band = $derived(currentBand.value);
  const places = $derived(placesFor(Math.max(band?.high ?? 0, frequency)));
  const digits = $derived(digitsOf(frequency, places));
  const bandLabel = $derived(describeFrequency(frequency));
  // What an FM station says about itself, as a car radio shows it: the
  // name and the kind of programme on the dial, the radiotext under them.
  const station = $derived(tuning.value.mode === 'wfm' ? rds.value : null);
  const stationLabel = $derived(stationName(station?.ps));
  const programme = $derived(
    programmeType(station?.pty, planFor(site.value?.band_plan, site.value?.grid)?.region === 2),
  );
  const radiotext = $derived(station?.rt?.trim() ?? '');

  function nudge(place: number, direction: number) {
    let next = frequency + place * direction;
    if (band) next = Math.min(band.high, Math.max(band.low, next));
    controller.tune(Math.round(next));
  }

  function onDigitPointerDown(event: PointerEvent & { currentTarget: HTMLElement }, place: number) {
    event.currentTarget.setPointerCapture(event.pointerId);
    drag = { place, startY: event.clientY, startHz: frequency, applied: 0, moved: false };
  }

  function onDigitPointerMove(event: PointerEvent) {
    if (!drag) return;
    // Up increases, which matches every physical tuning knob and dial.
    const steps = Math.trunc((drag.startY - event.clientY) / DRAG_PIXELS_PER_STEP);
    if (steps === drag.applied) return;
    drag.moved = true;
    drag.applied = steps;
    let next = drag.startHz + steps * drag.place;
    if (band) next = Math.min(band.high, Math.max(band.low, next));
    controller.tune(Math.round(next));
  }

  function onDigitPointerUp(event: PointerEvent & { currentTarget: HTMLElement }) {
    const ended = drag;
    drag = null;
    if (!ended) return;
    // A press with no travel is a click: the half you clicked decides the
    // direction, so a single digit is both "up" and "down" without needing
    // separate arrows that would be too small to hit on a phone.
    if (!ended.moved && event.type === 'pointerup') {
      const rect = event.currentTarget.getBoundingClientRect();
      nudge(ended.place, event.clientY < rect.top + rect.height / 2 ? 1 : -1);
    }
  }

  function commit() {
    const parsed = parseFrequency(draft);
    editing = false;
    if (parsed !== null) controller.tune(Math.round(parsed));
  }
</script>

{#if editing}
  <div class="frequency frequency--editing">
    <input
      bind:this={input}
      class="frequency__input"
      bind:value={draft}
      inputmode="decimal"
      aria-label="Frequency"
      onkeydown={(event) => {
        if (event.key === 'Enter') commit();
        if (event.key === 'Escape') editing = false;
      }}
      onblur={commit}
    />
    <span class="frequency__hint">Enter to tune &middot; MHz unless you say otherwise</span>
  </div>
{:else}
  <div class="frequency">
    <div
      class="frequency__digits"
      role="group"
      aria-label="Tuned to {(frequency / 1e6).toFixed(6)} megahertz"
    >
      {#each places as place, index}
        <!-- Group as MHz.kHz.Hz by the place itself, not by a fixed index:
             the index version broke the moment a digit was added. -->
        {#if place === 1e5 || place === 1e2}<span class="frequency__separator">.</span>{/if}
        <button
          type="button"
          class="frequency__digit{isLeadingZero(digits, places, index) ? ' frequency__digit--dim' : ''}"
          onpointerdown={(event) => onDigitPointerDown(event, place)}
          onpointermove={onDigitPointerMove}
          onpointerup={onDigitPointerUp}
          onpointercancel={() => (drag = null)}
          onwheel={(event) => {
            event.preventDefault();
            nudge(place, event.deltaY < 0 ? 1 : -1);
          }}
          onkeydown={(event) => {
            if (event.key === 'ArrowUp') { event.preventDefault(); nudge(place, 1); }
            if (event.key === 'ArrowDown') { event.preventDefault(); nudge(place, -1); }
          }}
          aria-label="{place >= 1e6 ? `${place / 1e6} megahertz` : `${place} hertz`} digit, currently {digits[index]}"
          style:touch-action="none"
        >
          {digits[index]}
        </button>
      {/each}
      <span class="frequency__unit">MHz</span>
    </div>

    <div class="frequency__meta">
      <button
        type="button"
        class="frequency__edit"
        onclick={() => {
          draft = (frequency / 1e6).toFixed(6).replace(/0+$/, '').replace(/\.$/, '');
          editing = true;
        }}
      >
        Type a frequency
      </button>
      {#if bandLabel && layout.value.show.band}<span class="frequency__band">{bandLabel}</span>{/if}
      {#if layout.value.show.tools}<ToolsRow />{/if}
    </div>
    {#if stationLabel || radiotext}
      <div class="frequency__rds" title={radiotext || undefined}>
        {#if stationLabel}
          <p class="frequency__station">
            <span class="frequency__ps">{stationLabel}</span>
            {#if programme}<span class="frequency__pty">{programme}</span>{/if}
          </p>
        {/if}
        {#if radiotext}<p class="frequency__radiotext">{radiotext}</p>{/if}
      </div>
    {/if}
  </div>
{/if}
