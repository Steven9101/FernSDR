/*
Svelte's transitions, held to the system's reduced-motion setting. They run through the Web
Animations API, which the stylesheet's reduced-motion rule cannot reach, so without this a
listener list would still slide for someone who asked the system to stop that. Checked at each
transition, so changing the setting takes effect without a reload.
*/
import { flip as svelteFlip } from 'svelte/animate';
import { fade as svelteFade, fly as svelteFly } from 'svelte/transition';

function reduced(): boolean {
  return typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;
}

export const fly: typeof svelteFly = (node, params) => svelteFly(node, reduced() ? { ...params, duration: 0 } : params);
export const fade: typeof svelteFade = (node, params) => svelteFade(node, reduced() ? { ...params, duration: 0 } : params);
export const flip: typeof svelteFlip = (node, rects, params) =>
  svelteFlip(node, rects, reduced() ? { ...params, duration: 0 } : params);
