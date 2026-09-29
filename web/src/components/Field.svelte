<script lang="ts">
  import type { Snippet } from 'svelte';

  interface Props {
    label: string;
    value?: string;
    /** Explanation, on hover and for screen readers. Never printed on the page. */
    title?: string;
    /** Puts the control on its own line, for anything a row cannot hold. */
    stacked?: boolean;
    children: Snippet;
  }

  let { label, value, title, stacked, children }: Props = $props();
</script>

<div class="field{stacked ? ' field--stacked' : ''}" {title}>
  <span class="field__label">{label}</span>
  <div class="field__control">{@render children()}</div>
  {#if value !== undefined}<span class="field__value">{value}</span>{/if}
</div>
