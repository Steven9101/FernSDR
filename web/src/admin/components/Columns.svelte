<script lang="ts">
	import type { Snippet } from 'svelte';

	/*
	A page's working column and, from laptop width up, a narrower one beside it for what the
	operator glances at while working there: live figures, a preview, the facts this page cannot
	change. Settings keep a readable width however wide the screen is; the room a wide screen adds
	goes to that context instead of to longer rows. Below laptop width the two stack, the aside
	after the work unless it is what tells the operator whether the rest matters.

	The aside comes first in the document when it leads on a phone, and is only placed on the
	right by the grid, so reading order and tab order follow what a phone shows. Beside the work it
	stays in view while the page scrolls, and scrolls on its own when it is taller than the window,
	so a long list of listeners is never out of reach.
	*/
	let {
		main,
		aside,
		asideFirst = false,
		asideLabel
	}: { main: Snippet; aside: Snippet; asideFirst?: boolean; asideLabel: string } = $props();
</script>

{#snippet side()}
	<aside
		aria-label={asideLabel}
		class="flex min-w-0 flex-col gap-8 lg:sticky lg:top-8 lg:col-start-2 lg:row-start-1 lg:max-h-[calc(100dvh-4rem)] lg:self-start lg:overflow-y-auto lg:overscroll-contain lg:pb-2"
	>
		{@render aside()}
	</aside>
{/snippet}

<div class="grid grid-cols-1 gap-8 lg:grid-cols-[minmax(0,1fr)_clamp(16rem,27vw,21rem)] lg:gap-10 xl:gap-14">
	{#if asideFirst}{@render side()}{/if}
	<div class="flex min-w-0 max-w-2xl flex-col gap-8 lg:col-start-1 lg:row-start-1">
		{@render main()}
	</div>
	{#if !asideFirst}{@render side()}{/if}
</div>
