<script lang="ts">
	import { parseNotes, type Span } from '../lib/release-notes';

	/*
	A release's notes, set as the changelog means them: section headings, lists and paragraphs. Every
	piece is a text node; nothing in the notes can become markup on this page.
	*/
	let { notes }: { notes: string } = $props();
	const blocks = $derived(parseNotes(notes));
</script>

{#snippet inline(spans: Span[])}
	{#each spans as span, i (i)}{#if span.code}<code class="rounded-md bg-muted px-1 py-px text-[0.92em]">{span.text}</code>{:else if span.strong}<strong class="font-semibold text-foreground">{span.text}</strong>{:else if span.em}<em>{span.text}</em>{:else}{span.text}{/if}{/each}
{/snippet}

<div class="flex flex-col gap-3 text-[14px] leading-relaxed text-muted-foreground">
	{#each blocks as block, i (i)}
		{#if block.kind === 'heading'}
			<h3 class="pt-1 text-[13px] font-semibold text-foreground first:pt-0">{block.text}</h3>
		{:else if block.kind === 'list'}
			<ul class="flex flex-col gap-2">
				{#each block.items as item, j (j)}
					<li class="relative pl-4 before:absolute before:top-[0.6em] before:left-0.5 before:size-1.5 before:rounded-full before:bg-muted-foreground/50">
						{@render inline(item)}
					</li>
				{/each}
			</ul>
		{:else}
			<p>{@render inline(block.spans)}</p>
		{/if}
	{/each}
</div>
