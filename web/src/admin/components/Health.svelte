<script lang="ts">
	import { bandCondition, live, quietSummary } from '../lib/live.svelte';

	/*
	The receiver's state in a line, on every page: the first thing that should change when
	something breaks, and the only thing here that ever turns red.
	*/
	const summary = $derived.by(() => {
		const bands = live.state?.bands ?? [];
		if (!live.state) return { tone: 'text-muted-foreground', text: live.error ? 'Receiver unreachable' : 'Connecting' };
		if (live.error) return { tone: 'text-destructive', text: 'Receiver unreachable' };
		const conditions = bands.map((band) => bandCondition(band));
		const needs = conditions.filter((c) => c === 'needs-you' || c === 'stopped').length;
		const retrying = conditions.filter((c) => c === 'retrying' || c === 'starting' || c === 'restarting').length;
		if (needs) return { tone: 'text-destructive', text: `${needs} band${needs === 1 ? ' needs' : 's need'} you` };
		if (retrying) return { tone: 'text-warning', text: `${retrying} band${retrying === 1 ? '' : 's'} coming back` };
		return { tone: 'text-muted-foreground', text: quietSummary(conditions) };
	});
</script>

<span class="truncate text-sm {summary.tone}" role="status">{summary.text}</span>
