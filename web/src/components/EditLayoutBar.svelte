<script lang="ts">
  /**
   * The edit mode's own controls, over the top of the page while the
   * listener arranges it: the meter's face and the side of the controls,
   * which have no place of their own to put a button, going back to the
   * station's layout, and Done. Hiding parts is done on the parts
   * themselves (Editable.svelte), and the spectrum's height on the line under
   * it. Loaded the first time the edit mode opens.
   */
  import { onMount } from 'svelte';
  import { MediaQuery } from 'svelte/reactivity';
  import Segmented from './Segmented.svelte';
  import { editingLayout, isOwnLayout, layout, meterFace, resetLayout, setLayout } from '../state/layout';
  import { operatorTheme } from '../state/store';


  const METERS = [
    { value: 'bar', label: 'Bar', title: 'A bar with a peak marker' },
    { value: 'needle', label: 'Needle', title: 'An analog meter' },
    { value: 'numeric', label: 'Numbers', title: 'The reading and its peak as figures' },
    { value: 'history', label: 'Trace', title: 'The last half minute, to see a fade coming' },
  ] as const;
  const SIDES = [
    { value: 'left', label: 'Left' },
    { value: 'right', label: 'Right' },
  ] as const;
  // The controls only have a side on a wide screen; the phone's sheet has none.
  const wide = new MediaQuery('(min-width: 1024px)');

  const own = $derived(layout.value);
  let done: HTMLButtonElement;

  function finish() {
    editingLayout.value = false;
  }

  function reset() {
    resetLayout();
    // The button just pressed is disabled now, and would drop the focus.
    done.focus();
  }

  onMount(() => {
    // Wherever the listener was when the mode opened, the Customise button or
    // the waterfall, is where they are when it closes, however it closes:
    // Done, Escape, or L again, which the page's own handler takes.
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    done.focus();
    const onKey = (event: KeyboardEvent) => {
      // L closes it from the bar's own buttons too, which the page's handler
      // leaves alone as it does every button.
      const plain = !event.ctrlKey && !event.metaKey && !event.altKey;
      if ((event.key === 'l' || event.key === 'L') && plain && event.target instanceof Element &&
          event.target.closest('.layout-bar')) {
        event.preventDefault();
        finish();
        return;
      }
      // A popover or dialog open over the page closes first, on its own Escape.
      if (event.key !== 'Escape' || event.defaultPrevented) return;
      if (event.target instanceof Element && event.target.closest('[role="dialog"]')) return;
      event.preventDefault();
      finish();
    };
    window.addEventListener('keydown', onKey);
    return () => {
      window.removeEventListener('keydown', onKey);
      requestAnimationFrame(() => {
        const back = opener?.isConnected && opener !== document.body ? opener : document.getElementById('customise-layout');
        back?.focus();
      });
    };
  });
</script>

<div class="layout-bar" role="toolbar" aria-label="Arrange the page">
  <p class="layout-bar__title" title="Hide what you do not use, and drag the line under the spectrum.">Arrange the page</p>
  <div class="layout-bar__choices">
    {#if own.show.meter}
      <div class="layout-bar__meter">
        <Segmented label="Meter" options={METERS} value={meterFace(own.meter, operatorTheme.value?.meter)}
          onChange={(meter) => setLayout({ meter })} />
      </div>
    {/if}
    {#if wide.current}
      <div class="layout-bar__side">
        <Segmented label="Side of the controls" options={SIDES} value={own.side} onChange={(side) => setLayout({ side })} />
      </div>
    {/if}
  </div>
  <div class="layout-bar__actions">
    <button type="button" class="button button--small" disabled={!isOwnLayout(own)} onclick={reset}>
      Station's layout
    </button>
    <button type="button" class="button button--small button--primary" bind:this={done} onclick={finish}>Done</button>
  </div>
  <p class="sr-only" role="status">Arranging the page. Press Escape or Done when finished.</p>
</div>

<style>
  /* Over the title bar, which holds nothing to arrange: one row, so it
     covers nothing that is. */
  .layout-bar {
    position: fixed;
    top: calc(env(safe-area-inset-top, 0px) + 6px);
    left: 50%;
    transform: translateX(-50%);
    z-index: 60;
    display: flex;
    align-items: center;
    gap: var(--space-3);
    width: max-content;
    max-width: calc(100vw - 2 * var(--space-3));
    padding: 4px 4px 4px var(--space-4);
    border-radius: var(--radius-pill);
    background: var(--popover);
    color: var(--foreground);
    box-shadow: var(--shadow-popover);
    border: 1px solid var(--border);
  }

  .layout-bar__title {
    font-size: var(--text-sm);
    font-weight: 600;
    white-space: nowrap;
  }

  .layout-bar__choices,
  .layout-bar__actions {
    display: flex;
    align-items: center;
    gap: var(--space-2);
  }

  .layout-bar__meter {
    min-width: 19rem;
  }

  .layout-bar__side {
    min-width: 8.5rem;
  }

  .button:disabled {
    opacity: 0.45;
    cursor: default;
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
  }

  /* A phone has no room for a row: the choices go under the title, and the
     bar is a card across the top. */
  @media (max-width: 760px) {
    .layout-bar {
      left: var(--space-3);
      right: var(--space-3);
      transform: none;
      width: auto;
      flex-wrap: wrap;
      padding: var(--space-3);
      border-radius: var(--radius-xl);
    }

    .layout-bar__actions {
      margin-left: auto;
    }

    .layout-bar__choices {
      order: 3;
      flex-basis: 100%;
      flex-wrap: wrap;
    }

    .layout-bar__meter {
      min-width: 0;
      flex: 1;
    }
  }
</style>
