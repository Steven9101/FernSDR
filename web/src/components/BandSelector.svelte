<script lang="ts">
  /**
   * Band picker, in two shapes for two places.
   *
   * `row` lives in the top bar on a phone, scrolling horizontally rather than
   * wrapping so the header keeps a predictable height and the display never
   * jumps. `list` lives at the foot of the sidebar on a wide screen, where the
   * bands are the receiver's primary navigation and deserve to be readable at a
   * glance rather than squeezed into a strip - and where the column had empty space
   * doing nothing.
   */
  import { bands, controller, tuning } from '../state/store';
  import Tooltip from './Tooltip.svelte';
  import { formatSpan } from '../util/frequency';
  import { bandRank, receiverLabel } from '../util/receivers';
  import { comesOnAt, comesOnShort, offAir } from '../util/band-hours';

  let { variant = 'row' }: { variant?: 'row' | 'list' } = $props();

  const list = $derived(bands.value);
  const active = $derived(tuning.value.band);

  // A search once the list is longer than a glance takes in: by name, by
  // what the band listens with, or by a frequency inside it.
  const SEARCH_FROM = 10;
  let query = $state('');
  const shown = $derived(list.length < SEARCH_FROM || !query.trim() ? list : list
    .map((band) => ({ band, rank: bandRank(query, band) }))
    .filter((entry): entry is { band: (typeof list)[number]; rank: number } => entry.rank !== null)
    .sort((a, b) => a.rank - b.rank)
    .map((entry) => entry.band));
  const mhz = (hz: number) => (hz / 1e6).toFixed(3);
  // Off the air by its hours is not down: the band waits for its time, and
  // choosing it would only land on the band that has its input now.
  const condition = (band: (typeof list)[number]) => (offAir(band) ? ' is-scheduled' : band.running ? '' : ' is-down');
  const choose = (band: (typeof list)[number]) => {
    if (!offAir(band)) controller.selectBand(band.id);
  };
</script>

{#if list.length > 1}
  {#if variant === 'list'}
    <nav class="band-list" aria-labelledby="band-list-title">
      <h3 class="band-list__title" id="band-list-title">Bands</h3>
      {#if list.length >= SEARCH_FROM}
        <input class="band-list__search" type="search" bind:value={query} placeholder="Name or MHz"
          aria-label="Find a band by name or frequency" autocomplete="off" />
      {/if}
      <div class="band-list__rows">
      {#each shown as band (band.id)}
        <button
          type="button"
          class="band-list__item{band.id === active ? ' is-active' : ''}{condition(band)}"
          aria-current={band.id === active ? 'true' : undefined}
          aria-disabled={offAir(band) ? 'true' : undefined}
          aria-label={offAir(band) ? `${band.name}, ${comesOnAt(band)}` : undefined}
          onclick={() => choose(band)}
        >
          <span class="band-list__name">
            <span class="band-list__label">{band.name}</span>
            {#if receiverLabel(band.receiver)}<span class="band-list__kind">{receiverLabel(band.receiver)}</span>{/if}
          </span>
          <span class="band-list__range">{mhz(band.low)}–{mhz(band.high)} <span class="band-list__unit">MHz</span></span>
          <span class="band-list__listeners">{offAir(band) ? comesOnShort(band) : band.running ? band.listeners || '' : 'down'}</span>
        </button>
      {:else}
        <p class="band-list__empty">No band covers “{query}”.</p>
      {/each}
      </div>
    </nav>
  {:else}
    <nav class="bands" aria-label="Receiver bands">
      {#each list as band}
        <Tooltip placement="bottom">
          {#snippet label()}
            <strong>{band.name}</strong>{#if receiverLabel(band.receiver)}, {receiverLabel(band.receiver)}{/if}<br />{mhz(band.low)}–{mhz(band.high)} MHz, {formatSpan(band.high - band.low)} wide<br />{offAir(band) ? comesOnAt(band) : band.running ? `${band.listeners} listening` : 'Not running'}
          {/snippet}
          {#snippet children(anchor)}
            <button
              {@attach anchor}
              type="button"
              class="bands__item{band.id === active ? ' is-active' : ''}{condition(band)}"
              aria-current={band.id === active ? 'true' : undefined}
              aria-disabled={offAir(band) ? 'true' : undefined}
              onclick={() => choose(band)}
            >
              <span class="bands__name">{band.name}</span>
              {#if band.listeners > 0}<span class="bands__listeners">{band.listeners}</span>{/if}
            </button>
          {/snippet}
        </Tooltip>
      {/each}
    </nav>
  {/if}
{/if}
