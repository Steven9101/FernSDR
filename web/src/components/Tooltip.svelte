<script lang="ts">
  /**
   * Tooltips.
   *
   * The `title` attribute was doing this job and doing it badly: the browser
   * decides when it appears (about a second, unconfigurably), it cannot be
   * styled, it never appears on a touch screen, and on a phone a long press gets
   * the text-selection menu instead. So this is a real one, positioned by
   * Floating UI so it flips and shifts rather than sliding off the edge of a
   * phone.
   *
   * The delay rules are Emil Kowalski's, and they are the whole reason this is
   * worth the code:
   *
   *   - a delay before the first tooltip, so brushing past a row of controls
   *     does not fire six of them
   *   - NO delay and NO animation while a group is already warm, because once a
   *     person is reading tooltips, waiting again for each one feels broken
   *   - the warmth expires shortly after the pointer leaves
   *
   * On a touch screen there is no hover, so the affordance is a long press:
   * hold a control for half a second and its tooltip appears, exactly as it does
   * on Android's own toolbars. The press is cancelled by any movement, so a
   * long press that turns into a drag - which is how the waterfall is panned and
   * how the sheet is opened - never leaves a tooltip behind. Tapping anywhere
   * dismisses it.
   *
   * The control it describes is rendered by the caller, which applies the
   * attachment it is handed. No wrapper element: the segmented controls measure
   * their buttons' offsets, and a wrapper would move them.
   */
  import type { Snippet } from 'svelte';
  import type { Attachment } from 'svelte/attachments';
  import { autoUpdate, computePosition, flip, offset, shift } from '@floating-ui/dom';
  import { tooltipTiming } from './tooltip-timing';

  /** Long enough not to fire on a tap, short enough to feel deliberate. */
  const LONG_PRESS_MS = 500;

  interface Props {
    label: string | Snippet;
    placement?: 'top' | 'bottom' | 'left' | 'right';
    children: Snippet<[Attachment<HTMLElement>]>;
  }

  let { label, placement = 'top', children }: Props = $props();

  let open = $state(false);
  let cold = $state(true);
  let anchor = $state.raw<HTMLElement | null>(null);
  let floating = $state.raw<HTMLDivElement | null>(null);
  let timer: number | undefined;
  let press: number | undefined;
  let pressOrigin: { x: number; y: number } | null = null;
  // Whether the tooltip opened on hover or a long press, which is what warms
  // the group. Focus opens it without setting this.
  let shown = false;

  // The rule lives in tooltip-timing.ts, where it can be tested without a
  // browser. This is only the wiring.
  function show() {
    window.clearTimeout(timer);
    const { delayMs, cold: isCold } = tooltipTiming.open(Date.now());
    cold = isCold;
    timer = window.setTimeout(() => {
      shown = true;
      open = true;
    }, delayMs);
  }

  function hide() {
    window.clearTimeout(timer);
    tooltipTiming.close(Date.now(), shown);
    shown = false;
    open = false;
  }

  function cancelPress() {
    window.clearTimeout(press);
    pressOrigin = null;
  }

  const attach: Attachment<HTMLElement> = (element) => {
    anchor = element;
    const listeners = {
      pointerenter: (event: PointerEvent) => {
        // Touch reports itself as a pointer enter immediately before the tap;
        // opening a tooltip there would cover the thing being tapped. The long
        // press below is the touch affordance instead.
        if (event.pointerType !== 'touch') show();
      },
      pointerdown: (event: PointerEvent) => {
        if (event.pointerType !== 'touch') return;
        pressOrigin = { x: event.clientX, y: event.clientY };
        window.clearTimeout(press);
        press = window.setTimeout(() => {
          shown = true;
          cold = false;
          open = true;
        }, LONG_PRESS_MS);
      },
      pointermove: (event: PointerEvent) => {
        // Any real movement means this is a drag, not a press. Without this a
        // long press that becomes a pan leaves a tooltip stranded over the
        // waterfall.
        const origin = pressOrigin;
        if (origin && Math.hypot(event.clientX - origin.x, event.clientY - origin.y) > 8) cancelPress();
      },
      pointerup: cancelPress,
      pointercancel: () => {
        cancelPress();
        hide();
      },
      pointerleave: () => {
        cancelPress();
        hide();
      },
      focus: () => {
        // Keyboard focus only: a tap focuses the button too, and a tooltip
        // opened by that covered what had just been tapped. The browser's
        // :focus-visible is exactly keyboard focus on a button.
        if (element.matches(':focus-visible')) open = true;
      },
      blur: hide,
    };
    for (const [type, listener] of Object.entries(listeners)) element.addEventListener(type, listener as EventListener);
    return () => {
      for (const [type, listener] of Object.entries(listeners)) element.removeEventListener(type, listener as EventListener);
      if (anchor === element) anchor = null;
    };
  };

  $effect(() => () => {
    window.clearTimeout(timer);
    window.clearTimeout(press);
  });

  // A tooltip opened by a long press has no pointerleave to close it, so it
  // closes on the next touch anywhere.
  $effect(() => {
    if (!open) return;
    const dismiss = (event: PointerEvent) => {
      if (event.pointerType === 'touch') hide();
    };
    window.addEventListener('pointerdown', dismiss, { capture: true });
    return () => window.removeEventListener('pointerdown', dismiss, { capture: true });
  });

  $effect(() => {
    const reference = anchor;
    const tooltip = floating;
    const side = placement;
    if (!open || !reference || !tooltip) return;

    // autoUpdate keeps it attached while the page scrolls or the sheet is
    // dragged; without it a tooltip over a moving panel is left behind.
    return autoUpdate(reference, tooltip, () => {
      void computePosition(reference, tooltip, {
        placement: side,
        middleware: [offset(6), flip({ padding: 8 }), shift({ padding: 8 })],
      }).then(({ x, y, placement: resolved }) => {
        // Set transform directly rather than through a custom property: a
        // variable on a parent forces a style recalculation for every child.
        tooltip.style.transform = `translate(${Math.round(x)}px, ${Math.round(y)}px)`;
        // Grow out of the thing being described, not out of thin air. flip()
        // may have put the tooltip on the opposite side, so the origin comes
        // from where it actually ended up.
        const resolvedSide = resolved.split('-')[0];
        tooltip.style.setProperty(
          '--tooltip-origin',
          resolvedSide === 'top' ? 'bottom center'
            : resolvedSide === 'bottom' ? 'top center'
              : resolvedSide === 'left' ? 'right center'
                : 'left center',
        );
      });
    });
  });
</script>

{@render children(attach)}
{#if open}
  <div bind:this={floating} class="tooltip" role="tooltip" data-cold={String(cold)}>
    {#if typeof label === 'string'}{label}{:else}{@render label()}{/if}
  </div>
{/if}
