<script lang="ts">
  /**
   * The shortcut list. Worth having visible somewhere: the keyboard is by far
   * the fastest way to work a receiver, and shortcuts nobody can discover might
   * as well not exist.
   */
  let { onClose }: { onClose: () => void } = $props();

  const SHORTCUTS: { keys: string; description: string }[] = [
    { keys: '← →', description: 'Tune down / up (hold Shift for coarse, Ctrl for 1 kHz; on WFM 50 kHz, 100 kHz, 1 MHz)' },
    { keys: '↑ ↓', description: 'Volume' },
    { keys: 'M', description: 'Mute' },
    { keys: 'Space', description: 'Start audio, or mute once it is running' },
    { keys: 'Z / X', description: 'Zoom out / in' },
    { keys: '[ ]', description: 'Narrow / widen the filter' },
    { keys: '1 to 9', description: 'Mode: USB, LSB, CW, CW-L, AM, SAM, NFM, DSB, and WFM where the band has it' },
    { keys: 'F', description: 'Type a frequency' },
    { keys: 'N', description: 'Toggle noise reduction' },
    { keys: 'L', description: 'Arrange the page: hide what you do not use, set the spectrum height' },
    { keys: '?', description: 'This list' },
  ];
</script>

<!-- A click on the backdrop is a mouse shortcut beside the Close button and
     Escape, and the card only keeps that click from reaching the backdrop.
     The dialog does not take focus; moving focus into it would change how
     the page behaves, which is a change of its own. -->
<!-- svelte-ignore a11y_interactive_supports_focus, a11y_click_events_have_key_events -->
<div class="modal" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts" onclick={onClose}>
  <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
  <div class="modal__card" onclick={(event) => event.stopPropagation()}>
    <header class="modal__header">
      <h2 class="modal__title">Keyboard shortcuts</h2>
      <button type="button" class="icon-button" aria-label="Close" onclick={onClose}>
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor"
             stroke-width="1.8" stroke-linecap="round" aria-hidden="true">
          <path d="M6 6l12 12M18 6L6 18" />
        </svg>
      </button>
    </header>
    <dl class="shortcuts">
      {#each SHORTCUTS as shortcut}
        <div class="shortcuts__row">
          <dt>
            <kbd>{shortcut.keys}</kbd>
          </dt>
          <dd>{shortcut.description}</dd>
        </div>
      {/each}
    </dl>
  </div>
</div>
