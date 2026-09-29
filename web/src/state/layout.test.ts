import { beforeEach, describe, expect, it, vi } from 'vitest';
import { DEFAULT_LAYOUT, isOwnLayout, layout, loadLayout, meterFace, resetLayout, setLayout } from './layout';

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
    expect(layout.value).toEqual({
      ...DEFAULT_LAYOUT,
      meter: 'needle',
      side: 'left',
      show: { ...DEFAULT_LAYOUT.show, volume: false },
    });
    resetLayout();
    loadLayout();
    expect(layout.value).toEqual(DEFAULT_LAYOUT);
  });

  it('ignores what it does not understand', () => {
    stored.set('fernsdr.layout.v1', '{"meter":"<script>","side":"top","show":{"meter":"no","tools":false},"tools":{"record":false,"rig":"x"},"spectrum":1e9}');
    loadLayout();
    expect(layout.value).toEqual({
      ...DEFAULT_LAYOUT,
      show: { ...DEFAULT_LAYOUT.show, tools: false },
      tools: { ...DEFAULT_LAYOUT.tools, record: false },
      spectrum: 600,
    });
  });

  it('keeps a layout saved before the edit mode, with everything new shown', () => {
    stored.set('fernsdr.layout.v1', '{"meter":"bar","side":"left","show":{"meter":true,"volume":false,"band":true,"tools":true}}');
    loadLayout();
    expect(layout.value.show).toEqual({ meter: true, volume: false, band: true, tools: true, status: true });
    expect(layout.value.tools).toEqual(DEFAULT_LAYOUT.tools);
    expect(layout.value.spectrum).toBeNull();
  });

  it('hides one tool and sets the spectrum without touching the rest', () => {
    setLayout({ tools: { logbook: false } });
    setLayout({ spectrum: 212.4 });
    expect(layout.value.tools).toEqual({ ...DEFAULT_LAYOUT.tools, logbook: false });
    expect(layout.value.spectrum).toBe(212);
    expect(isOwnLayout(layout.value)).toBe(true);
    resetLayout();
    expect(isOwnLayout(layout.value)).toBe(false);
  });

  it("lets the listener's meter win over the operator's", () => {
    expect(meterFace('station', 'needle')).toBe('needle');
    expect(meterFace('station', undefined)).toBe('bar');
    expect(meterFace('numeric', 'needle')).toBe('numeric');
  });
});
