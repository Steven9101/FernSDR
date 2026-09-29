/**
 * Whether the listener's radio is linked, apart from the link itself so the
 * button under the dial can show it without loading the drivers.
 */
import { box } from '../state/reactive.svelte';
import type { ReceiverMode } from './drivers';

export interface RigState {
  status: 'off' | 'connecting' | 'on' | 'error';
  /** What the listener should know now: a problem, or why nothing happens. */
  message: string;
  freq: number | null;
  mode: ReceiverMode | null;
}

export const rigState = box<RigState>({ status: 'off', message: '', freq: null, mode: null });
