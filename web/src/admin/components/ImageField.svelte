<script lang="ts">
	import ImageOff from '@lucide/svelte/icons/image-off';
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { api, ApiError } from '../api';
	import SettingsRow from './SettingsRow.svelte';
	import TextRow from './TextRow.svelte';

	/*
	A picture, as a file from this device or as an address. Asking for an address means asking the
	operator to host the picture somewhere first, which for a photo of their antenna on their phone
	is an errand; the file picker opens the camera roll there. An address still works for a picture
	that is already online. Drop a file on the preview on a desktop.
	*/
	let {
		label,
		value,
		onChange
	}: { label: string; value: string; onChange: (value: string) => void } = $props();

	let input: HTMLInputElement | undefined = $state();
	let busy = $state(false);
	let problem = $state('');
	let broken = $state(false);
	let over = $state(false);

	$effect(() => {
		// A new address gets a fresh chance to load.
		void value;
		broken = false;
	});

	async function send(file: File | undefined) {
		if (!file) return;
		busy = true;
		problem = '';
		try {
			const result = await api.upload(file);
			onChange(result.url);
		} catch (failure) {
			problem = (failure as ApiError).message;
		} finally {
			busy = false;
		}
	}
</script>

{#if value}
	<div
		class="p-3"
		role="group"
		aria-label="{label} preview"
		ondragover={(event) => {
			event.preventDefault();
			over = true;
		}}
		ondragleave={() => (over = false)}
		ondrop={(event) => {
			event.preventDefault();
			over = false;
			void send(event.dataTransfer?.files?.[0]);
		}}
	>
		<div class="relative grid aspect-[16/9] place-items-center overflow-hidden rounded-xl bg-muted transition-shadow {over ? 'ring-2 ring-ring' : ''}">
			{#if broken}
				<span class="flex flex-col items-center gap-2 px-4 text-center text-[13px] text-muted-foreground">
					<ImageOff size={20} /> Nothing loaded from that address
				</span>
			{:else}
				{#key value}
					<img src={value} alt="" class="absolute inset-0 size-full object-cover" onerror={() => (broken = true)} />
				{/key}
			{/if}
		</div>
	</div>
{/if}
<SettingsRow
	label={busy ? 'Uploading' : value ? 'Choose another picture' : 'Choose a picture'}
	detail={value ? undefined : 'PNG, JPEG, GIF or WebP, under 8 MB'}
	tone="font-medium"
	onclick={() => !busy && input?.click()}
/>
{#if busy}
	<div class="flex items-center gap-2 px-4 py-2 text-[13px] text-muted-foreground" role="status">
		<LoaderCircle size={15} class="animate-spin" /> Sending the picture to the receiver
	</div>
{/if}
{#if problem}
	<p class="px-4 py-2 text-[13px] text-destructive" role="alert">{problem}</p>
{/if}
<TextRow label="Or an address" value={value} placeholder="https://" inputmode="url" oninput={(event) => onChange(event.currentTarget.value)} />
{#if value}
	<SettingsRow label="Remove the picture" tone="font-medium text-destructive" onclick={() => onChange('')} />
{/if}
<input
	bind:this={input}
	type="file"
	class="hidden"
	accept="image/png,image/jpeg,image/gif,image/webp"
	onchange={(event) => {
		void send(event.currentTarget.files?.[0]);
		event.currentTarget.value = '';
	}}
/>
