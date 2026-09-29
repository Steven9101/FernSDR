<script lang="ts">
	import Trash2 from '@lucide/svelte/icons/trash-2';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type UploadedPicture } from '../api';
	import { bytes } from '../lib/format';
	import SettingsGroup from './SettingsGroup.svelte';
	import { Confirm } from './ui/confirm/index';

	/*
	The pictures kept on this receiver. Uploads are named after a hash of their content, so replacing
	a background leaves the old file behind with nothing pointing at it; this is where it can be
	seen and removed. Whether a picture is in use is the receiver's call, not this page's: the page
	can be a poll behind, and being wrong would leave a listener's page pointing at nothing.
	*/
	let { refresh = 0 }: { refresh?: number } = $props();

	let pictures = $state<UploadedPicture[]>([]);
	let total = $state(0);
	let loaded = $state(false);
	let doomed = $state<UploadedPicture | null>(null);
	let confirmOpen = $state(false);

	async function load() {
		try {
			const result = await api.uploads();
			pictures = result.uploads;
			total = result.bytes;
			loaded = true;
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	onMount(() => void load());

	// Reload after the page saves, since that is what changes which pictures are in use.
	let seen = 0;
	$effect(() => {
		if (refresh !== seen) {
			seen = refresh;
			void load();
		}
	});

	async function remove(picture: UploadedPicture) {
		try {
			const result = await api.deleteUpload(picture.name);
			pictures = result.uploads;
			total = result.bytes;
			toast.success('Picture deleted');
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}
</script>

{#if loaded}
	<SettingsGroup
		title="Pictures on this receiver"
		footer={pictures.length === 0
			? 'Pictures you choose for the background or a widget are kept here.'
			: `${pictures.length} picture${pictures.length === 1 ? '' : 's'}, ${bytes(total)}. One that is in use cannot be deleted.`}
	>
		{#if pictures.length === 0}
			<p class="px-4 py-3.5 text-[15px] text-muted-foreground">None yet.</p>
		{:else}
			<ul class="grid grid-cols-3 gap-2 p-3 sm:grid-cols-4">
				{#each pictures as picture (picture.name)}
					<li class="relative overflow-hidden rounded-xl bg-muted">
						<img src={picture.url} alt="" loading="lazy" class="aspect-square w-full object-cover" />
						<div class="absolute inset-x-0 bottom-0 flex items-end justify-between gap-1 bg-linear-to-t from-black/70 to-transparent p-1.5 pt-6 text-[11px] font-medium text-white">
							<span class="truncate">{picture.in_use ? 'In use' : bytes(picture.bytes)}</span>
							{#if !picture.in_use}
								<button
									type="button"
									class="pressable grid size-10 md:size-8 shrink-0 place-items-center rounded-full bg-black/40 backdrop-blur hover:bg-destructive"
									aria-label="Delete this picture, {bytes(picture.bytes)}"
									onclick={() => {
										doomed = picture;
										confirmOpen = true;
									}}
								>
									<Trash2 size={14} />
								</button>
							{/if}
						</div>
					</li>
				{/each}
			</ul>
		{/if}
	</SettingsGroup>
{/if}

{#if doomed}
	<Confirm
		bind:open={confirmOpen}
		title="Delete this picture?"
		description="Nothing uses it now. It is removed from the receiver for good."
		action="Delete"
		destructive
		onConfirm={() => doomed && remove(doomed)}
	/>
{/if}
