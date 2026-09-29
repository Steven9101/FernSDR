import { flushSync } from 'svelte';
import { describe, expect, it } from 'vitest';
import { box, computed, watch } from './reactive.svelte';

// These run the client build of the runes (see vite.config.ts). Built for
// the server, a watcher never runs, and these tests are what would say so.
describe('reactive values', () => {
  it('recomputes a computed value when a box it reads changes', () => {
    const a = box(2);
    const doubled = computed(() => a.value * 2);
    expect(doubled.value).toBe(4);
    a.value = 5;
    expect(doubled.value).toBe(10);
  });

  it('runs a watcher after the changes of a task, once, until stopped', () => {
    const a = box(1);
    const seen: number[] = [];
    const stop = watch(() => {
      seen.push(a.value);
    });
    flushSync();
    expect(seen).toEqual([1]);
    a.value = 2;
    a.value = 3;
    expect(seen).toEqual([1]);
    flushSync();
    expect(seen).toEqual([1, 3]);
    stop();
    a.value = 4;
    flushSync();
    expect(seen).toEqual([1, 3]);
  });

  it('does not make a watcher depend on what it peeks at', () => {
    const a = box(1);
    const b = box(10);
    let runs = 0;
    const stop = watch(() => {
      runs++;
      void a.value;
      void b.peek();
    });
    flushSync();
    b.value = 11;
    flushSync();
    expect(runs).toBe(1);
    a.value = 2;
    flushSync();
    expect(runs).toBe(2);
    stop();
  });

  it('follows a computed value into a watcher, and only when it changes', () => {
    const a = box(3);
    const parity = computed(() => a.value % 2);
    const seen: number[] = [];
    const stop = watch(() => {
      seen.push(parity.value);
    });
    flushSync();
    a.value = 5;
    flushSync();
    a.value = 6;
    flushSync();
    expect(seen).toEqual([1, 0]);
    stop();
  });

  it('cleans up before each run and when stopped', () => {
    const a = box(1);
    const log: string[] = [];
    const stop = watch(() => {
      const value = a.value;
      log.push(`run ${value}`);
      return () => log.push(`clean ${value}`);
    });
    flushSync();
    a.value = 2;
    flushSync();
    stop();
    expect(log).toEqual(['run 1', 'clean 1', 'run 2', 'clean 2']);
  });
});
