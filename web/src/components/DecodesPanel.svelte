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
  import { tick } from 'svelte';
  import Panel from './Panel.svelte';

  let expanded = $state(false);
  let dialog = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    if (expanded && dialog && !dialog.open) dialog.showModal();
  });

  // Kept here for the same reasons as the history's: enlarging and closing
  // create the view again, and neither should change what it shows, nor lose
  // the button focus returns to.
  let channel = $state('');
  let cqOnly = $state(false);
  let search = $state('');
  let holder = $state<HTMLDivElement | null>(null);
  async function closed() {
    expanded = false;
    await tick();
    holder?.querySelector<HTMLButtonElement>('[data-enlarge]')?.focus();
  }
</script>

<Panel title="Decodes">
  {#await loadDecodesView()}
    <p role="status">Loading decodes…</p>
  {:then DecodesView}
    <div class="panel__holder" bind:this={holder}>
    {#if expanded}
      <dialog class="history-dialog" bind:this={dialog} aria-label="Decodes" onclose={closed}>
        <DecodesView expanded bind:channel bind:cqOnly bind:search onClose={() => dialog?.close()} />
      </dialog>
      <p class="history__note">Shown full size.</p>
    {:else}
      <DecodesView bind:channel bind:cqOnly bind:search onExpand={() => (expanded = true)} />
    {/if}
    </div>
  {:catch}
    <p role="alert">The decodes did not load. Reload the page to try again.</p>
  {/await}
</Panel>
