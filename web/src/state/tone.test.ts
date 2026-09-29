import { beforeEach, describe, expect, it, vi } from 'vitest';
import { loadTone, setTone, tone, toneGroup } from './tone';

const stored = new Map<string, string>();
beforeEach(() => {
  stored.clear();
  vi.stubGlobal('localStorage', { getItem: (k: string) => stored.get(k) ?? null, setItem: (k: string, v: string) => stored.set(k, v) });
  const flat = { bass: 0, treble: 0 };
  tone.value = { am: flat, ssb: flat, cw: flat, fm: flat };
});

describe('tone controls', () => {
  it('keeps one setting per kind of mode, clamped, across a reload', () => {
    setTone('sam', { bass: 8 });
    setTone('usb', { bass: -4, treble: 40 });
    expect(toneGroup('am')).toBe('am');
    expect(tone.value.am).toEqual({ bass: 8, treble: 0 });
    expect(tone.value.ssb).toEqual({ bass: -4, treble: 12 });
    tone.value = { ...tone.value, am: { bass: 0, treble: 0 } };
    loadTone();
    expect(tone.value.am.bass).toBe(8);
    expect(tone.value.cw).toEqual({ bass: 0, treble: 0 });
  });

  it('ignores stored nonsense', () => {
    stored.set('fernsdr.tone.v1', '{"am":{"bass":"loud","treble":null},"fm":7}');
    loadTone();
    expect(tone.value.am).toEqual({ bass: 0, treble: 0 });
    expect(tone.value.fm).toEqual({ bass: 0, treble: 0 });
  });
});
