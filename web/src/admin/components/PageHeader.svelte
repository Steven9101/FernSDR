<script lang="ts">
	import type { Snippet } from 'svelte';

	let {
		title,
		description,
		actions
	}: { title: string; description?: string; actions?: Snippet } = $props();
</script>

<header class="flex flex-wrap items-end justify-between gap-x-6 gap-y-4 pt-2 pb-8 md:pt-6">
	<div class="flex min-w-0 flex-col gap-1">
		<h1 class="text-[1.875rem] leading-tight font-bold tracking-[-0.035em] md:text-[2.125rem]">
			{title}
		</h1>
		{#if description}
			<p class="max-w-2xl text-[15px] text-muted-foreground md:text-base">{description}</p>
		{/if}
	</div>
	{#if actions}
		<div class="flex shrink-0 flex-wrap items-center gap-2">{@render actions()}</div>
	{/if}
</header>
