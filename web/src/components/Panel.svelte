<script lang="ts">
  import type { Snippet } from 'svelte';

  interface Props {
    title: string;
    children: Snippet;
    /** Shown to the right of the title; a live value or a status word. */
    aside?: Snippet;
  }

  let { title, children, aside }: Props = $props();
</script>

<section class="panel">
  <header class="panel__header">
    <h2 class="panel__title">{title}</h2>
    {#if aside}<div class="panel__aside">{@render aside()}</div>{/if}
  </header>
  <div class="panel__body">{@render children()}</div>
</section>
