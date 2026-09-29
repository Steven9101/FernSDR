<script lang="ts">
	import { fade, flip, fly } from '../lib/motion';
	import Mic from '@lucide/svelte/icons/mic';
	import MicOff from '@lucide/svelte/icons/mic-off';
	import X from '@lucide/svelte/icons/x';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type ListenerState } from '../api';
	import { bitrate, duration, megahertz } from '../lib/format';
	import { live } from '../lib/live.svelte';
	import Sparkline from './Sparkline.svelte';
	import { Confirm } from './ui/confirm/index';

	/*
	Who is connected and to what, with the two things an operator does about a listener: mute them
	in the chat, or end their session. Everyone on the overview; only one band's listeners, as a
	quiet group, beside that band's settings.
	*/
	let { band }: { band?: string } = $props();

	const listeners = $derived(
		[...(live.state?.listeners ?? [])]
			.filter((listener) => band === undefined || listener.band === band)
			.sort((a, b) => a.band.localeCompare(b.band) || b.connected_seconds - a.connected_seconds)
	);

	let pending = $state<{ kind: 'mute' | 'disconnect'; listener: ListenerState } | null>(null);
	let open = $state(false);

	async function act(kind: 'mute' | 'unmute' | 'disconnect', listener: ListenerState) {
		try {
			if (kind === 'mute') await api.mute(listener.address, 0);
			else if (kind === 'unmute') await api.unmute(listener.address);
			else await api.disconnect(listener.id);
			toast.success(
				kind === 'mute'
					? `${listener.address} is muted in the chat`
					: kind === 'unmute'
						? `${listener.address} may chat again`
						: 'Disconnected'
			);
			await live.refresh();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	function ask(kind: 'mute' | 'disconnect', listener: ListenerState) {
		pending = { kind, listener };
		open = true;
	}
</script>

{#snippet actions(listener: ListenerState)}
	<div class="flex shrink-0 gap-0.5">
		<button
			type="button"
			class="pressable grid size-11 md:size-10 place-items-center rounded-full transition-colors hover:bg-accent {listener.muted
				? 'text-destructive'
				: 'text-muted-foreground hover:text-foreground'}"
			aria-label={listener.muted ? `Let ${listener.address} into the chat again` : `Mute ${listener.address} in the chat`}
			title={listener.muted ? 'Muted in the chat. Unmute' : 'Mute in the chat'}
			onclick={() => (listener.muted ? act('unmute', listener) : ask('mute', listener))}
		>
			{#if listener.muted}<MicOff size={18} />{:else}<Mic size={18} />{/if}
		</button>
		<button
			type="button"
			class="pressable grid size-11 md:size-10 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-destructive/10 hover:text-destructive"
			aria-label="Disconnect {listener.address}"
			title="Disconnect"
			onclick={() => ask('disconnect', listener)}
		>
			<X size={18} />
		</button>
	</div>
{/snippet}

{#if band !== undefined}
	<section data-settings-group class="flex flex-col gap-2">
		<h2 class="px-4 text-sm font-medium text-muted-foreground">
			Listening here{listeners.length > 0 ? ` · ${listeners.length}` : ''}
		</h2>
		{#if listeners.length === 0}
			<p class="rounded-2xl bg-card px-4 py-3.5 text-[15px] text-muted-foreground">Nobody is tuned in.</p>
		{:else}
			<ul class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
				{#each listeners as listener (listener.id)}
					<li
						animate:flip={{ duration: 360 }}
						in:fly={{ y: 8, duration: 280 }}
						out:fade={{ duration: 160 }}
						class="flex min-h-13 items-center gap-3 py-2 pr-2 pl-4"
					>
						<span class="flex min-w-0 flex-1 flex-col">
							<span class="flex items-baseline gap-1.5 text-[15px] tabular">
								{megahertz(listener.frequency)}
								<span class="text-xs font-medium text-muted-foreground uppercase">{listener.mode}</span>
							</span>
							<span class="truncate text-[13px] text-muted-foreground tabular">
								{listener.address} · {duration(listener.connected_seconds)}
							</span>
						</span>
						{@render actions(listener)}
					</li>
				{/each}
			</ul>
		{/if}
	</section>
{:else}
	<section class="@container flex flex-col gap-2">
		<h2 class="px-4 text-sm font-medium text-muted-foreground">
			Listening{listeners.length > 0 ? ` · ${listeners.length}` : ''}
		</h2>

		{#if live.state && listeners.length === 0}
			<p class="rounded-2xl bg-card px-4 py-3.5 text-[15px] text-muted-foreground">
				Nobody is listening right now. Anyone who opens the receiver appears here, with what they are tuned to.
			</p>
		{:else if listeners.length > 0}
			<ul class="flex flex-col divide-y divide-border rounded-2xl bg-card">
				{#each listeners as listener (listener.id)}
					<li
						animate:flip={{ duration: 360 }}
						in:fly={{ y: 10, duration: 320 }}
						out:fade={{ duration: 180 }}
						class="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-3 gap-y-1 py-2.5 pr-2 pl-4 @lg:grid-cols-[minmax(0,1.1fr)_minmax(0,1fr)_auto_auto]"
					>
						<span class="flex min-w-0 flex-col">
							<span class="flex items-baseline gap-1.5 text-[15px] tabular">
								{megahertz(listener.frequency)}
								<span class="text-xs font-medium text-muted-foreground uppercase">{listener.mode}</span>
							</span>
							<span class="truncate text-[13px] text-muted-foreground">{listener.band}</span>
						</span>
						<span class="row-start-2 flex min-w-0 flex-col @lg:row-start-auto">
							<span class="truncate text-[13px] text-muted-foreground tabular @lg:text-[15px] @lg:text-foreground">{listener.address}</span>
							<span class="hidden text-[13px] text-muted-foreground @lg:block">{duration(listener.connected_seconds)}</span>
						</span>
						<span class="hidden items-center gap-3 @lg:flex">
							<Sparkline
								values={live.streams[listener.id] ?? []}
								width={72}
								height={24}
								class="text-muted-foreground"
								label="{listener.address} stream over the last minute"
							/>
							<span class="w-20 text-right text-sm tabular">{bitrate(listener.audio_bitrate + listener.waterfall_bitrate)}</span>
						</span>
						<span class="row-span-2 @lg:row-span-1">{@render actions(listener)}</span>
					</li>
				{/each}
			</ul>
		{/if}
	</section>
{/if}

{#if pending}
	<Confirm
		bind:open
		title={pending.kind === 'mute' ? `Mute ${pending.listener.address} in the chat?` : 'Disconnect this listener?'}
		description={pending.kind === 'mute'
			? 'They can go on listening and can no longer post. They are told so rather than left talking to nobody. It lasts until you lift it, across restarts, and anyone sharing that address is muted with them.'
			: 'Their audio stops at once. Nothing stops them connecting again, so this ends a session rather than barring anybody.'}
		action={pending.kind === 'mute' ? 'Mute' : 'Disconnect'}
		destructive={pending.kind === 'disconnect'}
		onConfirm={() => pending && act(pending.kind, pending.listener)}
	/>
{/if}
