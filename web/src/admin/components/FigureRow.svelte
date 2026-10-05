<script lang="ts">
	import type { Snippet } from 'svelte';
	import Sparkline from './Sparkline.svelte';

	/* One live figure in a group: what it is, what it is now, and how it went over the last minutes. */
	let {
		label,
		series,
		seriesLabel,
		tone,
		floor,
		ceiling,
		children
	}: {
		label: string;
		series?: number[];
		seriesLabel?: string;
		tone?: string;
		floor?: number;
		ceiling?: number;
		children: Snippet;
	} = $props();
</script>

<div class="flex items-center gap-3 px-4 py-3">
	<span class="flex min-w-0 flex-1 flex-col">
		<span class="text-[13px] text-muted-foreground">{label}</span>
		<span class="truncate text-[1.375rem] leading-tight font-semibold tracking-[-0.02em] tabular">{@render children()}</span>
	</span>
	{#if series}
		<Sparkline values={series} width={104} height={32} class={tone} {floor} {ceiling} label={seriesLabel ?? `${label} over the last few minutes`} />
	{/if}
</div>
