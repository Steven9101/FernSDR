<script lang="ts" generics="T extends string">
  import type { Attachment } from 'svelte/attachments';
  import Tooltip from './Tooltip.svelte';

  interface SegmentedOption {
    value: T;
    label: string;
    title?: string;
    /**
     * The button's value attribute. Programs that sync a transceiver with a
     * web receiver read the mode from the value of the button clicked, as on
     * a PA3FWM WebSDR ("USB").
     */
    formValue?: string;
  }

  /** The active option's box, in the group's own coordinates. */
  interface ChipBox {
    x: number;
    y: number;
    width: number;
    height: number;
  }

  interface Props {
    label: string;
    options: readonly SegmentedOption[];
    value: T;
    onChange: (value: T) => void;
    /** Lets long lists wrap instead of scrolling off the edge. */
    wrap?: boolean;
    /** Columns when wrapping; four unless given. */
    columns?: number;
    /** Shown but not choosable, when the choice does nothing where it is. */
    disabled?: boolean;
  }

  let { label, options, value, onChange, wrap, columns, disabled }: Props = $props();

  let group: HTMLDivElement;
  // Where the chip sits. Null until measured, so it fades in on its mark
  // rather than sliding in from the corner on first paint.
  let chip = $state<ChipBox | null>(null);

  $effect(() => {
    void value;
    void options;
    void wrap;
    const container = group;
    const measure = () => {
      const active = container.querySelector<HTMLElement>('.segmented__item.is-active');
      if (!active) {
        chip = null;
        return;
      }
      chip = {
        x: active.offsetLeft,
        y: active.offsetTop,
        width: active.offsetWidth,
        height: active.offsetHeight,
      };
    };
    // The options are flexible, so the chip has to follow a resize as well as
    // a selection - otherwise it is left behind the moment the panel changes
    // width, and it is the one part of the control that cannot re-render
    // itself from the value alone.
    //
    // Not measured here as well: a new observer reports once on its own,
    // after layout and before the paint, and this effect makes a new one for
    // every selection. Reading offsetLeft here instead forced that layout in
    // the middle of mounting the page: 15 ms of main-thread time in a profile.
    // The items too, not only the group: when the web font arrives the
    // labels change width inside a group that keeps its own.
    const observer = new ResizeObserver(measure);
    observer.observe(container);
    for (const item of container.querySelectorAll('.segmented__item')) observer.observe(item);
    return () => observer.disconnect();
  });

  // A radio group is one stop for Tab, and the arrows move the choice, as
  // the admin panel's does: eight modes were eight Tab presses.
  const selected = $derived(Math.max(0, options.findIndex((option) => option.value === value)));

  function onKeyDown(event: KeyboardEvent) {
    if (disabled) return;
    const step = event.key === 'ArrowRight' || event.key === 'ArrowDown' ? 1
      : event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 0;
    if (!step) return;
    event.preventDefault();
    const next = (selected + step + options.length) % options.length;
    onChange(options[next].value);
    queueMicrotask(() => group.querySelectorAll<HTMLElement>('[role="radio"]')[next]?.focus());
  }
</script>

{#snippet item(option: SegmentedOption, attach?: Attachment<HTMLElement>)}
  <button
    {@attach attach}
    type="button"
    role="radio"
    aria-checked={option.value === value}
    tabindex={options[selected]?.value === option.value ? 0 : -1}
    {disabled}
    value={option.formValue}
    class="segmented__item{option.value === value ? ' is-active' : ''}"
    onclick={() => onChange(option.value)}
  >{option.label}</button>
{/snippet}

<div
  bind:this={group}
  class="segmented{wrap ? ' segmented--wrap' : ''}{disabled ? ' segmented--disabled' : ''}"
  style:--segmented-columns={columns}
  role="radiogroup"
  tabindex="-1"
  aria-label={label}
  aria-disabled={disabled || undefined}
  onkeydown={onKeyDown}
>
  {#if chip}
    <span
      class="segmented__chip"
      aria-hidden="true"
      style:width="{chip.width}px"
      style:height="{chip.height}px"
      style:transform="translate({chip.x}px, {chip.y}px)"
    ></span>
  {/if}
  {#each options as option}
    <!-- Only the options that have something to say get a tooltip; a tooltip
         that repeats the label is noise with a delay attached. -->
    {#if option.title}
      <Tooltip label={option.title}>
        {#snippet children(anchor)}{@render item(option, anchor)}{/snippet}
      </Tooltip>
    {:else}
      {@render item(option)}
    {/if}
  {/each}
</div>
