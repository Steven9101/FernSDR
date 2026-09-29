/**
 * When a tooltip should wait, and when it should simply be there.
 *
 * Kept as a pure module so the rule can be tested without a browser. The rule
 * itself is Emil Kowalski's: a delay before the first tooltip, so brushing
 * past a row of controls does not fire six of them; then no delay and no
 * entrance for as long as the group stays warm, because once somebody is
 * reading tooltips, making each one fade in again is making them wait twice.
 */

export const OPEN_DELAY_MS = 400;
/** How long the group stays warm after a tooltip closes. */
export const WARM_MS = 300;

export interface TooltipTiming {
  /** Milliseconds to wait before showing, and whether to play an entrance. */
  open(now: number): { delayMs: number; cold: boolean };
  /** A tooltip that was actually on screen has closed. */
  close(now: number, wasOpen: boolean): void;
}

export function createTooltipTiming(): TooltipTiming {
  let warmUntil = 0;
  return {
    open(now) {
      const warm = now < warmUntil;
      return { delayMs: warm ? 0 : OPEN_DELAY_MS, cold: !warm };
    },
    close(now, wasOpen) {
      // Only a tooltip that was on screen leaves the group warm. Brushing
      // across a control and leaving before the delay elapsed must not make
      // the next one instant - that is the case the delay exists for.
      if (wasOpen) warmUntil = now + WARM_MS;
    },
  };
}

/** Shared by every tooltip on the page: warmth is a property of the group. */
export const tooltipTiming = createTooltipTiming();
