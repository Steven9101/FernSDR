import { beforeEach, describe, expect, it, vi } from 'vitest';
import { DEFAULT_LAYOUT, layout, loadLayout, meterFace, resetLayout, setLayout } from './layout';

const stored = new Map<string, string>();
beforeEach(() => {
  stored.clear();
  vi.stubGlobal('localStorage', {
    getItem: (k: string) => stored.get(k) ?? null,
    setItem: (k: string, v: string) => stored.set(k, v),
    removeItem: (k: string) => stored.delete(k),
  });
  layout.value = DEFAULT_LAYOUT;
});

describe('the listener layout', () => {
  it('keeps the listener choices across a reload and resets to the station look', () => {
    setLayout({ meter: 'needle', side: 'left', show: { volume: false } });
    layout.value = DEFAULT_LAYOUT;
    loadLayout();
    expect(layout.value).toEqual({ meter: 'needle', side: 'left', show: { meter: true, volume: false, band: true, tools: true } });
    resetLayout();
    loadLayout();
    expect(layout.value).toEqual(DEFAULT_LAYOUT);
  });

  it('ignores what it does not understand', () => {
    stored.set('fernsdr.layout.v1', '{"meter":"<script>","side":"top","show":{"meter":"no","tools":false}}');
    loadLayout();
    expect(layout.value).toEqual({ meter: 'station', side: 'right', show: { meter: true, volume: true, band: true, tools: false } });
  });

  it("lets the listener's meter win over the operator's", () => {
    expect(meterFace('station', 'needle')).toBe('needle');
    expect(meterFace('station', undefined)).toBe('bar');
    expect(meterFace('numeric', 'needle')).toBe('numeric');
  });
});
