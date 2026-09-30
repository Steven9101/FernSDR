<script lang="ts" module>
  import { lazy } from './lazy';

  // In the module rather than the component: the panel is created afresh on
  // every visit to its tab, and a view it had to load again would arrive a
  // moment after the panel, once the tab switch had already measured the
  // height it animates to.
  const loadHistoryView = lazy(() => import('./HistoryView.svelte'));
</script>

<script lang="ts">
  /**
   * What this band looked like earlier. Only present when the operator keeps an
   * archive; a tab that always says "nothing here" is worse than no tab.
   */
  import { tick } from 'svelte';
  import Panel from './Panel.svelte';
  import { currentBand } from '../state/store';

  const band = $derived(currentBand.value);

  // The panel is a column beside the waterfall; the archive of a night wants
  // the whole window. A modal dialog, so Escape closes it and focus comes
  // back to the button that opened it.
  let expanded = $state(false);
  let dialog = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    if (expanded && dialog && !dialog.open) dialog.showModal();
  });

  // Kept here rather than in the view: enlarging and closing each create the
  // view again, and the listener is looking at the same stretch throughout.
  let hours = $state(0.25);
  let back = $state(0);

  // The Enlarge button focus came from was replaced along with the view, so
  // the dialog cannot hand focus back to it by itself.
  let holder = $state<HTMLDivElement | null>(null);
  async function closed() {
    expanded = false;
    await tick();
    holder?.querySelector<HTMLButtonElement>('[data-enlarge]')?.focus();
  }
</script>

<Panel title="History" aside={band?.history === 'private' ? operatorOnly : undefined}>
  {#await loadHistoryView()}
    <p role="status">Loading history…</p>
  {:then HistoryView}
    <div class="panel__holder" bind:this={holder}>
    {#if expanded}
      <dialog class="history-dialog" bind:this={dialog} aria-label="History of {band?.name ?? 'this band'}"
        onclose={closed}>
        <HistoryView expanded bind:hours bind:back onClose={() => dialog?.close()} />
      </dialog>
      <p class="history__note">Shown full size.</p>
    {:else}
      <HistoryView bind:hours bind:back onExpand={() => (expanded = true)} />
    {/if}
    </div>
  {:catch}
    <p role="alert">The history did not load. Reload the page to try again.</p>
  {/await}
</Panel>

{#snippet operatorOnly()}<span class="panel__meta">operator only</span>{/snippet}
