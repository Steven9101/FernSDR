<script lang="ts">
	import type { Snippet } from 'svelte';

	/*
	A group of settings the way the operator's own apps draw them: a quiet heading, one rounded
	surface of rows, and a line underneath for the one thing worth knowing about them. Explanation
	lives in that footer, not between controls.
	*/
	let {
		title,
		footer,
		children
	}: { title?: string; footer?: string | Snippet; children: Snippet } = $props();
</script>

<section data-settings-group class="flex flex-col gap-2">
	{#if title}
		<h2 class="px-4 text-sm font-medium text-muted-foreground">{title}</h2>
	{/if}
	<div class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
		{@render children()}
	</div>
	{#if footer}
		<div class="px-4 text-[13px] leading-relaxed text-muted-foreground">
			{#if typeof footer === 'string'}{footer}{:else}{@render footer()}{/if}
		</div>
	{/if}
</section>
