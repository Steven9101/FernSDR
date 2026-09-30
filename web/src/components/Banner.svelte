<script lang="ts">
  import { bands, controller, currentBand, errorMessage, inactiveReason, inactivityDeadline, notice, removed } from '../state/store';
  import { endingNotice } from '../util/band-hours';

  const error = $derived(errorMessage.value);
  const message = $derived(notice.value);
  const deadline = $derived(inactivityDeadline.value);
  const inactive = $derived(inactiveReason.value);
  let element = $state<HTMLDivElement | null>(null);

  // The listener timeout counts down in whole seconds while it waits for an
  // answer. The receiver keeps the real clock; this only shows it.
  let now = $state(performance.now());
  $effect(() => {
    if (deadline === null) return;
    now = performance.now();
    const timer = window.setInterval(() => (now = performance.now()), 1000);
    return () => window.clearInterval(timer);
  });
  const secondsLeft = $derived(deadline === null ? 0 : Math.max(0, Math.ceil((deadline - now) / 1000)));

  // The band's hours ending: said in their last ten minutes, and while the
  // listener stays on it off the air; on a clock of the page's own, looked
  // at twice a minute and only on a band with hours.
  const band = $derived(currentBand.value);
  let wallNow = $state(Date.now());
  $effect(() => {
    if (!band?.next_change || band.next_change < 0) return;
    wallNow = Date.now();
    const timer = window.setInterval(() => (wallNow = Date.now()), 30_000);
    return () => window.clearInterval(timer);
  });
  const ending = $derived(endingNotice(band ?? undefined, bands.value, wallNow));

  // The banner is a row of its own, and on a small phone every row counts:
  // the sheet sizes itself to what is left beside a minimum-height stage, so
  // it has to know the banner is there. Without this a notice pushed the
  // status row off a 320x568 screen and let the page scroll.
  $effect(() => {
    const root = document.documentElement;
    const node = element;
    if (!node) {
      root.style.setProperty('--banner-height', '0px');
      return;
    }
    const measure = () => root.style.setProperty('--banner-height', `${node.getBoundingClientRect().height}px`);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    return () => {
      observer.disconnect();
      root.style.setProperty('--banner-height', '0px');
    };
  });
</script>

{#if removed.value}
  <div bind:this={element} class="banner" role="alert">
    <span>The operator of this receiver disconnected you.</span>
  </div>
{:else if inactive}
  <div bind:this={element} class="banner banner--action" role="alert">
    <span>This receiver let your place go after {inactive.replace(/^no activity for /, '')} without activity.</span>
    <button type="button" class="button button--small" onclick={() => controller.listenAgain()}>Listen again</button>
  </div>
{:else if deadline !== null}
  <div bind:this={element} class="banner banner--action" role="alert">
    <span>Still listening? Without an answer this receiver lets your place go in {secondsLeft} s.</span>
    <button type="button" class="button button--small" onclick={() => controller.stillListening()}>I'm listening</button>
  </div>
{:else if error || message}
  <div bind:this={element} class="banner{error ? ' banner--error' : ''}" role="status">
    {error || message}
  </div>
{:else if ending}
  <div bind:this={element} class="banner" role="status">{ending}</div>
{/if}
