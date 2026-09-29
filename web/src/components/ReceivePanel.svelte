<script lang="ts">
  /**
   * The control panels: receive, display, history, connection.
   *
   * Grouped by what a user is trying to do rather than by which subsystem
   * implements it. "I can't hear it well" is the receive panel; "this is eating
   * my data" is the connection panel.
   */
  import Field from './Field.svelte';
  import Panel from './Panel.svelte';
  import Rack from './Rack.svelte';
  import Segmented from './Segmented.svelte';
  import Slider from './Slider.svelte';
  import Toggle from './Toggle.svelte';
  import { controller, currentBand, display, dsp, meter, tuning, wfmDeemphasis } from '../state/store';
  import { formatSpan, formatSigned } from '../util/frequency';
  import { displayPassband, passbandShift, presetPassband, shiftedPassband } from '../util/passband';
  import { setTone, tone, toneGroup, TONE_RANGE_DB } from '../state/tone';
  import { CTCSS_TONES } from '../util/ctcss';

  const MODES = [
    { value: 'usb', label: 'USB', title: 'Upper sideband', formValue: 'USB' },
    { value: 'lsb', label: 'LSB', title: 'Lower sideband', formValue: 'LSB' },
    { value: 'cw', label: 'CW', title: 'Morse, upper side', formValue: 'CW' },
    { value: 'cwl', label: 'CW-L', title: 'Morse, lower side', formValue: 'CW' },
    { value: 'am', label: 'AM', title: 'Envelope detection', formValue: 'AM' },
    { value: 'sam', label: 'SAM', title: 'Synchronous AM: steadier through fading', formValue: 'AM' },
    { value: 'nfm', label: 'NFM', title: 'Narrow FM', formValue: 'FM' },
    { value: 'dsb', label: 'DSB', title: 'Both sidebands', formValue: 'AM' },
    { value: 'wfm', label: 'WFM', title: 'Broadcast FM', formValue: 'FM' },
  ] as const;

  // One gain control for every mode: it holds the band noise at least 15 dB
  // under the signal, and the speeds differ in how long it holds its gain
  // after the signal before letting it rise again.
  const AGC_PROFILES = [
    { value: 'auto', label: 'Auto', title: 'The recommended setting, Slow' },
    { value: 'fast', label: 'Fast', title: 'Holds 0.3 s: fast fading, contests' },
    { value: 'medium', label: 'Med', title: 'Holds 1 s' },
    { value: 'slow', label: 'Slow', title: 'Holds 2.5 s, through the pause between overs' },
    { value: 'long', label: 'Long', title: 'Holds 5 s: nets with long pauses' },
    { value: 'off', label: 'Off', title: 'Manual gain' },
  ] as const;

  // Audio cut below these after demodulation. SSB's own filter already starts at
  // 300 Hz or so; this is for AM hum and the subaudible tones under NFM speech.
  const LOW_CUTS = [
    { value: '0', label: 'Off', title: 'Everything the filter passes' },
    { value: '100', label: '100', title: 'Mains hum' },
    { value: '200', label: '200', title: 'Hum and rumble' },
    { value: '300', label: '300 Hz', title: 'Also the subaudible tones under NFM speech' },
  ] as const;

  // NFM de-emphasis: the transmitter's treble boost taken back out.
  const DEEMPHASIS = [
    { value: '0', label: 'Off', title: 'Flat, for a decoder that wants the signal as sent' },
    { value: '300', label: '300 µs', title: 'Brighter: the receiver\'s usual choice' },
    { value: '750', label: '750 µs', title: 'The land mobile standard, TIA-603' },
  ] as const;

  // Broadcast FM de-emphasis: what the station's region uses, unless chosen.
  const WFM_DEEMPHASIS = [
    { value: '50', label: '50 µs', title: 'Europe, Africa, Asia and Oceania' },
    { value: '75', label: '75 µs', title: 'The Americas and South Korea' },
    { value: '0', label: 'Off', title: 'Flat, as transmitted' },
  ] as const;

  /** Filter presets, in Hz relative to the tuning point. */
  const FILTER_PRESETS: Record<string, { label: string; low: number; high: number }[]> = {
    ssb: [
      { label: '1.8 k', low: 300, high: 2100 },
      { label: '2.4 k', low: 300, high: 2700 },
      { label: '2.7 k', low: 200, high: 2900 },
      { label: '3.6 k', low: 200, high: 3800 },
    ],
    cw: [
      { label: '100', low: -50, high: 50 },
      { label: '250', low: -125, high: 125 },
      { label: '500', low: -250, high: 250 },
      { label: '1 k', low: -500, high: 500 },
    ],
    am: [
      { label: '4 k', low: -2000, high: 2000 },
      { label: '6 k', low: -3000, high: 3000 },
      { label: '9 k', low: -4500, high: 4500 },
      { label: '12 k', low: -6000, high: 6000 },
    ],
    // Narrower than 200 kHz for a weak station beside a strong one.
    wfm: [
      { label: '120 k', low: -60000, high: 60000 },
      { label: '150 k', low: -75000, high: 75000 },
      { label: '180 k', low: -90000, high: 90000 },
      { label: '200 k', low: -100000, high: 100000 },
    ],
  };

  function presetsForMode(mode: string) {
    if (mode === 'cw' || mode === 'cwl') return FILTER_PRESETS.cw;
    if (mode === 'wfm') return FILTER_PRESETS.wfm;
    if (mode === 'am' || mode === 'sam' || mode === 'nfm' || mode === 'dsb') return FILTER_PRESETS.am;
    return FILTER_PRESETS.ssb;
  }

  const tune = $derived(tuning.value);
  // Broadcast FM is only offered where the band is sampled wide enough for it.
  const modes = $derived(currentBand.value?.wfm ? MODES : MODES.filter((mode) => mode.value !== 'wfm'));
  const fm = $derived(tune.mode === 'nfm' || tune.mode === 'wfm');
  const settings = $derived(dsp.value);
  const bandwidth = $derived(Math.abs(tune.high - tune.low));
  const presets = $derived(presetsForMode(tune.mode));
  const filterOptions = $derived(presets.map((preset) => ({ value: preset.label, label: preset.label })));
  // Tone, low cut and de-emphasis shape the sound for a listener who knows
  // what they want from it; the rest of the panel is for everyone.
  const full = $derived(display.value.controls === 'full');
  // The fine filter controls, in the operator's terms (see displayPassband).
  const shown = $derived(displayPassband(tune.mode, tune.cwPitch, tune.low, tune.high));
  const shift = $derived(passbandShift(tune.mode, tune.cwPitch, tune.low, tune.high));

  function setEdge(edge: 'low' | 'high', text: string) {
    const value = Number(text);
    if (!Number.isFinite(value)) return;
    const next = { ...shown, [edge]: Math.round(value) };
    if (next.high - next.low < 50) return;
    const filter = presetPassband(tune.mode, tune.cwPitch, next.low, next.high);
    controller.setPassband(filter.low, filter.high);
  }

  const toneSetting = $derived(tone.value[toneGroup(tune.mode)]);
  const TONE_FOR = { am: 'AM', ssb: 'SSB', cw: 'CW', fm: 'FM' } as const;
  const formatTone = (value: number) => (value === 0 ? 'flat' : `${value > 0 ? '+' : ''}${formatSigned(value, 0)} dB`);
</script>

<Panel title="Receive">
  <Rack title="Demodulator" aside={formatSpan(bandwidth)}>
  <Field label="Mode" stacked>
    <Segmented
      label="Mode"
      wrap
      columns={modes.length > 8 ? 5 : 4}
      options={modes}
      value={tune.mode as (typeof MODES)[number]['value']}
      onChange={(mode) => controller.setMode(mode)}
    />
  </Field>

  {#if tune.mode === 'sam' && meter.value?.pll_offset !== undefined}
    <!-- What SAM's carrier loop is doing, where the listener chose SAM: locked
         and how far the station's carrier sits from the dial, or still
         looking for it. Only a server that runs SAM sends it. -->
    <!-- The word and the offset in columns of their own, so that the row
         keeps its shape as the lock comes and goes and the offset moves. -->
    <Field
      label="Carrier"
      title="SAM follows the station's carrier; the value is how far it sits from the dial"
      value={meter.value.pll_locked
        ? `${meter.value.pll_offset >= 0 ? '+' : '\u2212'}${Math.abs(meter.value.pll_offset).toFixed(1)} Hz`
        : '\u2013'}
    >
      <span class="carrier-lock{meter.value.pll_locked ? ' is-locked' : ''}" role="status">
        <span class="carrier-lock__dot" aria-hidden="true"></span>
        <span class="carrier-lock__word">{meter.value.pll_locked ? 'Locked' : 'Searching'}</span>
      </span>
    </Field>
  {/if}

  {#if tune.mode === 'nfm' && meter.value?.ctcss !== undefined}
    <!-- The subaudible tone under the voice, measured by the receiver: what
         a repeater wants to hear, and what tells two users of a channel apart. -->
    <Field label="CTCSS" title="The subaudible tone the station sends, measured over two seconds"
      value={meter.value.ctcss > 0 ? `${meter.value.ctcss.toFixed(1)} Hz` : 'none'}>
      <span></span>
    </Field>
    <Toggle
      label="Remove the tone"
      checked={settings.ctcssFilter}
      onChange={(ctcssFilter) => controller.setDsp({ ctcssFilter })}
      title="Takes the measured tone out of what you hear with a narrow notch, and leaves the voice as it is."
    />
    {#if full}
      <Field label="Tone squelch" stacked title="Stay silent unless the station sends this tone: only the users of one repeater or group, not everyone on the channel">
        <select class="field__select" aria-label="Tone squelch" value={settings.ctcssSquelch}
          onchange={(event) => controller.setDsp({ ctcssSquelch: Number(event.currentTarget.value) })}>
          <option value={0}>Off</option>
          {#each CTCSS_TONES as hz (hz)}
            <option value={hz}>{hz.toFixed(1)} Hz{meter.value?.ctcss === hz ? ', heard now' : ''}</option>
          {/each}
        </select>
      </Field>
    {/if}
  {/if}

  <Field label="Filter" stacked>
    <Segmented
      label="Filter width"
      options={filterOptions}
      value={presets.find(
        (preset) => Math.abs(Math.abs(preset.high - preset.low) - bandwidth) < 30,
      )?.label ?? ''}
      onChange={(label) => {
        const preset = presets.find((p) => p.label === label);
        if (!preset) return;
        const filter = presetPassband(tune.mode, tune.cwPitch, preset.low, preset.high);
        controller.setPassband(filter.low, filter.high);
      }}
    />
  </Field>

  {#if full}
    <Field label="Edges" title="The passband's edges in hertz: audio tones for SSB, around the note for CW, from the carrier for AM and FM">
      <span class="edges">
        <input class="edges__input" type="number" inputmode="numeric" step="10" value={Math.round(shown.low)}
          aria-label="Lower edge in hertz" onchange={(event) => setEdge('low', event.currentTarget.value)} />
        <span aria-hidden="true">to</span>
        <input class="edges__input" type="number" inputmode="numeric" step="10" value={Math.round(shown.high)}
          aria-label="Upper edge in hertz" onchange={(event) => setEdge('high', event.currentTarget.value)} />
        <span class="edges__unit" aria-hidden="true">Hz</span>
      </span>
    </Field>
    <Slider
      label="Shift"
      min={-1000}
      max={1000}
      step={10}
      value={Math.max(-1000, Math.min(1000, shift))}
      onChange={(value) => {
        const filter = shiftedPassband(tune.mode, tune.cwPitch, tune.low, tune.high, value);
        controller.setPassband(filter.low, filter.high);
      }}
      format={(value) => (value === 0 ? 'centred' : `${value > 0 ? '+' : ''}${formatSigned(value, 0)} Hz`)}
      title="IF shift: moves the passband up or down without changing its width, away from a neighbour."
    />
  {/if}

  <!-- FM's discriminator hears only the phase, so no gain ahead of it
       changes what comes out; the receiver runs FM without one. -->
  <Field
    label="AGC"
    value={fm ? 'None on FM' : meter.value ? `${formatSigned(meter.value.gain_db, 0)} dB` : ''}
    stacked
  >
    <Segmented
      label="AGC"
      options={AGC_PROFILES}
      value={settings.agc as (typeof AGC_PROFILES)[number]['value']}
      onChange={(agc) => controller.setDsp({ agc })}
      disabled={fm}
    />
  </Field>

  {#if full}
    <Field label="Low cut" stacked>
      <Segmented
        label="Low cut"
        options={LOW_CUTS}
        value={String(settings.highpass) as (typeof LOW_CUTS)[number]['value']}
        onChange={(value) => controller.setDsp({ highpass: Number(value) })}
      />
    </Field>

    {#if tune.mode === 'nfm'}
      <Field label="De-emphasis" stacked>
        <Segmented
          label="De-emphasis"
          options={DEEMPHASIS}
          value={String(settings.deemphasis) as (typeof DEEMPHASIS)[number]['value']}
          onChange={(value) => controller.setDsp({ deemphasis: Number(value) })}
        />
      </Field>
    {:else if tune.mode === 'wfm'}
      <Field label="De-emphasis" stacked>
        <Segmented
          label="De-emphasis"
          options={WFM_DEEMPHASIS}
          value={String(wfmDeemphasis(settings)) as (typeof WFM_DEEMPHASIS)[number]['value']}
          onChange={(value) => controller.setDsp({ wfmDeemphasis: Number(value) })}
        />
      </Field>
    {/if}
  {/if}

  {#if settings.agc === 'off' && !fm}
    <Slider
      label="Gain"
      min={-20}
      max={60}
      value={settings.gain}
      onChange={(gain) => controller.setDsp({ gain })}
      format={(value) => `${value} dB`}
    />
  {/if}
  </Rack>

  {#if full}
  <Rack title="Tone" aside="for {TONE_FOR[toneGroup(tune.mode)]}">
    <Slider
      label="Bass"
      min={-TONE_RANGE_DB}
      max={TONE_RANGE_DB}
      value={toneSetting.bass}
      onChange={(bass) => setTone(tune.mode, { bass })}
      format={formatTone}
      title="Below about 300 Hz. Kept separately for AM, SSB, CW and FM, in this browser."
    />
    <Slider
      label="Treble"
      min={-TONE_RANGE_DB}
      max={TONE_RANGE_DB}
      value={toneSetting.treble}
      onChange={(treble) => setTone(tune.mode, { treble })}
      format={formatTone}
      title="Above about 1.8 kHz. Kept separately for AM, SSB, CW and FM, in this browser."
    />
  </Rack>
  {/if}

  <Rack title="Noise">
    <Slider
      label="Reduction"
      min={0}
      max={100}
      value={Math.round(settings.nr * 100)}
      onChange={(value) => controller.setDsp({ nr: value / 100 })}
      format={(value) => (value === 0 ? 'off' : `${value}%`)}
      title="Learns the noise floor from the gaps between signals. A carrier keyed down continuously looks like noise to it; use the auto-notch for those."
    />

    <Toggle
      label="Auto notch"
      checked={settings.autonotch}
      onChange={(autonotch) => controller.setDsp({ autonotch })}
      title="Pulls down steady heterodynes without touching the rest of the passband."
    />

    <Toggle
      label="Auto squelch"
      checked={settings.autoSquelch}
      onChange={(autoSquelch) => controller.setDsp({ autoSquelch })}
      title="Mutes when the passband looks like noise rather than when it is quiet, so it needs no threshold and does not have to be reset when the band changes. The manual squelch below still applies; either can mute."
    />

    <Slider
      label="Squelch"
      min={-140}
      max={-20}
      value={settings.squelch < -190 ? -140 : settings.squelch}
      onChange={(value) => controller.setDsp({ squelch: value <= -140 ? -200 : value })}
      format={(value) => (value <= -140 ? 'open' : `${formatSigned(value, 0)} dBFS`)}
    />
  </Rack>
</Panel>
