<script lang="ts">
  /**
   * Browsers refuse to start audio without a user gesture, so there has to be
   * something to press. Rather than hide that behind a small icon, this is a
   * clear panel over the display: a receiver that looks like it is working and
   * is silent is the single most common way a web SDR wastes someone's time.
   */
  import { audioState, controller, muted, volume } from '../state/store';

  function start(): void {
    if (muted.value) controller.setMuted(false);
    if (volume.value === 0) controller.setVolume(0.8);
    void controller.startAudio();
  }

  const state = $derived(audioState.value);
  const failed = $derived(state === 'failed');
  const waiting = $derived(state !== 'running');

  // The panel says "Browsers need one tap", so any tap should count: tuning
  // the waterfall, opening the sheet, pressing a mode.
  //
  // Two rules, both learned the hard way. Only a trusted event counts: a
  // synthetic pointerdown during load used to burn the one-shot listener and
  // open the context outside a real gesture, which is exactly the state a
  // browser will not resume. And the listener re-arms on every attempt rather
  // than firing once, so a refused tap does not leave the page inert.
  $effect(() => {
    if (!waiting || failed) return;
    const onGesture = (event: Event) => {
      if (!event.isTrusted) return;
      start();
    };
    const opts = { capture: true } as const;
    document.addEventListener('pointerdown', onGesture, opts);
    document.addEventListener('keydown', onGesture, opts);
    return () => {
      document.removeEventListener('pointerdown', onGesture, opts);
      document.removeEventListener('keydown', onGesture, opts);
    };
  });
</script>

{#if state !== 'running'}
  <!-- Anchored to the bottom of the display rather than covering it: the copy
       says the waterfall is already live, so hiding the waterfall behind this
       would be both rude and a lie. -->
  <div class="audio-gate" role="status" aria-label="Start audio">
    <button type="button" class="audio-gate__card" onclick={start}>
      <div class="audio-gate__icon" aria-hidden="true">
        <svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.8"
             stroke-linecap="round" stroke-linejoin="round">
          <path d="M11 5 6 9H3v6h3l5 4z" />
          <path d="M16 9a4 4 0 0 1 0 6" />
          <path d="M19 6a8 8 0 0 1 0 12" />
        </svg>
      </div>
      <div class="audio-gate__body">
        <h2 class="audio-gate__title">{failed ? 'Audio could not start' : 'Tap to listen'}</h2>
        <p class="audio-gate__text">
          {failed
            ? 'Your browser would not open an audio device. Check that this page may play sound.'
            : state === 'suspended'
              ? 'The browser accepted the tap but did not open the audio device. Check the silent switch and that this tab is not muted.'
              : 'Browsers need one tap before they will play sound.'}
        </p>
      </div>
      <span class="button button--primary audio-gate__action" aria-hidden="true">
        {failed ? 'Try again' : 'Start audio'}
      </span>
    </button>
  </div>
{/if}
