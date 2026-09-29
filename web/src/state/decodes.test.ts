import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DecodeFeed, distanceKm, filterDecodes, mergeDecodes, parseDecodes, validDecode, type Decode } from './decodes';

const decode = (seq: number, message = 'CQ K1ABC FN42', channel = '40m-ft8-7074'): Decode => ({
  seq, decoder: 'ft8', channel, band: '40m', mode: 'ft8', time: 1_700_000_000_000, dial: 7_074_000, freq: 1200,
  snr: -12, dt: 0.1, message, quality: 'bp',
});

describe('decodes', () => {
  it('takes what the server sends and drops what it should not have', () => {
    expect(validDecode({ ...decode(1), call: 'K1ABC', grid: 'FN42' })).toMatchObject({ call: 'K1ABC', grid: 'FN42' });
    expect(validDecode({ ...decode(1), message: 'x'.repeat(65) })).toBeNull();
    expect(validDecode({ ...decode(1), snr: 'loud' })).toBeNull();
    expect(validDecode({ ...decode(0) })).toBeNull();
    expect(validDecode({ ...decode(1), grid: 'far too long' })?.grid).toBeUndefined();
    expect(parseDecodes({ epoch: 'a1', through: 3, decodes: [decode(3), { seq: 'x' }] })).toEqual({ epoch: 'a1', through: 3, decodes: [decode(3)] });
    expect(parseDecodes({ through: -1, decodes: [] })).toBeNull();
    expect(parseDecodes({ decodes: [] })).toBeNull();
  });

  it('keeps each decode once, in order, and no more than the cap', () => {
    let kept = mergeDecodes([], [decode(2), decode(1)]);
    expect(kept.map((d) => d.seq)).toEqual([1, 2]);
    kept = mergeDecodes(kept, [decode(2), decode(3)]);
    expect(kept.map((d) => d.seq)).toEqual([1, 2, 3]);
    expect(mergeDecodes(kept, [decode(4), decode(5)], 3).map((d) => d.seq)).toEqual([3, 4, 5]);
    expect(mergeDecodes(kept, [])).toBe(kept);
  });

  it('filters by channel, by CQ and by what the message contains', () => {
    const list = [decode(1), decode(2, 'K1ABC W9XYZ -12'), decode(3, 'CQ DX DL1AB JO31', '20m-ft8-14074')];
    expect(filterDecodes(list, { channel: '', cqOnly: true, search: '' }).map((d) => d.seq)).toEqual([1, 3]);
    expect(filterDecodes(list, { channel: '20m-ft8-14074', cqOnly: false, search: '' }).map((d) => d.seq)).toEqual([3]);
    expect(filterDecodes(list, { channel: '', cqOnly: false, search: 'w9x' }).map((d) => d.seq)).toEqual([2]);
    expect(filterDecodes(list, { channel: '', cqOnly: true, search: 'CQM' })).toEqual([]);
  });

  it('measures the distance between two places', () => {
    // Amsterdam to New York, about 5,860 km.
    expect(distanceKm({ lat: 52.37, lon: 4.9 }, { lat: 40.71, lon: -74.01 })).toBeGreaterThan(5800);
    expect(distanceKm({ lat: 52.37, lon: 4.9 }, { lat: 40.71, lon: -74.01 })).toBeLessThan(5900);
  });
});

describe('the decode feed', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('reads the newest first, then only what came after, and stops when nobody reads', async () => {
    const urls: string[] = [];
    const answers = [{ through: 2, decodes: [decode(1), decode(2)] }, { through: 3, decodes: [decode(3)] }];
    const feed = new DecodeFeed(async (url) => {
      urls.push(url);
      return answers.shift() ?? { through: 3, decodes: [] };
    });
    const close = feed.open();
    await vi.advanceTimersByTimeAsync(0);
    expect(feed.status.value).toBe('live');
    expect(feed.list.value.map((d) => d.seq)).toEqual([1, 2]);
    await vi.advanceTimersByTimeAsync(5000);
    expect(urls[1]).toContain('since=2');
    expect(feed.list.value.map((d) => d.seq)).toEqual([1, 2, 3]);
    close();
    close();
    const asked = urls.length;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(urls.length).toBe(asked);
  });

  it('runs one loop when a view closes and opens while a request is out', async () => {
    let calls = 0;
    const feed = new DecodeFeed((_url, signal) => {
      calls++;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => resolve({ through: 0, decodes: [] }), 1000);
        signal.addEventListener('abort', () => {
          clearTimeout(timer);
          reject(new Error('aborted'));
        });
      });
    });
    feed.open()();
    const close = feed.open();
    await vi.advanceTimersByTimeAsync(30_000);
    // Two at the start, then one every six seconds (one to answer, five to wait).
    expect(calls).toBeLessThanOrEqual(2 + 5);
    close();
  });

  it('starts over when the receiver restarted and numbers its decodes from 1 again', async () => {
    const urls: string[] = [];
    const answers = [
      { epoch: 'one', through: 900, decodes: [decode(899), decode(900)] },
      { epoch: 'two', through: 2, decodes: [] },
      { epoch: 'two', through: 2, decodes: [decode(1), decode(2)] },
      { epoch: 'two', through: 3, decodes: [decode(3)] },
    ];
    const feed = new DecodeFeed(async (url) => {
      urls.push(url);
      return answers.shift() ?? { epoch: 'two', through: 3, decodes: [] };
    });
    const close = feed.open();
    await vi.advanceTimersByTimeAsync(0);
    expect(feed.list.value.map((d) => d.seq)).toEqual([899, 900]);
    await vi.advanceTimersByTimeAsync(5000);
    expect(urls[1]).toContain('since=900');
    expect(urls[2]).not.toContain('since=');
    expect(feed.list.value.map((d) => d.seq)).toEqual([1, 2]);
    await vi.advanceTimersByTimeAsync(5000);
    expect(urls[3]).toContain('since=2');
    expect(feed.list.value.map((d) => d.seq)).toEqual([1, 2, 3]);
    close();
  });

  it('says when the receiver cannot be read, and keeps trying', async () => {
    let fail = true;
    const feed = new DecodeFeed(async () => {
      if (fail) throw new Error('503');
      return { through: 1, decodes: [decode(1)] };
    });
    const close = feed.open();
    await vi.advanceTimersByTimeAsync(0);
    expect(feed.status.value).toBe('error');
    fail = false;
    await vi.advanceTimersByTimeAsync(5000);
    expect(feed.status.value).toBe('live');
    close();
  });
});
