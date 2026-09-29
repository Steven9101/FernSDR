<script lang="ts">
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type UpdateView } from '../api';
	import ReleaseNotes from './ReleaseNotes.svelte';
	import { Button } from './ui/button/index';
	import * as Dialog from './ui/dialog/index';
	import { navigate } from '../lib/router.svelte';
	import { dismissUpdate, releaseDate } from '../lib/updates';

	/*
	A newer FernSDR, offered once: what it is, what it changes, and updating to it with one press.
	"Later" puts this version away for good; the next one is offered again. Nothing updates without
	the press, and the Updates page follows the update as it runs.
	*/
	let { view, open = $bindable(false) }: { view: UpdateView; open?: boolean } = $props();

	const found = $derived(view.check);
	let starting = $state(false);

	function later() {
		if (found.version) dismissUpdate(found.version);
		open = false;
	}

	async function update() {
		if (!found.version) return;
		starting = true;
		try {
			await api.startUpdate(found.version);
			dismissUpdate(found.version);
			open = false;
			toast.success(`Updating to ${found.version}`);
			navigate({ page: 'updates' });
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			starting = false;
		}
	}
</script>

<Dialog.Root
	bind:open
	onOpenChange={(next) => {
		if (!next && found.version) dismissUpdate(found.version);
	}}
>
	<Dialog.Content class="max-h-[88dvh] gap-0 overflow-hidden rounded-3xl p-0 sm:max-w-lg">
		<div class="flex flex-col gap-1 px-6 pt-6 pb-4 pr-14">
			<p class="text-[13px] font-medium text-muted-foreground">Update available</p>
			<Dialog.Title class="text-[1.625rem] leading-tight font-bold tracking-[-0.03em]">FernSDR {found.version}</Dialog.Title>
			<Dialog.Description class="text-[14px] text-muted-foreground">
				You have {view.running}{#if found.date}. Released {releaseDate(found.date)}{/if}.
			</Dialog.Description>
		</div>
		{#if found.notes}
			<div class="max-h-[46dvh] overflow-y-auto border-y border-border bg-card px-6 py-4">
				<ReleaseNotes notes={found.notes} />
			</div>
		{/if}
		<div class="flex flex-col gap-4 px-6 pt-4 pb-6">
			<p class="text-[13px] leading-relaxed text-muted-foreground">
				Listeners drop out for a few seconds while the receiver restarts, and you sign in again. If {found.version} does
				not work within five minutes, {view.running} comes back by itself.
			</p>
			<div class="flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
				<Button variant="secondary" size="lg" class="h-11 rounded-full px-5" onclick={later}>Later</Button>
				<Button size="lg" class="h-11 rounded-full px-5" disabled={starting} onclick={update}>
					{#if starting}<LoaderCircle class="animate-spin" />{/if} Update now
				</Button>
			</div>
		</div>
	</Dialog.Content>
</Dialog.Root>
