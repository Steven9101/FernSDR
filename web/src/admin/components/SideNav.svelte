<script lang="ts">
	import LogOut from '@lucide/svelte/icons/log-out';
	import { live } from '../lib/live.svelte';
	import { href, router, sections } from '../lib/router.svelte';
	import AppearanceToggle from './AppearanceToggle.svelte';
	import Health from './Health.svelte';

	let { onSignOut }: { onSignOut: () => void } = $props();

	let list: HTMLDivElement | undefined = $state();
	let indicator = $state({ top: 0, height: 0, ready: false });

	// Measured rather than computed from an index: the list is not uniform once a label wraps
	// or the operator zooms, and an indicator a few pixels off its item looks like a bug.
	$effect(() => {
		const active = router.route.page;
		const element = list?.querySelector<HTMLElement>(`[data-section="${active}"]`);
		if (element) indicator = { top: element.offsetTop, height: element.offsetHeight, ready: true };
		// A page outside the navigation, such as the setup flow, marks nothing.
		else indicator = { ...indicator, height: 0 };
	});

</script>

<nav
	aria-label="Admin"
	class="vt-sidenav sticky top-0 hidden h-dvh w-60 shrink-0 flex-col gap-7 border-r border-sidebar-border bg-sidebar px-4 py-6 md:flex"
>
	<div class="flex flex-col gap-2 px-3">
		<a href="#/overview" class="truncate text-[1.5rem] leading-tight font-bold tracking-[-0.04em]">
			{live.state?.site ?? 'FernSDR'}
		</a>
		<Health />
	</div>

	<div class="relative flex flex-col gap-1" bind:this={list}>
		<span
			aria-hidden="true"
			class="absolute inset-x-0 rounded-xl bg-sidebar-accent {indicator.ready
				? 'transition-[transform,height] duration-[560ms] ease-(--ease-page)'
				: ''}"
			style="height: {indicator.height}px; transform: translateY({indicator.top}px)"
		></span>
		{#each sections as section (section.id)}
			{@const active = section.id === router.route.page}
			<a
				href={href({ page: section.id })}
				data-section={section.id}
				aria-current={active ? 'page' : undefined}
				class="group relative flex h-11 items-center gap-3 rounded-xl px-3 text-sm font-medium transition-colors {active
					? 'text-foreground'
					: 'text-muted-foreground hover:text-foreground'}"
			>
				<section.icon
					size={19}
					strokeWidth={active ? 2.25 : 1.75}
					class="transition-transform duration-300 ease-(--ease-spring) group-hover:scale-110"
				/>
				{section.label}
			</a>
		{/each}
	</div>

	<div class="mt-auto flex items-center justify-between px-1">
		<AppearanceToggle />
		<button
			type="button"
			class="pressable flex h-10 items-center gap-2 rounded-full px-3 text-sm text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
			onclick={onSignOut}
		>
			<LogOut size={17} /> Sign out
		</button>
	</div>
</nav>
