<script lang="ts">
	import type { Snippet } from 'svelte';
	import { Button } from './ui/button/index';
	import * as Dialog from './ui/dialog/index';

	/*
	Editing one or two values in a sheet: the row it came from stays tidy, and the sheet has room
	for the field, its unit and one line about what it does. Saving closes it; a save that fails
	says why here, from the error `onSave` throws, and leaves the sheet open.
	*/
	let {
		open = $bindable(false),
		title,
		description,
		action = 'Save',
		disabled = false,
		onSave,
		children
	}: {
		open?: boolean;
		title: string;
		description?: string;
		action?: string;
		disabled?: boolean;
		onSave: () => void | Promise<void>;
		children: Snippet;
	} = $props();

	let busy = $state(false);
	// Why the last try did not work, in the sheet: it stays open so the value can be corrected.
	let failure = $state('');

	$effect(() => {
		if (open) failure = '';
	});
</script>

<Dialog.Root bind:open>
	<Dialog.Content class="gap-5 rounded-3xl p-5 sm:max-w-md">
		<div class="flex flex-col gap-1 pr-10">
			<Dialog.Title class="text-lg font-semibold">{title}</Dialog.Title>
			{#if description}<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">{description}</Dialog.Description>{/if}
		</div>
		<form
			class="flex flex-col gap-5"
			onsubmit={async (event) => {
				event.preventDefault();
				busy = true;
				failure = '';
				try {
					await onSave();
					open = false;
				} catch (problem) {
					failure = (problem as Error).message || 'That did not work.';
				} finally {
					busy = false;
				}
			}}
		>
			{@render children()}
			{#if failure}<p class="text-sm leading-relaxed text-destructive" role="alert">{failure}</p>{/if}
			<Button type="submit" size="lg" class="h-11 rounded-full text-[15px]" disabled={disabled || busy}>{action}</Button>
		</form>
	</Dialog.Content>
</Dialog.Root>
