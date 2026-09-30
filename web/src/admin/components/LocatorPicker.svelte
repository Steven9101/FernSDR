<script lang="ts">
	import Crosshair from '@lucide/svelte/icons/crosshair';
	import Minus from '@lucide/svelte/icons/minus';
	import Plus from '@lucide/svelte/icons/plus';
	import { Button } from './ui/button/index';
	import * as Dialog from './ui/dialog/index';
	import { Input } from './ui/input/index';
	import { locatorCentre, locatorFor, normalLocator } from '../../util/locator';

	/*
	Finding the station's grid square without looking it up elsewhere: from this browser's position
	when the operator sits at the station, or by pointing at a map. The map is the coarse outline the
	listener page's day and night map uses, good to a few kilometres, which is finer than a subsquare
	only by luck; zoomed in, the squares and subsquares are drawn so the operator can see which one
	the station is in rather than trust the coastline.
	*/
	let {
		open = $bindable(false),
		grid,
		onPick
	}: { open?: boolean; grid: string; onPick: (grid: string) => void } = $props();

	let canvas = $state<HTMLCanvasElement | null>(null);
	let width = $state(0);
	let rings = $state.raw<number[][][] | null>(null);
	// The view: its centre and how many degrees of longitude one CSS pixel spans.
	let centre = $state({ lat: 20, lon: 0 });
	let scale = $state(0.5);
	let picked = $state<{ lat: number; lon: number } | null>(null);
	let locating = $state(false);
	let problem = $state('');

	// Typed rather than pointed at: the map takes a pointer, and a keyboard, or
	// an antenna somewhere other than this browser, needs another way in.
	let typed = $state('');
	const typedGrid = $derived(normalLocator(typed));

	const height = $derived(Math.round(Math.min(width * 0.6, 420)));
	const choice = $derived(typedGrid || (picked ? locatorFor(picked.lat, picked.lon) : ''));

	function typeGrid(value: string) {
		typed = value;
		const here = locatorCentre(value);
		if (!here) return;
		picked = here;
		centre = here;
		// A subsquare close enough that the small squares are drawn and named.
		scale = value.trim().length === 6 ? MIN_SCALE : 0.02;
		clampCentre();
	}

	$effect(() => {
		if (!open) return;
		let cancelled = false;
		import('../../data/land').then((module) => {
			if (!cancelled) rings = module.landRings();
		});
		// Opened on the square already set, close enough to see its neighbours.
		const here = locatorCentre(grid);
		picked = here;
		typed = '';
		problem = '';
		if (here) {
			centre = here;
			scale = 0.02;
		} else {
			centre = { lat: 20, lon: 0 };
			scale = 360 / Math.max(width, 320);
		}
		return () => {
			cancelled = true;
		};
	});

	const MIN_SCALE = 0.002;
	const maxScale = () => 360 / Math.max(width, 320);

	function zoom(factor: number, at?: { x: number; y: number }) {
		const next = Math.min(maxScale(), Math.max(MIN_SCALE, scale * factor));
		if (at) {
			// Keep the point under the pointer where it is.
			const lon = centre.lon + (at.x - width / 2) * scale;
			const lat = centre.lat - (at.y - height / 2) * scale;
			centre = { lon: lon - (at.x - width / 2) * next, lat: lat + (at.y - height / 2) * next };
		}
		scale = next;
		clampCentre();
	}

	function clampCentre() {
		const halfW = (width / 2) * scale;
		const halfH = (height / 2) * scale;
		centre = {
			lon: Math.min(180 - halfW, Math.max(-180 + halfW, centre.lon)),
			lat: Math.min(90 - halfH, Math.max(-90 + halfH, centre.lat))
		};
		if (halfW >= 180) centre = { ...centre, lon: 0 };
		if (halfH >= 90) centre = { ...centre, lat: 0 };
	}

	function token(name: string, fallback: string): string {
		const value = canvas ? getComputedStyle(canvas).getPropertyValue(name).trim() : '';
		return value || fallback;
	}

	$effect(() => {
		const element = canvas;
		const land = rings;
		const view = { ...centre, scale, width, height };
		const mark = picked;
		if (!element || !land || view.width <= 0) return;
		const ratio = window.devicePixelRatio || 1;
		element.width = Math.round(view.width * ratio);
		element.height = Math.round(view.height * ratio);
		const context = element.getContext('2d');
		if (!context) return;
		context.setTransform(ratio, 0, 0, ratio, 0, 0);
		const toX = (lon: number) => view.width / 2 + (lon - view.lon) / view.scale;
		const toY = (lat: number) => view.height / 2 - (lat - view.lat) / view.scale;

		context.fillStyle = token('--background', '#111');
		context.fillRect(0, 0, view.width, view.height);
		context.fillStyle = token('--accent', '#444');
		for (const ring of land) {
			context.beginPath();
			ring.forEach(([x, y], index) => {
				const px = toX(x / 2 - 180);
				const py = toY(90 - y / 2);
				if (index) context.lineTo(px, py);
				else context.moveTo(px, py);
			});
			context.closePath();
			context.fill();
		}

		// The grid at the finest level that is still legible: fields, squares, subsquares.
		const levels = [
			{ lon: 20, lat: 10, label: (lon: number, lat: number) => locatorFor(lat + 5, lon + 10).slice(0, 2) },
			{ lon: 2, lat: 1, label: (lon: number, lat: number) => locatorFor(lat + 0.5, lon + 1, 4) },
			{ lon: 5 / 60, lat: 2.5 / 60, label: (lon: number, lat: number) => locatorFor(lat + 1.25 / 60, lon + 2.5 / 60).slice(4) }
		];
		const level = [...levels].reverse().find((l) => l.lon / view.scale >= 28) ?? levels[0];
		const west = view.lon - (view.width / 2) * view.scale;
		const east = view.lon + (view.width / 2) * view.scale;
		const south = view.lat - (view.height / 2) * view.scale;
		const north = view.lat + (view.height / 2) * view.scale;
		context.strokeStyle = token('--border', 'rgba(128,128,128,0.3)');
		context.lineWidth = 1;
		context.beginPath();
		const firstLon = Math.max(-180, Math.floor((west + 180) / level.lon) * level.lon - 180);
		const firstLat = Math.max(-90, Math.floor((south + 90) / level.lat) * level.lat - 90);
		for (let lon = firstLon; lon <= Math.min(180, east); lon += level.lon) {
			context.moveTo(Math.round(toX(lon)) + 0.5, 0);
			context.lineTo(Math.round(toX(lon)) + 0.5, view.height);
		}
		for (let lat = firstLat; lat <= Math.min(90, north); lat += level.lat) {
			context.moveTo(0, Math.round(toY(lat)) + 0.5);
			context.lineTo(view.width, Math.round(toY(lat)) + 0.5);
		}
		context.stroke();
		if (level.lon / view.scale >= 40 && level.lat / view.scale >= 16) {
			context.fillStyle = token('--muted-foreground', '#999');
			context.font = '11px ui-sans-serif, system-ui, sans-serif';
			for (let lon = firstLon; lon < Math.min(180, east); lon += level.lon) {
				for (let lat = firstLat; lat < Math.min(90, north); lat += level.lat) {
					context.fillText(level.label(lon, lat), toX(lon) + 4, toY(lat + level.lat) + 13);
				}
			}
		}

		if (mark) {
			// The subsquare the point is in, outlined, and the point itself.
			const cell = locatorCentre(locatorFor(mark.lat, mark.lon))!;
			const w = 5 / 60 / view.scale;
			const h = 2.5 / 60 / view.scale;
			context.strokeStyle = token('--signal', '#e8743b');
			context.lineWidth = 2;
			if (w >= 6) context.strokeRect(toX(cell.lon) - w / 2, toY(cell.lat) - h / 2, w, h);
			context.fillStyle = token('--signal', '#e8743b');
			context.beginPath();
			context.arc(toX(mark.lon), toY(mark.lat), 5, 0, 2 * Math.PI);
			context.fill();
		}
	});

	// A tap picks; a drag moves the map. Told apart by how far the pointer went.
	let drag: { x: number; y: number; lon: number; lat: number; moved: boolean } | null = null;

	function pointerDown(event: PointerEvent) {
		canvas?.setPointerCapture(event.pointerId);
		drag = { x: event.offsetX, y: event.offsetY, lon: centre.lon, lat: centre.lat, moved: false };
	}

	function pointerMove(event: PointerEvent) {
		if (!drag) return;
		const dx = event.offsetX - drag.x;
		const dy = event.offsetY - drag.y;
		if (!drag.moved && Math.hypot(dx, dy) < 5) return;
		drag.moved = true;
		centre = { lon: drag.lon - dx * scale, lat: drag.lat + dy * scale };
		clampCentre();
	}

	function pointerUp(event: PointerEvent) {
		if (drag && !drag.moved) {
			typed = '';
			picked = {
				lon: Math.max(-180, Math.min(180, centre.lon + (event.offsetX - width / 2) * scale)),
				lat: Math.max(-90, Math.min(90, centre.lat - (event.offsetY - height / 2) * scale))
			};
		}
		drag = null;
	}

	function wheel(event: WheelEvent) {
		event.preventDefault();
		zoom(event.deltaY > 0 ? 1.4 : 1 / 1.4, { x: event.offsetX, y: event.offsetY });
	}

	function locate() {
		if (!navigator.geolocation) {
			problem = 'This browser cannot tell where it is.';
			return;
		}
		locating = true;
		problem = '';
		navigator.geolocation.getCurrentPosition(
			(position) => {
				locating = false;
				typed = '';
				picked = { lat: position.coords.latitude, lon: position.coords.longitude };
				centre = picked;
				// Close enough that the subsquares are drawn and named.
				scale = MIN_SCALE;
				clampCentre();
			},
			(error) => {
				locating = false;
				problem =
					error.code === error.PERMISSION_DENIED
						? 'The browser was not allowed to say where it is.'
						: 'The browser could not find where it is.';
			},
			{ enableHighAccuracy: true, timeout: 15000, maximumAge: 60000 }
		);
	}
</script>

<Dialog.Root bind:open>
	<Dialog.Content class="gap-3 rounded-3xl p-3 pt-5 sm:max-w-2xl">
		<div class="flex flex-col gap-1 px-3 pr-12">
			<Dialog.Title class="text-lg font-semibold">Find the grid square</Dialog.Title>
			<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">
				Tap where the antenna is, or type its square. Drag to move, zoom until the small squares show, and
				check the one it is in.
			</Dialog.Description>
		</div>
		<div class="relative overflow-hidden rounded-2xl" bind:clientWidth={width}>
			<canvas
				bind:this={canvas}
				class="block w-full cursor-crosshair touch-none"
				style="height: {height}px"
				aria-label="World map for choosing the grid square{choice ? `, ${choice} chosen` : ''}"
				onpointerdown={pointerDown}
				onpointermove={pointerMove}
				onpointerup={pointerUp}
				onpointercancel={() => (drag = null)}
				onwheel={wheel}
			></canvas>
			<div class="absolute top-2 right-2 flex flex-col gap-1">
				<Button variant="secondary" size="icon-lg" class="rounded-full" aria-label="Zoom in" onclick={() => zoom(1 / 2)}><Plus /></Button>
				<Button variant="secondary" size="icon-lg" class="rounded-full" aria-label="Zoom out" onclick={() => zoom(2)}><Minus /></Button>
			</div>
		</div>
		{#if problem}<p class="px-3 text-sm text-destructive" role="alert">{problem}</p>{/if}
		<div class="flex flex-wrap items-center gap-2 px-1 pb-1">
			<Button variant="secondary" class="h-11 rounded-full px-4 md:h-9" disabled={locating} onclick={locate}>
				<Crosshair /> {locating ? 'Finding…' : 'Where this browser is'}
			</Button>
			<Input class="h-11 w-32 rounded-full px-4 md:h-9" value={typed} maxlength={6} autocomplete="off" spellcheck="false"
				placeholder="Or type it" aria-label="Grid square, such as JO31 or JO31ki"
				aria-invalid={typed.trim() !== '' && !typedGrid}
				oninput={(event) => typeGrid(event.currentTarget.value)} />
			<span class="flex-1"></span>
			<span class="text-[15px] tabular text-muted-foreground" aria-live="polite">
				{#if picked}{Math.abs(picked.lat).toFixed(3)}° {picked.lat >= 0 ? 'N' : 'S'}, {Math.abs(picked.lon).toFixed(3)}° {picked.lon >= 0 ? 'E' : 'W'}{/if}
			</span>
			<Button class="h-11 rounded-full px-4 md:h-9" disabled={!choice} onclick={() => { onPick(choice); open = false; }}>
				{choice ? `Use ${choice}` : 'Tap the map'}
			</Button>
		</div>
	</Dialog.Content>
</Dialog.Root>
