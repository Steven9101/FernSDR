<script lang="ts">
  import Field from './Field.svelte';
  import Panel from './Panel.svelte';
  import Rack from './Rack.svelte';
  import Segmented from './Segmented.svelte';
  import Slider from './Slider.svelte';
  import Toggle from './Toggle.svelte';
  import { applyTheme, display, operatorTheme, theme } from '../state/store';
  import { layout, meterFace, resetLayout, setLayout } from '../state/layout';
  import { MediaQuery } from 'svelte/reactivity';
  import { PALETTES, paletteGradient } from '../render/palettes';
  import { formatSigned } from '../util/frequency';

  const THEMES = [
    { value: 'auto', label: 'Auto', title: 'Follow the system setting' },
    { value: 'dark', label: 'Dark', title: 'Always dark' },
    { value: 'light', label: 'Light', title: 'Always light' },
  ] as const;

  const CONTROLS = [
    { value: 'essential', label: 'Essential', title: 'Mode, filter, AGC, noise and squelch' },
    { value: 'full', label: 'Full', title: 'Also bass and treble, low cut, de-emphasis and the finer filter controls' },
  ] as const;

  const METERS = [
    { value: 'bar', label: 'Bar', title: 'A bar with a peak marker' },
    { value: 'needle', label: 'Needle', title: 'An analog meter' },
    { value: 'numeric', label: 'Numbers', title: 'The reading and its peak as figures' },
    { value: 'history', label: 'Trace', title: 'The last half minute, to see a fade coming' },
  ] as const;
  const SIDES = [
    { value: 'left', label: 'Left' },
    { value: 'right', label: 'Right' },
  ] as const;
  // The column only has a side on a wide screen; the phone's sheet has none.
  const wide = new MediaQuery('(min-width: 1024px)');

  const settings = $derived(display.value);
  const own = $derived(layout.value);
</script>

<Panel title="Display">
  <Rack title="Appearance">
  <Field label="Theme">
    <Segmented
      label="Theme"
      options={THEMES}
      value={theme.value}
      onChange={(choice) => applyTheme(choice)}
    />
  </Field>

  <Field label="Controls">
    <Segmented
      label="Controls"
      options={CONTROLS}
      value={settings.controls}
      onChange={(controls) => (display.value = { ...settings, controls })}
    />
  </Field>

  <Field label="Colours" stacked>
    <div class="palette-grid">
      {#each PALETTES as palette}
        <button
          type="button"
          class="palette{settings.palette === palette.id ? ' is-active' : ''}"
          title={palette.description}
          aria-pressed={settings.palette === palette.id}
          onclick={() => (display.value = { ...settings, palette: palette.id, paletteChosen: true })}
        >
          <span class="palette__swatch" style:background={paletteGradient(palette.id)}></span><span class="palette__label">{palette.label}</span>
        </button>
      {/each}
    </div>
  </Field>

  </Rack>

  <Rack title="Layout">
    <Field label="Meter" stacked>
      <!-- The face in use is marked, the station's until the listener picks
           one; the reset below goes back to the station's. -->
      <Segmented label="Meter" options={METERS} value={meterFace(own.meter, operatorTheme.value?.meter)}
        onChange={(meter) => setLayout({ meter })} />
    </Field>
    {#if wide.current}
      <Field label="Controls">
        <Segmented label="Side of the controls" options={SIDES} value={own.side} onChange={(side) => setLayout({ side })} />
      </Field>
    {/if}
    <Toggle label="Meter on the dial" checked={own.show.meter} onChange={(meter) => setLayout({ show: { meter } })} />
    <Toggle label="Volume on the dial" checked={own.show.volume} onChange={(volume) => setLayout({ show: { volume } })} />
    <Toggle label="Band name" checked={own.show.band} onChange={(band) => setLayout({ show: { band } })} />
    <Toggle label="Tools under the dial" checked={own.show.tools} onChange={(tools) => setLayout({ show: { tools } })} />
    <button type="button" class="button button--small layout__reset" onclick={resetLayout}>Back to the station's layout</button>
  </Rack>

  <Rack title="Levels">
  <Toggle
    label="Automatic"
    checked={settings.autoLevels}
    onChange={(autoLevels) => (display.value = { ...settings, autoLevels })}
    title="Follows the noise floor as conditions change."
  />

  {#if !settings.autoLevels}
    <Slider
      label="Floor"
      min={-150}
      max={-40}
      value={Math.round(settings.floorDb)}
      onChange={(floorDb) => (display.value = { ...settings, floorDb })}
      format={(value) => `${formatSigned(value, 0)} dBFS`}
    />
    <Slider
      label="Ceiling"
      min={-100}
      max={10}
      value={Math.round(settings.ceilingDb)}
      onChange={(ceilingDb) => (display.value = { ...settings, ceilingDb })}
      format={(value) => `${formatSigned(value, 0)} dBFS`}
    />
  {/if}

  </Rack>

  <Rack title="Overlays">
    <Toggle
      label="Spectrum trace"
      checked={settings.showSpectrum}
      onChange={(showSpectrum) => (display.value = { ...settings, showSpectrum })}
    />
    <Toggle
      label="Band plan"
      checked={settings.showBandPlan}
      onChange={(showBandPlan) => (display.value = { ...settings, showBandPlan })}
      title="Amateur segments and the frequencies worth knowing about."
    />
  </Rack>
</Panel>
