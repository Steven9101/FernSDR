/**
 * The listener's tone controls: bass and treble, kept per kind of mode since
 * what sounds right differs: broadcast AM wants its bass back, SSB speech
 * reads better with less of it, and CW wants neither. Applied in the browser
 * (two shelving filters after the decoder), so they cost the receiver
 * nothing.
 */
import { box } from './reactive.svelte';

export type ToneGroup = 'am' | 'ssb' | 'cw' | 'fm';
export interface ToneSetting {
  bass: number;
  treble: number;
}

/** Shelf gains in dB either way: enough to warm AM or thin out a muddy SSB voice. */
export const TONE_RANGE_DB = 12;
const STORAGE_KEY = 'fernsdr.tone.v1';
const FLAT: ToneSetting = { bass: 0, treble: 0 };
const GROUPS: readonly ToneGroup[] = ['am', 'ssb', 'cw', 'fm'];

export const tone = box<Record<ToneGroup, ToneSetting>>({ am: FLAT, ssb: FLAT, cw: FLAT, fm: FLAT });

export function toneGroup(mode: string): ToneGroup {
  // Broadcast FM is programme audio, as AM broadcast is: one tone for both.
  if (mode === 'am' || mode === 'sam' || mode === 'dsb' || mode === 'wfm') return 'am';
  if (mode === 'cw' || mode === 'cwl') return 'cw';
  if (mode === 'nfm') return 'fm';
  return 'ssb';
}

function clampDb(value: unknown): number {
  const n = Math.round(Number(value));
  return Number.isFinite(n) ? Math.max(-TONE_RANGE_DB, Math.min(TONE_RANGE_DB, n)) : 0;
}

export function loadTone(): void {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null');
    if (!parsed || typeof parsed !== 'object') return;
    const next = { ...tone.value };
    for (const group of GROUPS) {
      const v = parsed[group];
      if (v && typeof v === 'object') next[group] = { bass: clampDb(v.bass), treble: clampDb(v.treble) };
    }
    tone.value = next;
  } catch {
    // Flat it is.
  }
}

export function setTone(mode: string, change: Partial<ToneSetting>): void {
  const group = toneGroup(mode);
  const current = tone.value[group];
  const next = { ...tone.value, [group]: { bass: clampDb(change.bass ?? current.bass), treble: clampDb(change.treble ?? current.treble) } };
  tone.value = next;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
  } catch {
    // Kept for this page only.
  }
}
