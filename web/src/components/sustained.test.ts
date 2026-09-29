import { describe, expect, it } from 'vitest';
import { Sustained } from './sustained';

describe('Sustained', () => {
  it('shows only after the condition has held, and ignores a moment of it', () => {
    const notice = new Sustained(5000, 15000);
    expect(notice.update(0, true)).toBe(false);
    expect(notice.update(3000, false)).toBe(false);
    expect(notice.update(4000, true)).toBe(false);
    expect(notice.update(8900, true)).toBe(false);
    expect(notice.update(9000, true)).toBe(true);
  });

  it('stays through a rate that flickers around the threshold, then goes', () => {
    const notice = new Sustained(5000, 15000);
    notice.update(0, true);
    expect(notice.update(5000, true)).toBe(true);
    for (let t = 6000; t < 60000; t += 1000) expect(notice.update(t, t % 2000 === 0)).toBe(true);
    expect(notice.update(61000, false)).toBe(true);
    // Last active at 58 s, inactive from 59 s: gone fifteen seconds later.
    expect(notice.update(73999, false)).toBe(true);
    expect(notice.update(74000, false)).toBe(false);
  });
});
