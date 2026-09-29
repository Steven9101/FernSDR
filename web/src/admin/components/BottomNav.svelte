<script lang="ts">
	import Ellipsis from '@lucide/svelte/icons/ellipsis';
	import LogOut from '@lucide/svelte/icons/log-out';
	import { href, router, sections, type Page } from '../lib/router.svelte';
	import AppearanceToggle from './AppearanceToggle.svelte';
	import * as Dialog from './ui/dialog/index';

	let { onSignOut }: { onSignOut: () => void } = $props();

	// The four places an operator goes on a phone, and the rest behind More.
	const primary: Page[] = ['overview', 'bands', 'modules', 'log'];
	const tabs = sections.filter((section) => primary.includes(section.id));
	const rest = sections.filter((section) => !primary.includes(section.id));

	let more = $state(false);
	const activeIndex = $derived.by(() => {
		const index = tabs.findIndex((tab) => tab.id === router.route.page);
		return index === -1 ? tabs.length : index;
	});
</script>

<nav
	aria-label="Admin"
	data-bottom-nav
	class="vt-bottomnav fixed inset-x-0 bottom-0 z-40 px-3 pb-[max(0.625rem,env(safe-area-inset-bottom))] md:hidden"
>
	<div class="glass relative grid grid-cols-5 rounded-[1.625rem] p-1.5">
		<!-- One indicator gliding between tabs, so the eye can follow where you went. -->
		<span
			aria-hidden="true"
			class="absolute inset-y-1.5 left-1.5 w-[calc((100%-0.75rem)/5)] rounded-[1.25rem] bg-foreground/8 transition-transform duration-[560ms] ease-(--ease-page) dark:bg-foreground/12"
			style="transform: translateX({activeIndex * 100}%)"
		></span>
		{#each tabs as tab (tab.id)}
			{@const active = tab.id === router.route.page}
			<a
				href={href({ page: tab.id })}
				aria-current={active ? 'page' : undefined}
				class="pressable relative flex flex-col items-center gap-0.5 rounded-[1.25rem] py-1.5 text-[11px] font-medium transition-colors duration-300 {active
					? 'text-foreground'
					: 'text-muted-foreground'}"
			>
				<tab.icon size={22} strokeWidth={active ? 2.2 : 1.7} />
				<span>{tab.label}</span>
			</a>
		{/each}
		<button
			type="button"
			onclick={() => (more = true)}
			aria-haspopup="dialog"
			class="pressable relative flex flex-col items-center gap-0.5 rounded-[1.25rem] py-1.5 text-[11px] font-medium transition-colors duration-300 {activeIndex ===
			tabs.length
				? 'text-foreground'
				: 'text-muted-foreground'}"
		>
			<Ellipsis size={22} strokeWidth={activeIndex === tabs.length ? 2.2 : 1.7} />
			<span>More</span>
		</button>
	</div>
</nav>

<Dialog.Root bind:open={more}>
	<Dialog.Content class="rounded-3xl p-3 pt-4 sm:max-w-sm">
		<Dialog.Title class="px-3 text-base font-semibold">More</Dialog.Title>
		<div class="flex flex-col">
			{#each rest as section (section.id)}
				<a
					href={href({ page: section.id })}
					onclick={() => (more = false)}
					class="flex h-12 items-center gap-3 rounded-xl px-3 text-[15px] font-medium transition-colors hover:bg-accent {section.id ===
					router.route.page
						? 'bg-accent'
						: ''}"
				>
					<section.icon size={20} strokeWidth={1.75} />
					{section.label}
				</a>
			{/each}
		</div>
		<div class="flex items-center justify-between border-t border-border px-1 pt-2">
			<AppearanceToggle />
			<button
				type="button"
				class="pressable flex h-11 items-center gap-2 rounded-full px-3 text-sm text-muted-foreground hover:bg-accent hover:text-foreground"
				onclick={() => {
					more = false;
					onSignOut();
				}}
			>
				<LogOut size={17} /> Sign out
			</button>
		</div>
	</Dialog.Content>
</Dialog.Root>
