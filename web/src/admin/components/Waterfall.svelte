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

	const WIDTH = 512;
	const TRACE = 64;
	const ROWS = 116;
	const HEIGHT = TRACE + ROWS;
	const INTERVAL_MS = 500;

	let hero: HTMLCanvasElement | undefined = $state();
	let range = $state<{ low: number; high: number } | null>(null);
	let empty = $state(false);

	onMount(() => {
		const history = document.createElement('canvas');
		history.width = WIDTH;
		history.height = ROWS;
		const past = history.getContext('2d')!;
		past.fillStyle = 'rgb(6, 10, 18)';
		past.fillRect(0, 0, WIDTH, ROWS);
		const row = past.createImageData(WIDTH, 1);

		const frame = document.createElement('canvas');
		frame.width = WIDTH;
		frame.height = HEIGHT;
		const out = frame.getContext('2d')!;

		let active = true;
		let timer = 0;

		// The waterfall: the old lines move down one, the new one goes on top.
		const push = (levels: number[], floor: number) => {
			past.drawImage(history, 0, 0, WIDTH, ROWS - 1, 0, 1, WIDTH, ROWS - 1);
			for (let x = 0; x < WIDTH; x++) {
				const level = levels[Math.min(levels.length - 1, Math.floor((x * levels.length) / WIDTH))];
				// Against the band's own floor, so a quiet band and a loud one both spend the
				// palette on what stands above the noise.
				const [r, g, b] = samplePalette('aurora', Math.min(1, Math.max(0, (level - floor + 4) / 44)));
				row.data[x * 4] = r;
				row.data[x * 4 + 1] = g;
				row.data[x * 4 + 2] = b;
				row.data[x * 4 + 3] = 255;
			}
			past.putImageData(row, 0, 0);
		};

		const draw = (levels: number[], floor: number) => {
			push(levels, floor);
			out.fillStyle = 'rgb(6, 10, 18)';
			out.fillRect(0, 0, WIDTH, HEIGHT);
			out.drawImage(history, 0, TRACE);

			const top = Math.max(floor + 40, Math.max(...levels) + 2);
			const bottom = floor - 8;
			const y = (level: number) => TRACE - 2 - ((Math.min(top, Math.max(bottom, level)) - bottom) / (top - bottom)) * (TRACE - 6);
			out.beginPath();
			for (let x = 0; x < WIDTH; x++) {
				const level = levels[Math.min(levels.length - 1, Math.floor((x * levels.length) / WIDTH))];
				if (x === 0) out.moveTo(x, y(level));
				else out.lineTo(x, y(level));
			}
			out.save();
			out.lineTo(WIDTH, TRACE);
			out.lineTo(0, TRACE);
			out.closePath();
			const fill = out.createLinearGradient(0, 0, 0, TRACE);
			fill.addColorStop(0, 'rgba(120, 220, 200, 0.28)');
			fill.addColorStop(1, 'rgba(120, 220, 200, 0.02)');
			out.fillStyle = fill;
			out.fill();
			out.restore();
			out.beginPath();
			for (let x = 0; x < WIDTH; x++) {
				const level = levels[Math.min(levels.length - 1, Math.floor((x * levels.length) / WIDTH))];
				if (x === 0) out.moveTo(x, y(level));
				else out.lineTo(x, y(level));
			}
			out.strokeStyle = 'rgba(210, 250, 240, 0.9)';
			out.lineWidth = 1.2;
			out.stroke();

			hero?.getContext('2d')?.drawImage(frame, 0, 0, WIDTH, HEIGHT);
		};

		let first = true;
		const tick = async () => {
			if (!active) return;
			if (!document.hidden && receiving) {
				try {
					// The first ask also brings the last minute the band kept, so the waterfall
					// opens on the recent past rather than filling for a minute. Only if it is
					// current: lines from before a band stopped are not drawn as if live.
					const line = await api.spectrum(band, WIDTH, first);
					first = false;
					if (line.levels.length > 0 && line.low !== undefined && line.high !== undefined) {
						empty = false;
						range = { low: line.low, high: line.high };
						const floor = line.floor !== undefined && line.floor > -159 ? line.floor : Math.min(...line.levels);
						const recent = line.recent;
						if (recent && recent.count > 0 && recent.age_ms >= 0 && recent.age_ms < 2000 && recent.interval_ms === INTERVAL_MS) {
							for (const older of decodeRecent(recent).slice(-ROWS)) push(older, floor);
						}
						draw(line.levels, floor);
					} else {
						empty = true;
						range = null;
						const context = hero?.getContext('2d');
						if (context) {
							context.fillStyle = 'rgb(6, 10, 18)';
							context.fillRect(0, 0, WIDTH, HEIGHT);
						}
					}
				} catch {
					// The next tick tries again; the page says elsewhere when the receiver is away.
				}
			}
			timer = window.setTimeout(tick, INTERVAL_MS);
		};
		void tick();
		return () => {
			active = false;
			window.clearTimeout(timer);
		};
	});

	const labels = $derived(
		range ? [range.low, (range.low + range.high) / 2, range.high].map((hz) => (hz / 1e6).toFixed(3)) : []
	);
</script>

<figure class="flex flex-col gap-2">
	<div
		class="relative h-40 overflow-hidden rounded-3xl bg-[rgb(6,10,18)] ring-1 ring-white/5 md:h-60"
		role="img"
		aria-label={receiving ? "The band's spectrum and waterfall, live" : 'The band is not receiving'}
	>
		<canvas
			bind:this={hero}
			width={WIDTH}
			height={HEIGHT}
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
