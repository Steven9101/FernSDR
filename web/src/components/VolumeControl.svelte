<script lang="ts">
  /**
   * Volume and mute. Always visible, never behind a menu: it is the control
   * people reach for fastest and often in a hurry.
   */
  import { audioState, controller, muted, volume } from '../state/store';

  const isMuted = $derived(muted.value);
  const level = $derived(volume.value);
  const running = $derived(audioState.value === 'running');
</script>

<div class="volume">
  <button
    type="button"
    class="icon-button{isMuted ? ' is-active' : ''}"
    aria-label={isMuted ? 'Unmute' : 'Mute'}
    aria-pressed={isMuted}
    onclick={() => {
      if (!running) {
        void controller.startAudio();
        return;
      }
      controller.setMuted(!isMuted);
    }}
  >
    <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8"
         stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M11 5 6 9H3v6h3l5 4z" />
      {#if isMuted}
        <path d="m17 9 4 6" />
        <path d="m21 9-4 6" />
      {:else}
        <path d="M16 9a4 4 0 0 1 0 6" />
        <path d="M19 6a8 8 0 0 1 0 12" />
      {/if}
    </svg>
  </button>
  <input
    class="slider slider--compact"
    type="range"
    min="0"
    max="100"
    value={Math.round(level * 100)}
    aria-label="Volume"
    oninput={(event) => {
      const next = Number(event.currentTarget.value) / 100;
      controller.setVolume(next);
      if (next > 0 && isMuted) controller.setMuted(false);
    }}
  />
</div>
