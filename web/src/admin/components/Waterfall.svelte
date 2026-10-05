<script lang="ts">
	import { onMount } from 'svelte';
	import { api } from '../api';
	import { decodeRecent } from '../lib/spectrum';
	import { samplePalette } from '../../render/palettes';

	/*
	The band itself, live, the way a receiver draws it: the spectrum now along the top, and below
	it the waterfall of the last minute or so, new lines arriving at the top. It shows whether the
	antenna hears anything, whether the noise blanker helped and where the carriers are. While the
	band is not receiving it stops and says so, rather than repeating the last line it had as if
	it were live.
	*/
	let { band, receiving = true }: { band: string; receiving?: boolean } = $props();

	// The canvas is drawn at the pixels the screen really has and the band is asked for as many
	// columns, up to what the receiver gives (2048): a fixed small canvas stretched to the card was
	// soft on a desktop and three times over on a phone. The waterfall keeps a minute of lines,
	// one every half second, stretched down the card's height; across, every column is its own.
	const MAX_BINS = 2048;
	const ROWS = 116;
	const TRACE_SHARE = 64 / 180;
	const INTERVAL_MS = 500;
	const GROUND = 'rgb(6, 10, 18)';

	let hero: HTMLCanvasElement | undefined = $state();
	let box: HTMLDivElement | undefined = $state();
	let range = $state<{ low: number; high: number } | null>(null);
	let empty = $state(false);

	onMount(() => {
		const history = document.createElement('canvas');
		const past = history.getContext('2d')!;
		let row = past.createImageData(1, 1);
		let columns = 0;

		// A new width keeps the minute already drawn, stretched to it, rather than starting empty.
		const setColumns = (next: number) => {
			if (next === columns) return;
			const before = columns > 0 ? past.getImageData(0, 0, columns, ROWS) : null;
			const old = document.createElement('canvas');
			if (before) {
				old.width = columns;
				old.height = ROWS;
				old.getContext('2d')!.putImageData(before, 0, 0);
			}
			history.width = next;
			history.height = ROWS;
			past.fillStyle = GROUND;
			past.fillRect(0, 0, next, ROWS);
			if (before) past.drawImage(old, 0, 0, next, ROWS);
			row = past.createImageData(next, 1);
			columns = next;
		};

		const size = () => {
			if (!hero || !box) return;
			const ratio = window.devicePixelRatio || 1;
			const width = Math.max(64, Math.round(box.clientWidth * ratio));
			const height = Math.max(64, Math.round(box.clientHeight * ratio));
			if (hero.width !== width || hero.height !== height) {
				hero.width = width;
				hero.height = height;
			}
			setColumns(Math.min(MAX_BINS, width));
		};
		size();

		let active = true;
		let timer = 0;
		let last: { levels: number[]; floor: number } | null = null;

		const level = (levels: number[], x: number, across: number) =>
			levels[Math.min(levels.length - 1, Math.floor((x * levels.length) / across))];

		// The waterfall: the old lines move down one, the new one goes on top.
		const push = (levels: number[], floor: number) => {
			past.drawImage(history, 0, 0, columns, ROWS - 1, 0, 1, columns, ROWS - 1);
			for (let x = 0; x < columns; x++) {
				// Against the band's own floor, so a quiet band and a loud one both spend the
				// palette on what stands above the noise.
				const [r, g, b] = samplePalette('aurora', Math.min(1, Math.max(0, (level(levels, x, columns) - floor + 4) / 44)));
				row.data[x * 4] = r;
				row.data[x * 4 + 1] = g;
				row.data[x * 4 + 2] = b;
				row.data[x * 4 + 3] = 255;
			}
			past.putImageData(row, 0, 0);
		};

		const draw = (levels: number[], floor: number) => {
			const out = hero?.getContext('2d');
			if (!hero || !out) return;
			const width = hero.width;
			const height = hero.height;
			const trace = Math.round(height * TRACE_SHARE);
			const ratio = window.devicePixelRatio || 1;
			out.fillStyle = GROUND;
			out.fillRect(0, 0, width, height);
			out.imageSmoothingEnabled = true;
			out.drawImage(history, 0, 0, columns, ROWS, 0, trace, width, height - trace);

			const top = Math.max(floor + 40, Math.max(...levels) + 2);
			const bottom = floor - 8;
			const y = (value: number) =>
				trace - 2 * ratio - ((Math.min(top, Math.max(bottom, value)) - bottom) / (top - bottom)) * (trace - 6 * ratio);
			const path = () => {
				out.beginPath();
				for (let x = 0; x < width; x++) {
					if (x === 0) out.moveTo(x, y(level(levels, x, width)));
					else out.lineTo(x, y(level(levels, x, width)));
				}
			};
			path();
			out.lineTo(width, trace);
			out.lineTo(0, trace);
			out.closePath();
			const fill = out.createLinearGradient(0, 0, 0, trace);
			fill.addColorStop(0, 'rgba(120, 220, 200, 0.28)');
			fill.addColorStop(1, 'rgba(120, 220, 200, 0.02)');
			out.fillStyle = fill;
			out.fill();
			path();
			out.strokeStyle = 'rgba(210, 250, 240, 0.9)';
			out.lineWidth = 1.2 * ratio;
			out.stroke();
		};

		let first = true;
		const tick = async () => {
			if (!active) return;
			if (!document.hidden && receiving) {
				try {
					// The first ask also brings the last minute the band kept, so the waterfall
					// opens on the recent past rather than filling for a minute. Only if it is
					// current: lines from before a band stopped are not drawn as if live.
					const line = await api.spectrum(band, columns, first);
					first = false;
					if (line.levels.length > 0 && line.low !== undefined && line.high !== undefined) {
						empty = false;
						range = { low: line.low, high: line.high };
						const floor = line.floor !== undefined && line.floor > -159 ? line.floor : Math.min(...line.levels);
						const recent = line.recent;
						if (recent && recent.count > 0 && recent.age_ms >= 0 && recent.age_ms < 2000 && recent.interval_ms === INTERVAL_MS) {
							for (const older of decodeRecent(recent).slice(-ROWS)) push(older, floor);
						}
						push(line.levels, floor);
						last = { levels: line.levels, floor };
						draw(line.levels, floor);
					} else {
						empty = true;
						range = null;
						last = null;
						const context = hero?.getContext('2d');
						if (context && hero) {
							context.fillStyle = GROUND;
							context.fillRect(0, 0, hero.width, hero.height);
						}
					}
				} catch {
					// The next tick tries again; the page says elsewhere when the receiver is away.
				}
			}
			timer = window.setTimeout(tick, INTERVAL_MS);
		};
		void tick();
		// A resize clears the canvas; the last line is drawn again at once instead of half a
		// second later.
		const observer = new ResizeObserver(() => {
			size();
			if (last) draw(last.levels, last.floor);
		});
		if (box) observer.observe(box);
		return () => {
			active = false;
			window.clearTimeout(timer);
			observer.disconnect();
		};
	});

	const labels = $derived(
		range ? [range.low, (range.low + range.high) / 2, range.high].map((hz) => (hz / 1e6).toFixed(3)) : []
	);
</script>

<figure class="flex flex-col gap-2">
	<div
		bind:this={box}
		class="relative h-40 overflow-hidden rounded-3xl bg-[rgb(6,10,18)] ring-1 ring-white/5 md:h-60"
		role="img"
		aria-label={receiving ? "The band's spectrum and waterfall, live" : 'The band is not receiving'}
	>
		<canvas
			bind:this={hero}
			class="absolute inset-0 h-full w-full transition-opacity duration-300 {receiving && !empty ? '' : 'opacity-25'}"
			aria-hidden="true"
		></canvas>
		{#if !receiving || empty}
			<p class="absolute inset-0 grid place-items-center text-sm font-medium text-white/80">
				{receiving ? 'No spectrum yet' : 'Not receiving'}
			</p>
		{/if}
	</div>
	{#if labels.length}
		<figcaption class="flex justify-between px-1 text-xs text-muted-foreground tabular">
			{#each labels as label, index (index)}<span>{label} MHz</span>{/each}
		</figcaption>
	{/if}
</figure>
