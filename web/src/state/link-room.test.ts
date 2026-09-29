import { describe, expect, it } from 'vitest';
import { measuredDownlink } from './link-room';

const script = (transferSize: number, ms: number) => ({ initiatorType: 'script', transferSize, responseStart: 100, responseEnd: 100 + ms });

describe('the link as the page measured it', () => {
  it("reads the largest script's transfer, first byte to last", () => {
    // 76 kB in 25 s: a 24 kbit/s link.
    expect(Math.round(measuredDownlink([script(76_000, 25_000)])!)).toBe(24_320);
    // The same in 60 ms: about 10 Mbit/s.
    expect(measuredDownlink([script(76_000, 60), script(30_000, 900)])!).toBeGreaterThan(9_000_000);
  });

  it('says nothing for a script from the cache or with no timing', () => {
    expect(measuredDownlink([script(0, 10)])).toBeNull();
    expect(measuredDownlink([{ initiatorType: 'script', transferSize: 76_000, responseStart: 0, responseEnd: 0 }])).toBeNull();
    expect(measuredDownlink([{ initiatorType: 'css', transferSize: 76_000, responseStart: 1, responseEnd: 2 }])).toBeNull();
  });
});
