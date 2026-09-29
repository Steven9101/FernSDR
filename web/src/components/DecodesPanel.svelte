<script lang="ts" module>
  import { lazy } from './lazy';

  // In the module for the same reason as the history's: the panel is created
  // afresh on each visit to its tab.
  const loadDecodesView = lazy(() => import('./decodes/DecodesView.svelte'));
</script>

<script lang="ts">
  /**
   * What the operator's public decoders heard: FT8 and the like, newest first,
   * each a click away from being listened to. Only present when the operator
   * made a decoder public.
   */
  import Panel from './Panel.svelte';

  let expanded = $state(false);
  let dialog = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    if (expanded && dialog && !dialog.open) dialog.showModal();
  });
</script>

<Panel title="Decodes">
  {#await loadDecodesView()}
    <p role="status">Loading decodes…</p>
  {:then DecodesView}
    {#if expanded}
      <dialog class="history-dialog" bind:this={dialog} aria-label="Decodes" onclose={() => (expanded = false)}>
        <DecodesView expanded onClose={() => dialog?.close()} />
      </dialog>
      <p class="history__note">Shown full size.</p>
    {:else}
      <DecodesView onExpand={() => (expanded = true)} />
    {/if}
  {:catch}
    <p role="alert">The decodes did not load. Reload the page to try again.</p>
  {/await}
</Panel>
