<script lang="ts">
	import type { Component, Snippet } from 'svelte';

	let {
		icon: Icon,
		title,
		children
	}: { icon?: Component<{ size?: number; class?: string }>; title: string; children?: Snippet } = $props();
</script>

<div class="flex flex-col items-center gap-2 rounded-3xl border border-dashed border-border px-6 py-10 text-center">
	{#if Icon}<Icon size={22} class="mb-1 text-muted-foreground" />{/if}
	<p class="font-medium">{title}</p>
	{#if children}<p class="max-w-sm text-sm leading-relaxed text-muted-foreground">{@render children()}</p>{/if}
</div>
