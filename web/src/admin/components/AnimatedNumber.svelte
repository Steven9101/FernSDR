<script lang="ts">
	import { Tween } from 'svelte/motion';
	import { cubicOut } from 'svelte/easing';

	/*
	A count that moves to its new value instead of jumping, so a change is seen as a change: twelve
	listeners becoming fourteen reads as two arriving. Short, and skipped for reduced motion.
	*/
	let {
		value,
		format = (n: number) => Math.round(n).toLocaleString()
	}: { value: number; format?: (value: number) => string } = $props();

	const reduced =
		typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;
	const tween = new Tween(0, { duration: reduced ? 0 : 700, easing: cubicOut });

	$effect(() => {
		tween.target = Number.isFinite(value) ? value : 0;
	});
</script>

<span class="tabular">{format(tween.current)}</span>
