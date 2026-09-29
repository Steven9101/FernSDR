<script lang="ts">
	import Monitor from '@lucide/svelte/icons/monitor';
	import Moon from '@lucide/svelte/icons/moon';
	import Sun from '@lucide/svelte/icons/sun';
	import { appearance, type AppearanceChoice } from '../lib/appearance.svelte';

	const order: AppearanceChoice[] = ['system', 'light', 'dark'];
	const names: Record<AppearanceChoice, string> = {
		system: 'Appearance follows the system',
		light: 'Light appearance',
		dark: 'Dark appearance'
	};
	const next = $derived(order[(order.indexOf(appearance.choice) + 1) % order.length]);
</script>

<button
	type="button"
	class="pressable grid size-10 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
	title="{names[appearance.choice]}. Switch to: {names[next].toLowerCase()}"
	aria-label="{names[appearance.choice]}. Switch to: {names[next].toLowerCase()}"
	onclick={() => appearance.set(next)}
>
	{#if appearance.choice === 'system'}<Monitor size={18} />{:else if appearance.choice === 'light'}<Sun size={18} />{:else}<Moon size={18} />{/if}
</button>
