import { describe, expect, it } from 'vitest';
import { createTooltipTiming, OPEN_DELAY_MS, WARM_MS } from './tooltip-timing';

describe('tooltip timing', () => {
  it('makes the first tooltip wait, and animates it', () => {
    const timing = createTooltipTiming();
    expect(timing.open(1000)).toEqual({ delayMs: OPEN_DELAY_MS, cold: true });
  });

  it('is instant while the group is warm, with no entrance', () => {
    const timing = createTooltipTiming();
    timing.open(1000);
    timing.close(2000, true);
    expect(timing.open(2000 + WARM_MS - 1)).toEqual({ delayMs: 0, cold: false });
  });

  it('goes cold again once the warmth expires', () => {
    const timing = createTooltipTiming();
    timing.close(2000, true);
    expect(timing.open(2000 + WARM_MS + 1)).toEqual({ delayMs: OPEN_DELAY_MS, cold: true });
  });

  it('does not warm up from a tooltip that never appeared', () => {
    // Brushing across a control and leaving before the delay elapsed is
    // exactly the case the delay exists for; it must not make the next one
    // instant.
    const timing = createTooltipTiming();
    timing.open(1000);
    timing.close(1100, false);
    expect(timing.open(1150)).toEqual({ delayMs: OPEN_DELAY_MS, cold: true });
  });

  it('keeps the group warm across a run of tooltips', () => {
    const timing = createTooltipTiming();
    let now = 0;
    timing.open(now);
    now += OPEN_DELAY_MS;
    for (let i = 0; i < 5; i++) {
      timing.close(now, true);
      now += 50;
      expect(timing.open(now).delayMs).toBe(0);
      now += 200;
    }
  });
});
