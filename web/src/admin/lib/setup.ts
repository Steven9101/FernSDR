/**
 * Where the setup flow is, kept in this browser so it can be left and taken
 * up again, after the receiver restarts too, and whether it is done here.
 * The receiver itself says what is set up; this only remembers the place.
 */
import { navigate } from './router.svelte';

export const SETUP_STEPS = ['password', 'station', 'radio', 'bands', 'listeners', 'done'] as const;
export type SetupStep = (typeof SETUP_STEPS)[number];

const STEP_KEY = 'fernsdr.setup.step';
const DONE_KEY = 'fernsdr.setup.done';

/** The step to show, or with `step`, where to be the next time. */
export function setupStep(step?: SetupStep): SetupStep {
  try {
    if (step) localStorage.setItem(STEP_KEY, step);
    const kept = localStorage.getItem(STEP_KEY);
    if (kept && (SETUP_STEPS as readonly string[]).includes(kept)) return kept as SetupStep;
  } catch {
    // No storage, as in a private window: the flow starts at the top each time.
  }
  return step ?? 'password';
}

/** Whether a setup was begun here and not finished, such as one a restart broke off. */
export function setupInProgress(): boolean {
  try {
    return localStorage.getItem(STEP_KEY) !== null && !setupDone();
  } catch {
    return false;
  }
}

export function setupDone(): boolean {
  try {
    return localStorage.getItem(DONE_KEY) === '1';
  } catch {
    return false;
  }
}

/** Marks the flow done in this browser, where the operator would rather set things up page by page. */
export function dismissSetup(): void {
  try {
    localStorage.setItem(DONE_KEY, '1');
    localStorage.removeItem(STEP_KEY);
  } catch {
    // Nothing to keep it in.
  }
}

/** Marks the flow done in this browser and goes to the overview. */
export function finishSetup(): void {
  try {
    localStorage.setItem(DONE_KEY, '1');
    localStorage.removeItem(STEP_KEY);
  } catch {
    // Nothing to keep it in; the overview's checklist says what is left.
  }
  navigate({ page: 'overview' });
}

/**
 * Whether a receiver looks as the installer left it: no band but the test
 * signal. That is when the flow opens by itself after signing in.
 */
export function looksNew(sources: readonly string[]): boolean {
  return sources.length > 0 && sources.every((source) => source === 'test');
}
