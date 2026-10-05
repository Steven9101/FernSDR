<script lang="ts">
  /**
   * The decodes as a list: newest at the top, filtered by channel, by CQ and
   * by a callsign or anything else in the message. A row tunes the receiver
   * to its channel, so what was decoded can be heard and seen on the
   * waterfall. Full size, a map of where the located stations are sits
   * beside it.
   */
  import { bands, controller, decoders, site } from '../../state/store';
  import { decodeFeed, distanceKm, filterDecodes, isCq, type Decode } from '../../state/decodes';
  import { locatorCentre } from '../../util/locator';
  import DecodesMap from './DecodesMap.svelte';

  interface Props {
    expanded?: boolean;
    onExpand?: () => void;
    onClose?: () => void;
    /** The filters, bound by the panel, so enlarging keeps them. */
    channel?: string;
    cqOnly?: boolean;
    search?: string;
  }

  let {
    expanded = false, onExpand, onClose,
    channel = $bindable(''), cqOnly = $bindable(false), search = $bindable(''),
  }: Props = $props();

  // The panel beside the waterfall shows the latest; the full list is for
  // the enlarged view, where there is room for it.
  const PANEL_ROWS = 150;

  $effect(() => decodeFeed.open());

  const channels = $derived(decoders.value.flatMap((decoder) => decoder.channels));
  const manyModes = $derived(new Set(channels.map((c) => c.mode)).size > 1);
  const status = $derived(decodeFeed.status.value);
  const all = $derived(decodeFeed.list.value);
  const shown = $derived(filterDecodes(all, { channel, cqOnly, search }));
  const rows = $derived((expanded ? shown : shown.slice(-PANEL_ROWS)).slice().reverse());
  const station = $derived(locatorCentre(site.value?.grid));

  function bandName(id: string): string {
    return bands.value.find((band) => band.id === id)?.name ?? id;
  }

  function channelLabel(c: { band: string; dial: number; mode: string }): string {
    const khz = c.dial / 1000;
    const dial = `${Number.isInteger(khz) ? khz : khz.toFixed(1)} kHz`;
    return `${bandName(c.band)} ${dial}${manyModes ? ` ${c.mode.toUpperCase()}` : ''}`;
  }

  const clock = (time: number) => new Date(time).toISOString().slice(11, 19);

  function distance(decode: Decode): string {
    const there = locatorCentre(decode.grid);
    if (!station || !there) return '';
    return `${Math.round(distanceKm(station, there)).toLocaleString()} km`;
  }

  function listen(decode: Decode) {
    const found = channels.find((c) => c.id === decode.channel);
    if (!controller.goTo(decode.dial, 'usb')) return;
    controller.setPassband(Math.max(100, found?.low ?? 100), found?.high ?? 3000);
  }
</script>

<div class="history decodes{expanded ? ' history--expanded' : ''}">
  <div class="decodes__filters">
    {#if channels.length > 1}
      <select class="tool__input decodes__channel" aria-label="Channel" bind:value={channel}>
        <option value="">All channels</option>
        {#each channels as c (c.id)}<option value={c.id}>{channelLabel(c)}</option>{/each}
      </select>
    {/if}
    <input class="tool__input" type="search" placeholder="Callsign or text" aria-label="Find in the messages"
      autocomplete="off" spellcheck="false" bind:value={search} />
    <button type="button" class="history__action{cqOnly ? ' is-pressed' : ''}" aria-pressed={cqOnly}
      title="Only stations calling CQ" onclick={() => (cqOnly = !cqOnly)}>CQ only</button>
  </div>
  <div class="history__navigation">
    <span class="history__note" role="status">
      {#if status === 'loading'}Reading decodes…
      {:else if status === 'error'}The decodes could not be read; trying again.
      {:else if all.length === 0}Nothing decoded yet. FT8 decodes arrive every 15 seconds.
      {:else}{shown.length.toLocaleString()} of {all.length.toLocaleString()} decodes, times in UTC{/if}
    </span>
    {#if onExpand}<button type="button" class="history__action" data-enlarge onclick={onExpand}>Enlarge</button>{/if}
    {#if onClose}<button type="button" class="history__action" onclick={onClose}>Close</button>{/if}
  </div>
  <div class="decodes__body">
    {#if expanded}
      <DecodesMap decodes={shown} {station} />
    {/if}
    {#if rows.length > 0 && !expanded}
      <!-- The side column is a third of a desktop and all of a phone: one decode to a row, the
           message first and whole, its time and audio frequency under it, so nothing is
           cut and nothing scrolls sideways. The enlarged view has room for the table. -->
      <ul class="decodes__scroll decodes__list">
        {#each rows as decode (decode.seq)}
          <li>
            <button type="button" class="decodes__row{isCq(decode.message) ? ' is-cq' : ''}"
              title="Listen to {channelLabel(decode)}" onclick={() => listen(decode)}>
              <span class="decodes__text">{decode.message}</span>
              <span class="decodes__snr">{decode.snr > 0 ? '+' : ''}{Math.round(decode.snr)} dB</span>
              <span class="decodes__meta">{clock(decode.time)} · {Math.round(decode.freq)} Hz</span>
            </button>
          </li>
        {/each}
      </ul>
    {:else if rows.length > 0}
      <div class="decodes__scroll">
        <table class="decodes__table">
          <thead>
            <tr>
              <th scope="col">UTC</th>
              {#if expanded && channels.length > 1}<th scope="col">Channel</th>{/if}
              <th scope="col" class="is-number">dB</th>
              {#if expanded}<th scope="col" class="is-number">DT</th>{/if}
              <th scope="col" class="is-number">Hz</th>
              <th scope="col">Message</th>
              {#if expanded && station}<th scope="col" class="is-number">Distance</th>{/if}
            </tr>
          </thead>
          <tbody>
            {#each rows as decode (decode.seq)}
              <tr class={isCq(decode.message) ? 'is-cq' : ''}>
                <td>{clock(decode.time)}</td>
                {#if expanded && channels.length > 1}
                  <td>{channelLabel(channels.find((c) => c.id === decode.channel) ?? decode)}</td>
                {/if}
                <td class="is-number">{decode.snr > 0 ? '+' : ''}{Math.round(decode.snr)}</td>
                {#if expanded}<td class="is-number">{decode.dt.toFixed(1)}</td>{/if}
                <td class="is-number">{Math.round(decode.freq)}</td>
                <td>
                  <button type="button" class="decodes__message" title="Listen to {channelLabel(decode)}"
                    onclick={() => listen(decode)}>{decode.message}</button>
                </td>
                {#if expanded && station}<td class="is-number">{distance(decode)}</td>{/if}
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {:else if all.length > 0}
      <p class="history__note">No decode matches.</p>
    {/if}
  </div>
</div>
