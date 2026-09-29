<script lang="ts">
	import { conditionLabel, type BandCondition } from '../lib/live.svelte';
	import { cn } from '../lib/utils';

	/*
	A band's condition as a word. Colour only when something is wrong: amber while it is being
	retried, red when it needs the operator; the normal case stays quiet.
	*/
	let { condition, class: className }: { condition: BandCondition; class?: string } = $props();

	const tone = $derived(
		condition === 'retrying' || condition === 'starting' || condition === 'restarting'
			? 'text-warning'
			: condition === 'needs-you' || condition === 'stopped'
				? 'text-destructive'
				: 'text-muted-foreground'
	);
</script>

<span class={cn('text-sm font-medium', tone, className)}>{conditionLabel[condition]}</span>
