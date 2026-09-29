/**
 * How the listener has arranged the page for themselves: which meter they
 * read, which side the controls are on, what sits on the dial and under it,
 * and how tall the spectrum is over the waterfall. The operator's theme
 * brands the receiver and picks the meter it starts with; what a listener
 * chooses here wins, for them, in this browser only. The edit mode
 * (components/EditLayoutBar.svelte) is where it is chosen.
 */
import { box } from './reactive.svelte';

export type MeterFace = 'bar' | 'needle' | 'numeric' | 'history';

export const TOOLS = ['vfo', 'bookmarks', 'record', 'logbook', 'rig'] as const;
export type Tool = (typeof TOOLS)[number];

export interface Layout {
  /** 'station' follows the operator's choice. */
  meter: 'station' | MeterFace;
  side: 'right' | 'left';
  show: { meter: boolean; volume: boolean; band: boolean; tools: boolean; status: boolean };
  /** Each tool under the dial, where the row is shown at all. */
  tools: Record<Tool, boolean>;
  /** The spectrum's height over the waterfall in CSS pixels, or null for the page's own. */
  spectrum: number | null;
}

/** The spectrum a listener may ask for: none at all, up to most of a tall screen. */
export const SPECTRUM_MIN = 0;
export const SPECTRUM_MAX = 600;

const STORAGE_KEY = 'fernsdr.layout.v1';
export const DEFAULT_LAYOUT: Layout = {
  meter: 'station',
  side: 'right',
  show: { meter: true, volume: true, band: true, tools: true, status: true },
  tools: { vfo: true, bookmarks: true, record: true, logbook: true, rig: true },
  spectrum: null,
};
const FACES = new Set(['station', 'bar', 'needle', 'numeric', 'history']);

export const layout = box<Layout>(DEFAULT_LAYOUT);
/** Whether the page is being arranged right now. Never stored. */
export const editingLayout = box<boolean>(false);

export function validLayout(value: unknown): Layout {
  if (!value || typeof value !== 'object') return DEFAULT_LAYOUT;
  const v = value as Record<string, unknown>;
  const record = (value: unknown) => (value && typeof value === 'object' ? value : {}) as Record<string, unknown>;
  const show = record(v.show);
  const tools = record(v.tools);
  // Anything missing is shown: a layout saved before a part existed keeps it.
  const flag = (from: Record<string, unknown>, key: string) => (typeof from[key] === 'boolean' ? (from[key] as boolean) : true);
  const spectrum =
    typeof v.spectrum === 'number' && Number.isFinite(v.spectrum)
      ? Math.round(Math.min(SPECTRUM_MAX, Math.max(SPECTRUM_MIN, v.spectrum)))
      : null;
  return {
    meter: typeof v.meter === 'string' && FACES.has(v.meter) ? (v.meter as Layout['meter']) : 'station',
    side: v.side === 'left' ? 'left' : 'right',
    show: {
      meter: flag(show, 'meter'),
      volume: flag(show, 'volume'),
      band: flag(show, 'band'),
      tools: flag(show, 'tools'),
      status: flag(show, 'status'),
    },
    tools: Object.fromEntries(TOOLS.map((tool) => [tool, flag(tools, tool)])) as Record<Tool, boolean>,
    spectrum,
  };
}

export function loadLayout(): void {
  try {
    layout.value = validLayout(JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null'));
  } catch {
    layout.value = DEFAULT_LAYOUT;
  }
}

export function setLayout(
  change: Partial<Omit<Layout, 'show' | 'tools'>> & { show?: Partial<Layout['show']>; tools?: Partial<Layout['tools']> },
): void {
  const current = layout.value;
  const next = validLayout({
    ...current,
    ...change,
    show: { ...current.show, ...(change.show ?? {}) },
    tools: { ...current.tools, ...(change.tools ?? {}) },
  });
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

/** Whether anything differs from the station's layout, for the reset to say so. */
export function isOwnLayout(value: Layout): boolean {
  return JSON.stringify(value) !== JSON.stringify(DEFAULT_LAYOUT);
}
