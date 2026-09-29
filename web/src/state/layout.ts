/**
 * How the listener has arranged the page for themselves: which meter they
 * read, which side the controls are on, and what sits on the dial. The
 * operator's theme brands the receiver and picks the meter it starts with;
 * what a listener chooses here wins, for them, in this browser only.
 */
import { box } from './reactive.svelte';

export type MeterFace = 'bar' | 'needle' | 'numeric' | 'history';

export interface Layout {
  /** 'station' follows the operator's choice. */
  meter: 'station' | MeterFace;
  side: 'right' | 'left';
  show: { meter: boolean; volume: boolean; band: boolean; tools: boolean };
}

const STORAGE_KEY = 'fernsdr.layout.v1';
export const DEFAULT_LAYOUT: Layout = {
  meter: 'station',
  side: 'right',
  show: { meter: true, volume: true, band: true, tools: true },
};
const FACES = new Set(['station', 'bar', 'needle', 'numeric', 'history']);

export const layout = box<Layout>(DEFAULT_LAYOUT);

export function validLayout(value: unknown): Layout {
  if (!value || typeof value !== 'object') return DEFAULT_LAYOUT;
  const v = value as Record<string, unknown>;
  const show = (v.show && typeof v.show === 'object' ? v.show : {}) as Record<string, unknown>;
  const flag = (key: keyof Layout['show']) => (typeof show[key] === 'boolean' ? (show[key] as boolean) : true);
  return {
    meter: typeof v.meter === 'string' && FACES.has(v.meter) ? (v.meter as Layout['meter']) : 'station',
    side: v.side === 'left' ? 'left' : 'right',
    show: { meter: flag('meter'), volume: flag('volume'), band: flag('band'), tools: flag('tools') },
  };
}

export function loadLayout(): void {
  try {
    layout.value = validLayout(JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null'));
  } catch {
    layout.value = DEFAULT_LAYOUT;
  }
}

export function setLayout(change: Partial<Omit<Layout, 'show'>> & { show?: Partial<Layout['show']> }): void {
  const current = layout.value;
  const next = validLayout({ ...current, ...change, show: { ...current.show, ...(change.show ?? {}) } });
  layout.value = next;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
  } catch {
    // Kept for this page only.
  }
}

export function resetLayout(): void {
  layout.value = DEFAULT_LAYOUT;
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    // Nothing stored to remove.
  }
}

/** The meter face in use: the listener's own, else the operator's, else the bar. */
export function meterFace(own: Layout['meter'], operator: string | undefined): MeterFace {
  if (own !== 'station') return own;
  return operator === 'needle' || operator === 'numeric' || operator === 'history' ? operator : 'bar';
}
