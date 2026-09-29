<script lang="ts">
	import ChevronRight from '@lucide/svelte/icons/chevron-right';
	import type { Snippet } from 'svelte';

	/*
	One row: what it is on the left, its value or control on the right. Given `onclick`, the whole
	row is the button that opens a choice, with a chevron saying so; otherwise it only shows.
	*/
	let {
		label,
		detail,
		value,
		onclick,
		control,
		tone = ''
	}: {
		label: string;
		detail?: string;
		value?: string;
		onclick?: () => void;
		control?: Snippet;
		tone?: string;
	} = $props();
</script>

{#snippet body()}
	<span class="flex min-w-0 flex-1 flex-col">
		<span class="text-[15px] {tone}">{label}</span>
		{#if detail}<span class="text-[13px] text-muted-foreground">{detail}</span>{/if}
	</span>
	{#if value !== undefined}
		<span class="max-w-[55%] truncate text-right text-[15px] text-muted-foreground tabular">{value}</span>
	{/if}
	{#if control}{@render control()}{/if}
{/snippet}

{#if onclick}
	<button
		type="button"
		{onclick}
		class="flex min-h-13 w-full items-center gap-3 px-4 py-2.5 text-left transition-colors hover:bg-accent/50 active:bg-accent"
	>
		{@render body()}
		<ChevronRight size={18} class="-mr-1 shrink-0 text-muted-foreground/70" />
	</button>
{:else}
	<div class="flex min-h-13 items-center gap-3 px-4 py-2.5">
		{@render body()}
	</div>
{/if}
