import type { Environment } from 'vitest/environments';

/**
 * Node, with the page's modules built as the browser gets them. Svelte's
 * runes then compile for the client, where effects run; compiled for the
 * server, $effect does nothing, and a test of reactive code would pass with
 * nothing reacting.
 */
export default {
  name: 'node-client',
  viteEnvironment: 'client',
  setup() {
    return { teardown() {} };
  },
} satisfies Environment;
