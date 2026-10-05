<script lang="ts">
	/*
	A minute or so of one number, as a line. Drawn rather than charted: it is sixty points in a
	small box, and the question it answers is the shape (steady, climbing, a sudden jump), not a
	value to read off an axis.
	*/
	let {
		values,
		width = 120,
		height = 32,
		class: className = 'text-foreground',
		label,
		floor,
		ceiling
	}: { values: number[]; width?: number; height?: number; class?: string; label: string; floor?: number; ceiling?: number } = $props();

	const id = `spark-${Math.random().toString(36).slice(2)}`;

	const shape = $derived.by(() => {
		if (values.length < 2) return null;
		// A share of something has a scale of its own: a CPU wandering between 1% and 2% is flat,
		// not a mountain range, so the caller can pin the ends the line is drawn against.
		const low = Math.min(...values, floor ?? Infinity);
		const high = Math.max(...values, ceiling ?? -Infinity);
		// A flat series sits in the middle rather than on the floor, so flat reads as flat.
		const span = high - low || 1;
		const pad = 3;
		const step = (width - pad * 2) / (values.length - 1);
		const points = values.map((value, index) => ({
			x: pad + index * step,
			y: high === low ? height / 2 : pad + (1 - (value - low) / span) * (height - pad * 2)
		}));
		const line = points.map((point, index) => `${index ? 'L' : 'M'}${point.x.toFixed(1)},${point.y.toFixed(1)}`).join(' ');
		const last = points[points.length - 1];
		return {
			line,
			area: `${line} L${last.x.toFixed(1)},${height} L${points[0].x.toFixed(1)},${height} Z`,
			last,
			flat: high === low
		};
	});
</script>

<svg
	viewBox="0 0 {width} {height}"
	{width}
	{height}
	class={className}
	role="img"
	aria-label={label}
>
	{#if shape}
		<defs>
			<linearGradient id={id} x1="0" y1="0" x2="0" y2="1">
				<stop offset="0%" stop-color="currentColor" stop-opacity="0.22" />
				<stop offset="100%" stop-color="currentColor" stop-opacity="0" />
			</linearGradient>
		</defs>
		{#if !shape.flat}<path d={shape.area} fill="url(#{id})" />{/if}
		<path
			d={shape.line}
			fill="none"
			stroke="currentColor"
			stroke-opacity={shape.flat ? 0.35 : 1}
			stroke-width="1.5"
			stroke-linejoin="round"
			stroke-linecap="round"
			vector-effect="non-scaling-stroke"
		/>
	{:else}
		<line
			x1="3"
			x2={width - 3}
			y1={height / 2}
			y2={height / 2}
			stroke="currentColor"
			stroke-opacity="0.2"
			stroke-dasharray="2 4"
		/>
	{/if}
</svg>
