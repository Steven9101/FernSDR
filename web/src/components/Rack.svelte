<script lang="ts">
  /**
   * Shared control primitives: Rack, Field, Segmented, Slider, Toggle and
   * Panel.
   *
   * One implementation of each control, so every knob has the same hit area, the
   * same keyboard behaviour and the same focus treatment.
   *
   * The shape they take is a rack, not a settings form. An earlier version gave
   * every control a stacked label, a full-width slider and, for two of them, a
   * paragraph of explanation underneath - which is what a configuration dialog
   * looks like, not what an instrument looks like. Real receivers, mixing desks
   * and audio plugins pack their controls: label and value on the same line as
   * the thing they label, rows a third the height, no prose anywhere. Anything
   * that genuinely needs explaining is a tooltip; anything that needs a
   * paragraph is a design problem, not a copy problem.
   *
   * This one is a group of related controls under one hairline header.
   */
  import type { Snippet } from 'svelte';

  interface Props {
    title: string;
    aside?: string;
    children: Snippet;
  }

  let { title, aside, children }: Props = $props();
</script>

<section class="rack">
  <header class="rack__head">
    <h3 class="rack__title">{title}</h3>
    {#if aside !== undefined}<span class="rack__aside">{aside}</span>{/if}
  </header>
  <div class="rack__body">{@render children()}</div>
</section>
