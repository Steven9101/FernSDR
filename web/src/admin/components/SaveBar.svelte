<script lang="ts">
	import { fly } from '../lib/motion';
	import { cubicOut } from 'svelte/easing';
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { Button } from './ui/button/index';

	/*
	Unsaved changes, and the two things to do about them, floating where a thumb reaches. It only
	exists while something differs from what is saved: a permanently disabled Save button says
	nothing, and one that slides up says exactly that something has changed.
	*/
	let {
		visible,
		message,
		busy = false,
		save = 'Save',
		onSave,
		onDiscard
	}: {
		visible: boolean;
		message: string;
		busy?: boolean;
		save?: string;
		onSave: () => void;
		onDiscard?: () => void;
	} = $props();

	// While the bar is up the page gets room to scroll its last rows clear of it (app.css).
	$effect(() => {
		if (!visible) return;
		document.documentElement.toggleAttribute('data-savebar', true);
		return () => document.documentElement.removeAttribute('data-savebar');
	});
</script>

<!-- Always present, so a screen reader hears the change when it appears rather than depending on
an element that is only just being added. -->
<div class="sr-only" role="status" aria-live="polite">{visible ? message : ''}</div>

{#if visible}
	<div
		class="pointer-events-none fixed inset-x-0 bottom-[calc(max(0.625rem,env(safe-area-inset-bottom))+5.25rem)] z-30 flex justify-center px-3 md:bottom-6 md:pl-60"
		transition:fly={{ y: 24, duration: 320, easing: cubicOut }}
	>
		<div
			data-save-bar
			class="glass pointer-events-auto flex w-full max-w-xl flex-col gap-2 rounded-3xl p-2.5 sm:flex-row sm:items-center sm:gap-3 sm:rounded-full sm:py-2 sm:pr-2 sm:pl-5"
		>
			<span class="min-w-0 px-2 pt-1 text-sm leading-snug sm:flex-1 sm:truncate sm:p-0">{message}</span>
			<div class="flex shrink-0 gap-2">
				{#if onDiscard}
					<Button variant="ghost" size="lg" class="h-11 flex-1 rounded-full px-4 sm:h-9 sm:flex-none" disabled={busy} onclick={onDiscard}>
						Discard
					</Button>
				{/if}
				<Button size="lg" class="h-11 flex-1 rounded-full px-5 sm:h-9 sm:flex-none" disabled={busy} onclick={onSave}>
					{#if busy}<LoaderCircle class="animate-spin" />{/if}
					{save}
				</Button>
			</div>
		</div>
	</div>
{/if}
