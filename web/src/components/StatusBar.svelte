<script lang="ts" module>
  import { lazy } from './lazy';

  const loadAbout = lazy(() => import('./AboutDialog.svelte'));
</script>

<script lang="ts">
  /**
   * The bottom line: what the receiver is, whether it is connected, and what
   * the stream is costing. Small, but it is the difference between "it's
   * broken" and "my connection dropped and it's coming back".
   */
  import {
    audioLatencyMs,
    connectionDetail,
    connectionState,
    meter,
    site,
    streamTraffic,
  } from '../state/store';
  import { formatBitrate } from '../util/frequency';

  const statusLabel = $derived.by(() => {
    switch (connectionState.value) {
      case 'open': return 'Connected';
      case 'connecting': return 'Connecting';
      case 'reconnecting': return 'Reconnecting';
      case 'closed': return 'Disconnected';
    }
  });

  const stats = $derived(meter.value);
  const connection = $derived(connectionState.value);
  const information = $derived(site.value);
  let about = $state(false);
</script>

<footer class="status">
  <span class="status__state">{statusLabel}{#if connection === 'reconnecting' && connectionDetail.value}<span class="status__detail">{' '}&middot; {connectionDetail.value}</span>{/if}</span>

  {#if information}
    <span class="status__site">{#if information.location}<span>{information.location}</span>{/if}{#if information.antenna}<span class="status__antenna" title="Antenna">{information.antenna}</span>{/if}</span>
  {/if}

  <span class="status__spacer"></span>

  {#if stats}
    <span class="status__stat" title="Received stream including status and WebSocket framing">
      {streamTraffic.value ? formatBitrate(streamTraffic.value.totalBps) : '--'}
    </span>
    <span class="status__stat" title="Audio buffered ahead of the play head">
      {audioLatencyMs.value} ms
    </span>
    <span class="status__stat" title="People listening to this receiver">
      {stats.listeners} listening
    </span>
  {/if}
  <button type="button" class="status__about" onclick={() => (about = true)}>FernSDR</button>
</footer>

{#if about}
  {#await loadAbout() then AboutDialog}
    <AboutDialog onClose={() => (about = false)} />
  {/await}
{/if}
