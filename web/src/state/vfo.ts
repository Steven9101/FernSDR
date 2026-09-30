/**
 * VFO A and B, as on a transceiver: two frequencies with their mode and
 * filter, one in use and the other a press away. Swapping keeps where the
 * one left behind was; A = B copies the one in use into the other.
 */
import { box } from './reactive.svelte';

export interface VfoSetting {
  /** The signal's frequency, as the dial shows it. */
  freq: number;
  mode: string;
  low: number;
  high: number;
  /** The CW pitch the edges were kept at, Hz: they move with it on the way back. */
  pitch?: number;
  /** The band it was on, where bands overlap. */
  band?: string;
  /** The waterfall's edges, Hz: switching back finds the view as it was left. */
  view?: [number, number];
}

export interface VfoState {
  active: 'a' | 'b';
  a: VfoSetting | null;
  b: VfoSetting | null;
}

const STORAGE_KEY = 'fernsdr.vfo.v1';

export const vfo = box<VfoState>({ active: 'a', a: null, b: null });

function valid(value: unknown): VfoSetting | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const freq = Number(v.freq), low = Number(v.low), high = Number(v.high);
  if (!(freq > 0) || !Number.isFinite(low) || !Number.isFinite(high) || low >= high || typeof v.mode !== 'string') return null;
  const setting: VfoSetting = { freq, mode: v.mode, low, high };
  const pitch = Number(v.pitch);
  if (v.pitch !== undefined && pitch >= 200 && pitch <= 1500) setting.pitch = pitch;
  if (typeof v.band === 'string' && v.band.length <= 64) setting.band = v.band;
  const view = Array.isArray(v.view) ? v.view.map(Number) : null;
  if (view && view.length === 2 && view[0] > 0 && view[1] > view[0]) setting.view = [view[0], view[1]];
  return setting;
}

function save(state: VfoState): void {
  vfo.value = state;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  } catch {
    // Kept for this page only.
  }
}

export function loadVfo(): void {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null');
    if (parsed) vfo.value = { active: parsed.active === 'b' ? 'b' : 'a', a: valid(parsed.a), b: valid(parsed.b) };
  } catch {
    // Start with both empty.
  }
}

/**
 * Switches to the other VFO: `current` is where the receiver is now, kept in
 * the one being left; `apply` tunes to the other. A VFO never used starts
 * where the receiver is.
 */
export function swapVfo(current: VfoSetting, apply: (setting: VfoSetting) => void): void {
  const state = vfo.value;
  const leaving = state.active;
  const arriving = leaving === 'a' ? 'b' : 'a';
  const target = state[arriving] ?? current;
  save({ ...state, [leaving]: current, [arriving]: target, active: arriving });
  apply(target);
}

/** Copies the VFO in use into the other one. */
export function equalizeVfo(current: VfoSetting): void {
  const state = vfo.value;
  save({ ...state, a: current, b: current });
}
