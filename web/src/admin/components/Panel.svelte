<script lang="ts">
	import type { Snippet } from 'svelte';
	import { cn } from '../lib/utils';

	let {
		title,
		description,
		trailing,
		children,
		class: className,
		id
	}: {
		title?: string;
		description?: string;
		trailing?: Snippet;
		children: Snippet;
		class?: string;
		id?: string;
	} = $props();
</script>

<section {id} class={cn('flex flex-col gap-5 rounded-3xl bg-card p-5 md:p-7', className)}>
	{#if title || trailing}
		<div class="flex items-start justify-between gap-4">
			<div class="flex min-w-0 flex-col gap-1">
				{#if title}<h2 class="text-lg font-semibold md:text-xl">{title}</h2>{/if}
				{#if description}<p class="text-sm leading-relaxed text-muted-foreground">{description}</p>{/if}
			</div>
			{#if trailing}<div class="shrink-0">{@render trailing()}</div>{/if}
		</div>
	{/if}
	{@render children()}
</section>
