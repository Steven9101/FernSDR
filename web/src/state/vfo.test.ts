import { beforeEach, describe, expect, it, vi } from 'vitest';
import { equalizeVfo, loadVfo, swapVfo, vfo } from './vfo';

const stored = new Map<string, string>();
beforeEach(() => {
  stored.clear();
  vi.stubGlobal('localStorage', { getItem: (k: string) => stored.get(k) ?? null, setItem: (k: string, v: string) => stored.set(k, v) });
  vfo.value = { active: 'a', a: null, b: null };
});

const at = (freq: number, mode = 'usb') => ({ freq, mode, low: 300, high: 2700 });

describe('VFO A and B', () => {
  it('swaps between two places and keeps where each was', () => {
    const applied: number[] = [];
    // B never used: it starts where A is.
    swapVfo(at(7_074_000), (s) => applied.push(s.freq));
    expect(vfo.value.active).toBe('b');
    swapVfo(at(14_074_000), (s) => applied.push(s.freq));
    expect(vfo.value.active).toBe('a');
    swapVfo(at(7_074_500), (s) => applied.push(s.freq));
    expect(applied).toEqual([7_074_000, 7_074_000, 14_074_000]);
    expect(vfo.value.a!.freq).toBe(7_074_500);
  });

  it('copies A into B, and survives a reload', () => {
    equalizeVfo(at(3_573_000, 'usb'));
    vfo.value = { active: 'a', a: null, b: null };
    loadVfo();
    expect(vfo.value.b!.freq).toBe(3_573_000);
    stored.set('fernsdr.vfo.v1', '{"active":"x","a":{"freq":-1},"b":"nonsense"}');
    loadVfo();
    expect(vfo.value).toEqual({ active: 'a', a: null, b: null });
  });

  it('keeps each VFO\'s band and view, and drops a damaged view', () => {
    swapVfo({ ...at(7_074_000), band: '40m', view: [7_050_000, 7_100_000] }, () => {});
    swapVfo({ ...at(14_074_000), band: '20m', view: [14_000_000, 14_350_000] }, () => {});
    loadVfo();
    expect(vfo.value.a).toMatchObject({ band: '40m', view: [7_050_000, 7_100_000] });
    expect(vfo.value.b).toMatchObject({ band: '20m', view: [14_000_000, 14_350_000] });
    stored.set('fernsdr.vfo.v1', '{"active":"a","a":{"freq":7074000,"mode":"usb","low":300,"high":2700,"view":[9,3]}}');
    loadVfo();
    expect(vfo.value.a).toEqual({ freq: 7_074_000, mode: 'usb', low: 300, high: 2700 });
  });
});
