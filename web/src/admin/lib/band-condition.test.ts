import { describe, expect, it } from 'vitest';
import { bandCondition, isTrouble, quietSummary } from './live.svelte';

const day = { running: true, online: true, restarting: false, on_air: true };
const night = { running: false, restarting: false, on_air: false, status: 'off the air until 18:00' };

describe('band conditions', () => {
  it('does not count a band off the air by its hours as trouble', () => {
    expect(isTrouble(bandCondition(night))).toBe(false);
    expect(isTrouble(bandCondition({ running: false, restarting: false }))).toBe(true);
  });

  it('says a day and night pair is one receiving and one off the air, not all receiving', () => {
    expect(quietSummary([bandCondition(day), bandCondition(night)])).toBe('1 receiving, 1 off the air, as scheduled');
    expect(quietSummary([bandCondition(day), bandCondition(day)])).toBe('All 2 bands receiving');
    expect(quietSummary([bandCondition(day)])).toBe('Receiving');
  });
});
