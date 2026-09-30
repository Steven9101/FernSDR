<script lang="ts">
  /**
   * The shortcut list. Worth having visible somewhere: the keyboard is by far
   * the fastest way to work a receiver, and shortcuts nobody can discover might
   * as well not exist.
   */
  import { onMount } from 'svelte';

  let { onClose }: { onClose: () => void } = $props();
  let closeButton = $state<HTMLButtonElement | null>(null);

  // A modal dialog for keyboard users as well: focus goes in, stays in (the
  // Close button is the only control, so Tab keeps it there), Escape closes it
  // wherever focus is, and focus goes back to what opened it. Escape is taken
  // here, before the page's own shortcuts, which leave keys aimed at a button
  // alone and so never saw it while the opener or Close had focus.
  onMount(() => {
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    closeButton?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        onClose();
      } else if (event.key === 'Tab') {
        event.preventDefault();
        closeButton?.focus();
      }
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => {
      window.removeEventListener('keydown', onKeyDown, true);
      if (opener?.isConnected) opener.focus();
    };
  });

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
     Escape, and the card only keeps that click from reaching the backdrop. -->
<!-- svelte-ignore a11y_interactive_supports_focus, a11y_click_events_have_key_events -->
<div class="modal" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts" onclick={onClose}>
  <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
  <div class="modal__card" onclick={(event) => event.stopPropagation()}>
    <header class="modal__header">
      <h2 class="modal__title">Keyboard shortcuts</h2>
      <button bind:this={closeButton} type="button" class="icon-button" aria-label="Close" onclick={onClose}>
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
