<script lang="ts">
  /**
   * A small panel anchored to the button that opened it: the tools under the
   * dial open these rather than taking a tab or a row of their own.
   *
   * Moved to the end of the page while open, so it takes the page's colours
   * and not the HUD's, and nothing on the stage clips it. Escape, a click
   * outside and the button itself close it; focus goes in when it opens and
   * back to the button when it closes, as a menu's does.
   */
  import type { Snippet } from 'svelte';
  import { autoUpdate, computePosition, flip, offset, shift } from '@floating-ui/dom';

  interface Props {
    /** The panel's name, for screen readers. */
    label: string;
    open: boolean;
    anchor: HTMLElement | null;
    onClose: () => void;
    children: Snippet;
  }

  let { label, open, anchor, onClose, children }: Props = $props();
  let panel = $state<HTMLDivElement | null>(null);

  function portal(node: HTMLElement) {
    document.body.appendChild(node);
    return { destroy: () => node.remove() };
  }

  $effect(() => {
    const element = panel;
    const button = anchor;
    if (!open || !element || !button) return;
    const stop = autoUpdate(button, element, () => {
      computePosition(button, element, {
        placement: 'top-start',
        strategy: 'fixed',
        middleware: [offset(8), flip({ fallbackPlacements: ['bottom-start', 'top-end'] }), shift({ padding: 8 })],
      }).then(({ x, y }) => {
        element.style.left = `${x}px`;
        element.style.top = `${y}px`;
      });
    });
    // A tool's panel is loaded on first use, so on that first opening it is
    // not there yet when the popover is: wait for it rather than leave focus
    // on the button, where a callsign typed straight away would go nowhere.
    const focusFirst = () => {
      const first = element.querySelector<HTMLElement>('input, button, [tabindex]:not([tabindex="-1"])');
      first?.focus();
      return !!first;
    };
    let waiting: MutationObserver | null = null;
    if (!focusFirst()) {
      waiting = new MutationObserver(() => {
        if (focusFirst()) waiting?.disconnect();
      });
      waiting.observe(element, { childList: true, subtree: true });
    }
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.stopPropagation();
        onClose();
      }
    };
    const onPointer = (event: PointerEvent) => {
      const target = event.target as Node;
      if (!element.contains(target) && !button.contains(target)) onClose();
    };
    document.addEventListener('keydown', onKey, true);
    document.addEventListener('pointerdown', onPointer, true);
    return () => {
      stop();
      waiting?.disconnect();
      document.removeEventListener('keydown', onKey, true);
      document.removeEventListener('pointerdown', onPointer, true);
      // Back to the button, unless the focus has already gone somewhere else
      // the listener chose.
      if (!document.activeElement || document.activeElement === document.body || element.contains(document.activeElement)) {
        button.focus();
      }
    };
  });
</script>

{#if open}
  <div bind:this={panel} use:portal class="popover" role="dialog" aria-label={label}>
    {@render children()}
  </div>
{/if}
