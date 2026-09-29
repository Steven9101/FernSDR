<script lang="ts">
  import Field from './Field.svelte';
  import Panel from './Panel.svelte';
  import Rack from './Rack.svelte';
  import Segmented from './Segmented.svelte';
  import Slider from './Slider.svelte';
  import Toggle from './Toggle.svelte';
  import { applyTheme, display, theme } from '../state/store';
  import { editingLayout } from '../state/layout';
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


  const settings = $derived(display.value);
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
    <!-- Where things are is arranged on the page itself, in the edit mode:
         a list of switches here would describe a page the listener is
         looking at anyway. -->
    <Field label="Page">
      <button type="button" id="customise-layout" class="button button--small" onclick={() => (editingLayout.value = true)}>
        Customise
      </button>
    </Field>
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
