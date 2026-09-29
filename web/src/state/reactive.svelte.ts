/**
 * The listener page's reactive values: a box to hold state, a computed value
 * and a watcher, on Svelte's runes. Named apart from the runes, which
 * components use beside them.
 *
 * A box rather than exported runes because a module cannot export `$state`
 * that other modules assign, and the store is assigned from everywhere. Boxes
 * hold their value raw: the store replaces objects rather than mutating them,
 * and a deep proxy would cost a copy on every meter reading.
 */
import { untrack } from 'svelte';

export interface ReadonlyBox<T> {
  readonly value: T;
  /** The value without making the caller depend on it. */
  peek(): T;
}

export interface Box<T> extends ReadonlyBox<T> {
  value: T;
}

class StateBox<T> implements Box<T> {
  #value: T;

  constructor(initial: T) {
    this.#value = $state.raw(initial);
  }

  get value(): T {
    return this.#value;
  }

  set value(next: T) {
    this.#value = next;
  }

  peek(): T {
    return untrack(() => this.#value);
  }
}

class ComputedBox<T> implements ReadonlyBox<T> {
  readonly #value: T;

  constructor(compute: () => T) {
    this.#value = $derived.by(compute);
  }

  get value(): T {
    return this.#value;
  }

  peek(): T {
    return untrack(() => this.#value);
  }
}

export function box<T>(initial: T): Box<T> {
  return new StateBox(initial);
}

export function computed<T>(compute: () => T): ReadonlyBox<T> {
  return new ComputedBox(compute);
}

/**
 * Runs `run` and runs it again whenever a value it read changes; returns a
 * stop function. The runs are batched: several changes in one task cause one
 * run, after the task, never a run in the middle of an assignment. Code that
 * has to act at a precise point says so explicitly instead.
 */
export function watch(run: () => void | (() => void)): () => void {
  return $effect.root(() => {
    $effect(run);
  });
}
