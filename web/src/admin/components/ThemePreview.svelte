<script lang="ts">
	import { paletteGradient, type PaletteId } from '../../render/palettes';
	import { live } from '../lib/live.svelte';
	import { resolvedColors, safeImageUrl, type Theme } from '../lib/theme';
	import { Segmented } from './ui/segmented/index';

	/*
	The receiver in miniature, painted with the theme being edited: the colours land where the
	receiver uses them, the waterfall shows the colour map, and the meter is the style chosen. The
	receiver has a light and a dark scheme and listeners pick; a colour left unset follows each, so
	both can be looked at here. The frequency and the meter are examples; the listener count is real.
	*/
	let {
		theme,
		station,
		scheme = $bindable('dark')
	}: {
		theme: Theme;
		station?: { name?: string; operator?: string; location?: string };
		scheme?: 'dark' | 'light';
	} = $props();
	const c = $derived(resolvedColors(theme, scheme));
	const image = $derived(safeImageUrl(theme.background?.image));
	const opacity = $derived(Math.min(1, Math.max(0, theme.background?.opacity ?? 0.35)));
	const blur = $derived(Math.min(40, Math.max(0, theme.background?.blur ?? 0)));
	const tile = $derived(theme.background?.position === 'tile');
	const meter = $derived(theme.meter ?? 'bar');
	const palette = $derived((theme.palette ?? 'classic') as PaletteId);
	// Over a picture the receiver's panels turn translucent so the picture shows through.
	const panel = $derived(image ? `color-mix(in srgb, ${c.card} 72%, transparent)` : c.card);
	// A trace of the last half minute for the history meter, the same every time.
	const trace = [0.42, 0.45, 0.4, 0.5, 0.58, 0.55, 0.62, 0.7, 0.66, 0.72, 0.69, 0.75, 0.71, 0.64, 0.68, 0.74, 0.8, 0.77];
</script>

<section class="flex flex-col gap-2" aria-label="Preview of the receiver">
	<div class="flex items-center justify-between gap-3 px-4">
		<h2 class="text-sm font-medium text-muted-foreground">Preview</h2>
		<Segmented
			label="Scheme to preview"
			class="w-36"
			options={[
				{ value: 'dark', label: 'Dark' },
				{ value: 'light', label: 'Light' }
			]}
			value={scheme}
			onChange={(value) => (scheme = value)}
		/>
	</div>

	<div
		data-preview
		class="relative isolate overflow-hidden rounded-2xl text-[12px] shadow-[0_0_0_1px_var(--border)]"
		style="background: {c.background}; color: {c.foreground}"
		aria-hidden="true"
	>
		{#if image}
			<div
				class="absolute -inset-4 -z-10"
				style="background-image: url('{image}'); background-size: {tile ? 'auto' : 'cover'}; background-repeat: {tile
					? 'repeat'
					: 'no-repeat'}; background-position: center; opacity: {opacity}; filter: {blur > 0 ? `blur(${blur / 2}px)` : 'none'}"
			></div>
		{/if}

		<div class="flex items-center gap-2 px-3 py-2.5" style="background: {panel}; border-bottom: 1px solid {c.border}">
			<span class="truncate font-semibold" style="font-size: 13px">{station?.name || 'Your receiver'}</span>
			{#if station?.operator}<span class="truncate" style="color: {c.mutedForeground}">{station.operator}</span>{/if}
			<span class="ml-auto rounded-md px-2 py-0.5 font-medium" style="background: {c.primary}; color: {c.primaryForeground}">Listen</span>
		</div>

		<div class="relative h-24" style="background: {paletteGradient(palette)}; border-bottom: 1px solid {c.border}">
			<span class="absolute top-0 bottom-0 left-[38%] w-px" style="background: {c.signal}"></span>
			<span class="absolute top-1.5 left-[38%] -translate-x-1/2 rounded px-1 text-[10px] font-medium" style="background: {c.signal}; color: {c.background}">
				7.074
			</span>
		</div>

		<div class="flex flex-col gap-2.5 px-3 py-3" style="background: {panel}">
			<div class="flex items-baseline justify-between">
				<span class="font-semibold tabular" style="font-size: 20px; letter-spacing: -0.02em">7.074.000</span>
				<span style="color: {c.mutedForeground}">kHz</span>
			</div>

			<div class="grid grid-cols-4 gap-0.5 rounded-lg p-0.5" style="background: {c.muted}">
				{#each ['LSB', 'USB', 'AM', 'FM'] as mode (mode)}
					<span
						class="rounded-md py-1 text-center font-medium"
						style={mode === 'USB' ? `background: ${c.elevated}; color: ${c.foreground}` : `color: ${c.mutedForeground}`}
					>
						{mode}
					</span>
				{/each}
			</div>

			<div class="flex items-center gap-2">
				<span class="w-12 shrink-0" style="color: {c.mutedForeground}">Signal</span>
				{#if meter === 'bar'}
					<span class="relative h-2 flex-1 overflow-hidden rounded-full" style="background: {c.muted}">
						<span class="absolute inset-y-0 left-0 w-[62%] rounded-full" style="background: {c.foreground}"></span>
						<span class="absolute inset-y-0 left-[74%] w-0.5" style="background: {c.signal}"></span>
					</span>
					<span class="w-8 text-right tabular">S7</span>
				{:else if meter === 'needle'}
					<svg viewBox="0 0 120 44" class="h-11 flex-1">
						<path d="M10 40 A50 50 0 0 1 110 40" fill="none" stroke={c.borderStrong} stroke-width="2" />
						<path d="M78 14 A50 50 0 0 1 110 40" fill="none" stroke={c.signal} stroke-width="2" />
						<line x1="60" y1="42" x2="84" y2="9" stroke={c.foreground} stroke-width="1.8" stroke-linecap="round" />
					</svg>
				{:else if meter === 'numeric'}
					<span class="flex flex-1 items-baseline gap-2">
						<span class="font-semibold tabular" style="font-size: 16px">S7</span>
						<span class="tabular" style="color: {c.mutedForeground}">&minus;87 dBm</span>
						<span class="ml-auto tabular" style="color: {c.signal}">peak S8</span>
					</span>
				{:else}
					<svg viewBox="0 0 {trace.length - 1} 1" preserveAspectRatio="none" class="h-8 flex-1">
						<polyline
							points={trace.map((level, index) => `${index},${1 - level}`).join(' ')}
							fill="none"
							stroke={c.signal}
							stroke-width="1.5"
							vector-effect="non-scaling-stroke"
						/>
					</svg>
				{/if}
			</div>

			<span style="color: {c.subtleForeground}">Drag the waterfall to tune</span>
		</div>

		<div class="flex gap-3 px-3 py-2" style="background: {panel}; border-top: 1px solid {c.border}; color: {c.mutedForeground}">
			<span style="color: {c.success}">Connected</span>
			<span class="truncate">{station?.location || 'Location'}</span>
			<span class="ml-auto tabular">{live.state?.users ?? 0} listening</span>
		</div>
	</div>
</section>
