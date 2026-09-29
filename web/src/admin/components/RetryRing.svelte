<script lang="ts">
	/* When the receiver tries a failed input again, counted down in whole seconds. */
	let { seconds }: { seconds: number } = $props();

	let remaining = $state(0);

	$effect(() => {
		const start = performance.now();
		const from = Math.max(0, seconds);
		remaining = from;
		const timer = window.setInterval(() => {
			remaining = Math.max(0, from - (performance.now() - start) / 1000);
			if (remaining <= 0) window.clearInterval(timer);
		}, 250);
		return () => window.clearInterval(timer);
	});
</script>

<span class="text-sm text-muted-foreground tabular">
	{remaining > 0 ? `Next try in ${Math.ceil(remaining)} s` : 'Trying again now'}
</span>
