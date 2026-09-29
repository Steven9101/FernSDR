<script lang="ts">
  /**
   * UTC, the time the air runs on, with the listener's own beside it: logs,
   * schedules and nets are in UTC, and the question that follows is always
   * "and what is that here".
   */
  let now = $state(new Date());

  $effect(() => {
    const timer = window.setInterval(() => (now = new Date()), 1000);
    return () => window.clearInterval(timer);
  });

  const pad = (value: number) => String(value).padStart(2, '0');
  const offset = $derived(-now.getTimezoneOffset() / 60);
  const offsetText = $derived(
    offset === 0 ? 'UTC' : `UTC${offset > 0 ? '+' : '−'}${Math.abs(offset) % 1 ? Math.abs(offset).toFixed(1) : Math.abs(offset)}`,
  );
  const date = $derived(
    now.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC' }),
  );
</script>

<div class="clock">
  <div class="clock__main">
    <span class="clock__time">{pad(now.getUTCHours())}:{pad(now.getUTCMinutes())}<span class="clock__seconds">:{pad(now.getUTCSeconds())}</span></span>
    <span class="clock__zone">UTC · {date}</span>
  </div>
  {#if offset !== 0}
    <div class="clock__local">
      <span class="clock__local-time">{pad(now.getHours())}:{pad(now.getMinutes())}</span>
      <span class="clock__zone">Your time, {offsetText}</span>
    </div>
  {/if}
</div>
