<script lang="ts">
	import { onMount } from 'svelte';
	import { api } from '../api';
	import { cn } from '../lib/utils';

	/*
	What a band hears right now, as a line: carriers stand up out of the floor, a dead antenna is
	flat, and a noise source is a hump across the whole band. Refreshed every few seconds. It fills
	whatever width it is given; the stroke keeps its weight however far it is stretched.
	*/
	let { band, bins = 256, class: className }: { band: string; bins?: number; class?: string } = $props();

	const WIDTH = 1000;
	const HEIGHT = 100;
	let path = $state('');

	onMount(() => {
		let active = true;
		let timer = 0;
		const tick = async () => {
			if (!active) return;
			if (!document.hidden) {
				try {
					const line = await api.spectrum(band, bins);
					if (line.levels.length > 1) {
						const floor = line.floor !== undefined && line.floor > -159 ? line.floor : Math.min(...line.levels);
						const top = Math.max(floor + 30, Math.max(...line.levels));
						const bottom = floor - 6;
						const step = WIDTH / (line.levels.length - 1);
						path = line.levels
							.map((level, index) => {
								const y = HEIGHT - ((Math.min(top, Math.max(bottom, level)) - bottom) / (top - bottom)) * (HEIGHT - 4) - 2;
								return `${index ? 'L' : 'M'}${(index * step).toFixed(1)},${y.toFixed(1)}`;
							})
							.join(' ');
					}
				} catch {
					// Kept as it was; the page says elsewhere when the receiver is away.
				}
			}
			timer = window.setTimeout(tick, 3000);
		};
		void tick();
		return () => {
			active = false;
			window.clearTimeout(timer);
		};
	});
</script>

<svg
	viewBox="0 0 {WIDTH} {HEIGHT}"
	preserveAspectRatio="none"
	class={cn('block text-signal', className)}
	role="img"
	aria-label="The band's spectrum now"
>
	{#if path}
		<path d="{path} L{WIDTH},{HEIGHT} L0,{HEIGHT} Z" fill="currentColor" fill-opacity="0.12" />
		<path d={path} fill="none" stroke="currentColor" stroke-width="1.25" stroke-linejoin="round" vector-effect="non-scaling-stroke" />
	{/if}
</svg>
