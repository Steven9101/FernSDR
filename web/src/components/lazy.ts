import type { Component } from 'svelte';

/**
 * A component loaded on first use, for the parts of the page most listeners
 * never open.
 *
 * Returns the component itself once it has arrived, so `{#await}` shows it
 * again at once rather than flashing its loading text; a load that failed is
 * forgotten, so the next attempt tries again.
 */
export function lazy<Props extends Record<string, any>>(
  load: () => Promise<{ default: Component<Props> }>,
): () => Component<Props> | Promise<Component<Props>> {
  let cached: Component<Props> | Promise<Component<Props>> | null = null;
  return () => cached ??= load().then(
    (module) => (cached = module.default),
    (error: unknown) => {
      cached = null;
      throw error;
    },
  );
}
