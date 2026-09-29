<script lang="ts">
  /**
   * What this page is and whose work is in it: FernSDR and its licence, the
   * AGPL section 13 offer of the source this receiver runs, the software from
   * others that ships in the page, and where the data it draws comes from.
   *
   * Opened from the status line, which a theme cannot hide: a theme only sets
   * colours and pictures (state/theme.ts), so an operator can brand the page
   * but not take the credits off it.
   */
  import { bandPlan } from '../state/bandplan';
  import { decoders, operatorTheme, site } from '../state/store';

  let { onClose }: { onClose: () => void } = $props();

  let dialog = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    if (dialog && !dialog.open) dialog.showModal();
  });

  const information = $derived(site.value);
  const sourceUrl = $derived(information?.source_url || 'https://github.com/Steven9101/FernSDR');
  const planSources = $derived(bandPlan.value?.sources.filter((source) => source.url) ?? []);
  const widgets = $derived(new Set((operatorTheme.value?.widgets ?? []).map((widget) => widget.type)));
  const hasMap = $derived(widgets.has('greyline') || decoders.value.length > 0);
</script>

<dialog class="about" bind:this={dialog} aria-labelledby="about-title" onclose={onClose}>
  <header class="modal__header">
    <h2 class="modal__title" id="about-title">About FernSDR</h2>
    <button type="button" class="icon-button" aria-label="Close" onclick={() => dialog?.close()}>
      <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor"
           stroke-width="1.8" stroke-linecap="round" aria-hidden="true">
        <path d="M6 6l12 12M18 6L6 18" />
      </svg>
    </button>
  </header>

  <p class="about__lead">
    {#if information?.name}{information.name} runs{:else}This receiver runs{/if} FernSDR, free software under the
    <a href="https://www.gnu.org/licenses/agpl-3.0.html" target="_blank" rel="noreferrer noopener">GNU Affero General Public License, version 3</a>.
    You may use, study, share and change it. Whoever runs a changed version for others must offer them its source.
  </p>

  <ul class="about__links">
    <li><a href={sourceUrl} target="_blank" rel="noreferrer noopener">Source code of this receiver</a></li>
    <li><a href="/licenses.txt" target="_blank" rel="noreferrer noopener">Software by others in this page, with its licences</a></li>
  </ul>

  {#if hasMap || widgets.has('space') || planSources.length > 0 || decoders.value.length > 0}
  <h3 class="about__heading">Data</h3>
  <ul class="about__credits">
    {#if hasMap}<li>World map: Natural Earth, public domain.</li>{/if}
    {#if widgets.has('space')}<li>Space weather: NOAA Space Weather Prediction Center.</li>{/if}
    {#if planSources.length > 0}
      <li>
        <details class="about__sources">
          <summary>Band plan: {planSources.length} {planSources.length === 1 ? 'source' : 'sources'}</summary>
          <ul>
            {#each planSources as source (source.url)}
              <li><a href={source.url} target="_blank" rel="noreferrer noopener">{source.title}</a></li>
            {/each}
          </ul>
        </details>
      </li>
    {/if}
    {#if decoders.value.length > 0}
      <li>Decodes come from decoder modules this receiver runs; they are published as received and may contain errors.</li>
    {/if}
  </ul>
  {/if}

  {#if information?.operator}
    <p class="about__note">The station, its antenna and what it receives are the responsibility of its operator, {information.operator}.</p>
  {/if}
</dialog>
