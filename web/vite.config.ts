/// <reference types="node" />
import { defineConfig } from 'vitest/config';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import tailwindcss from '@tailwindcss/vite';
import { thirdPartyLicenses } from './tools/third-party-licenses';

// The dev server proxies to a running fernsdr so the client can be developed
// against a real receiver without a build step in between.
const SERVER = globalThis.process?.env?.FERNSDR_SERVER ?? 'http://127.0.0.1:8073';

// `vite build` builds the listener's page and `vite build --mode admin` then
// adds the admin panel to the same directory. Built together, the Svelte
// runtime both pages use went into one shared chunk holding everything
// either page needs, and every listener downloaded the admin's share of it.
export default defineConfig(({ mode }) => ({
  // Both pages are Svelte. Tailwind only ever sees the admin's stylesheet,
  // which limits itself to the admin's own files.
  plugins: [
    tailwindcss(),
    svelte(),
    mode === 'admin'
      ? thirdPartyLicenses('admin-licenses.txt',
        'The FernSDR admin panel includes the following software from others, each under the licence shown.')
      : thirdPartyLicenses('licenses.txt',
        'FernSDR is free software under the GNU Affero General Public License, version 3. This page includes '
        + 'the following software from others, each under the licence shown.'),
  ],
  build: {
    target: 'es2022',
    emptyOutDir: mode !== 'admin',
    // The audio worklet must be a separate, self-contained module: it is
    // loaded by the AudioContext, not imported by the page.
    rollupOptions: {
      // The admin panel is a second page rather than a route in the receiver:
      // listeners should never download it, and it should never be able to
      // break the thing people came for.
      input: mode === 'admin' ? { admin: 'admin.html' } : { index: 'index.html' } as Record<string, string>,
      output: {
        assetFileNames: 'assets/[name]-[hash][extname]',
        chunkFileNames: 'assets/[name]-[hash].js',
        entryFileNames: 'assets/[name]-[hash].js',
      },
    },
    // Everything is served from the receiver itself, often over a slow link;
    // keep the bundle honest.
    chunkSizeWarningLimit: 300,
  },
  server: {
    port: 5173,
    proxy: {
      '/ws': { target: SERVER, ws: true },
      '/api': { target: SERVER },
    },
  },
  test: {
    projects: [
      {
        extends: true,
        test: { name: 'unit', environment: 'node', include: ['src/**/*.test.ts'], exclude: ['src/state/**'] },
      },
      {
        // The listener's state is built on runes, and runes react only in
        // Svelte's client build; see vitest-client-environment.ts. The
        // browser condition gives the tests' own imports of svelte (flushSync)
        // the same client runtime the compiled code uses. A test of anything
        // that imports the store belongs here: in the unit project its
        // watchers would never run.
        extends: true,
        resolve: { conditions: ['browser'] },
        test: {
          name: 'state',
          environment: './vitest-client-environment.ts',
          include: ['src/state/**/*.test.ts'],
        },
      },
    ],
  },
}));
