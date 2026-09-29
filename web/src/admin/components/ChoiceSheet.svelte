<script lang="ts" generics="T extends string | number">
	import Check from '@lucide/svelte/icons/check';
	import type { Snippet } from 'svelte';
	import * as Dialog from './ui/dialog/index';

	/*
	Picking one value from a few that make sense, rather than dragging a slider to an arbitrary
	number. A sheet from the bottom on a phone, a small dialog on a larger screen; choosing closes
	it, because a choice is the whole task.
	*/
	let {
		open = $bindable(false),
		title,
		description,
		options,
		value,
		onChange,
		extra
	}: {
		open?: boolean;
		title: string;
		description?: string;
		options: { value: T; label: string; detail?: string }[];
		value: T;
		onChange: (value: T) => void;
		extra?: Snippet;
	} = $props();
</script>

<Dialog.Root bind:open>
	<Dialog.Content class="gap-3 rounded-3xl p-3 pt-5 sm:max-w-md">
		<div class="flex flex-col gap-1 px-3 pr-12">
			<Dialog.Title class="text-lg font-semibold">{title}</Dialog.Title>
			{#if description}<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">{description}</Dialog.Description>{/if}
		</div>
		<!-- Buttons, not a radio group: picking one applies it and closes the sheet, so there is no
		moving between options with the arrow keys first. The current one is marked as such. -->
		<ul class="flex flex-col" aria-label={title}>
			{#each options as option (option.value)}
				{@const active = option.value === value}
				<li>
					<button
						type="button"
						aria-current={active ? 'true' : undefined}
						class="flex min-h-13 w-full items-center gap-3 rounded-xl px-3 py-2 text-left transition-colors hover:bg-accent/60"
						onclick={() => {
							onChange(option.value);
							open = false;
						}}
					>
						<span class="flex min-w-0 flex-1 flex-col">
							<span class="text-[15px] {active ? 'font-semibold' : ''}">{option.label}</span>
							{#if option.detail}<span class="text-[13px] text-muted-foreground">{option.detail}</span>{/if}
						</span>
						{#if active}<Check size={18} class="shrink-0" aria-hidden="true" />{/if}
					</button>
				</li>
			{/each}
		</ul>
		{#if extra}<div class="px-3 pb-2">{@render extra()}</div>{/if}
	</Dialog.Content>
</Dialog.Root>
