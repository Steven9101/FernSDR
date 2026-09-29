import { describe, expect, it } from 'vitest';
import { decodeHistory, historyReach, historyRowRange, historyWindow, timeTicks } from './history';

function record(extra = {}, bytes = 16) {
  const header = new TextEncoder().encode(JSON.stringify({ bins: 16, floor_db: -140, ceiling_db: -20,
    low_hz: 0, high_hz: 32e6, oldest_ms: 1000, newest_ms: 1000, row_interval_ms: 1000,
    times: [1000], ...extra }));
  const buffer = new ArrayBuffer(4 + header.length + bytes);
  new DataView(buffer).setUint32(0, header.length, true);
  new Uint8Array(buffer, 4, header.length).set(header);
  return buffer;
}
describe('waterfall history', () => {
  it('uses the stored spectrum bounds, including the unused capture edges', () => {
    expect(decodeHistory(record())).toMatchObject({ lowHz: 0, highHz: 32e6, bins: 16 });
  });
  it('rejects truncated, oversized and unordered records', () => {
    for (const buffer of [new ArrayBuffer(3), record({}, 15), record({ bins: 1e9 }),
      record({ times: [2000, 1000] }, 32), record({ high_hz: 0 })]) {
      expect(() => decodeHistory(buffer)).toThrow();
    }
  });
  it('keeps outages blank and clamps the scrub range to the chosen span', () => {
    expect(historyRowRange(1000, 1000, 0, 10000, 100)).toEqual([80, 90]);
    expect(historyRowRange(7000, 1000, 0, 10000, 100)).toEqual([20, 30]);
    expect(historyReach(1000, 1000 + 24 * 3_600_000, 24)).toBe(0);
    expect(historyReach(1000, 1000 + 1.1 * 3_600_000, 1)).toBeCloseTo(0.1);
  });
  it('shows the available part of a young archive without hiding an ongoing outage', () => {
    expect(historyWindow(0, 10000, 7000)).toEqual({ from: 7000, to: 10000 });
    expect(historyWindow(8000, 10000, 7000)).toEqual({ from: 8000, to: 10000 });
    expect(historyWindow(0, 10000, 11000)).toEqual({ from: 0, to: 10000 });
    expect(historyRowRange(1000, 1000, 0, 10000, 100)[1]).toBeLessThanOrEqual(100);
    expect(historyRowRange(9000, 1000, 0, 10000, 100)).toEqual([0, 10]);
  });
});

describe('history time marks', () => {
  const at = (iso: string) => Date.parse(iso);

  it('puts marks on round clock times, newest at the top', () => {
    const ticks = timeTicks(at('2026-09-28T13:47:00Z'), at('2026-09-28T14:47:00Z'), 400, 'utc');
    // 400 px for an hour: ten minutes is the first step at least 36 px apart.
    expect(ticks.map((t) => t.label)).toEqual(['13:50', '14:00', '14:10', '14:20', '14:30', '14:40']);
    expect(ticks[0].position).toBeCloseTo(57 / 60, 5);
    expect(ticks.every((t, i) => i === 0 || t.position < ticks[i - 1].position)).toBe(true);
  });

  it('keeps marks apart on a short picture and names the day at midnight', () => {
    const ticks = timeTicks(at('2026-09-28T12:00:00Z'), at('2026-09-29T12:00:00Z'), 240, 'utc');
    expect(ticks.length).toBeLessThanOrEqual(240 / 36 + 1);
    const midnight = ticks.find((t) => t.day);
    expect(midnight?.label).toBe('Tue 29');
    expect(midnight?.time).toBe(at('2026-09-29T00:00:00Z'));
  });

  it('follows the local clock when asked', () => {
    const ticks = timeTicks(at('2026-09-28T13:47:00Z'), at('2026-09-28T14:47:00Z'), 400, 'local');
    expect(ticks.length).toBe(6);
    for (const tick of ticks) expect(new Date(tick.time).getMinutes() % 10).toBe(0);
  });

  it('draws nothing for an empty span', () => {
    expect(timeTicks(10, 10, 400, 'utc')).toEqual([]);
    expect(timeTicks(0, 1000, 0, 'utc')).toEqual([]);
  });
});
