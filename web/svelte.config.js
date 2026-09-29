import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

// TypeScript in <script lang="ts">, compiled by the same esbuild as the rest.
export default {
  preprocess: vitePreprocess(),
};
