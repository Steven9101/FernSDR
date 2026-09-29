<script lang="ts">
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import { live } from '../lib/live.svelte';

	/*
	Addresses silenced in the chat. Most are not listening now: a mute outlasts the visit, which is
	the point, so this is its own list rather than a mark on the listeners.
	*/
	const muted = $derived(live.state?.muted ?? []);

	async function unmute(address: string) {
		try {
			await api.unmute(address);
			toast.success(`${address} may chat again`);
			await live.refresh();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}
</script>

{#if muted.length > 0}
	<section data-settings-group class="flex flex-col gap-2">
		<h2 class="px-4 text-sm font-medium text-muted-foreground">Muted in the chat</h2>
		<ul class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
			{#each muted as entry (entry.address)}
				<li class="flex min-h-13 items-center gap-3 py-2 pr-2 pl-4">
					<span class="flex min-w-0 flex-1 flex-col">
						<span class="truncate text-[15px] tabular">{entry.address}</span>
						<span class="text-[13px] text-muted-foreground">
							{entry.until === 0 ? 'Until you lift it' : `Until ${new Date(entry.until).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`}
						</span>
					</span>
					<button
						type="button"
						class="pressable h-11 shrink-0 rounded-full px-3 text-sm font-medium transition-colors hover:bg-accent md:h-9"
						onclick={() => unmute(entry.address)}
					>
						Unmute
					</button>
				</li>
			{/each}
		</ul>
	</section>
{/if}
