<script lang="ts">
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import { Button } from './ui/button/index';
	import { Confirm } from './ui/confirm/index';
	import { readBackup, type BackupSummary } from '../lib/backup';
	import { installFromCatalog, restartAndReload } from '../lib/receiver';
	import { receiverLabel } from '../../util/receivers';

	/*
	Playing a backup from another FernSDR back onto this one: the file is read here first, so a
	wrong one is named before anything is sent, then the receiver takes it, fetches the modules the
	old machine had and restarts. As a row on the Updates page, or as a line of text in the first
	step of the setup, where someone moving a receiver starts.
	*/
	let {
		variant = 'row',
		onRestored
	}: {
		variant?: 'row' | 'link';
		/** Called once the receiver has the backup, before it restarts. */
		onRestored?: () => void;
	} = $props();

	let input = $state<HTMLInputElement | null>(null);
	let text = '';
	let summary = $state<BackupSummary | null>(null);
	let confirmOpen = $state(false);
	let busy = $state('');
	// Where the receiver cannot restart itself, what to do instead; the backup waits in its files.
	let restartNote = $state('');

	async function chosen() {
		const file = input?.files?.[0];
		if (input) input.value = '';
		if (!file) return;
		text = await file.text();
		summary = readBackup(text);
		if (!summary) {
			toast.error(`${file.name} is not a FernSDR backup.`);
			return;
		}
		confirmOpen = true;
	}

	const description = $derived.by(() => {
		if (!summary) return '';
		const made = summary.created ? ` made on ${new Date(summary.created).toLocaleDateString()}` : '';
		const bands = `${summary.bands} ${summary.bands === 1 ? 'band' : 'bands'}`;
		const names = summary.modules.map((id) => receiverLabel(id) ?? id);
		const modules = names.length
			? ` It installs the ${list(names)} ${names.length === 1 ? 'module' : 'modules'} from the module catalog where this receiver lacks them.`
			: '';
		return (
			`The backup of ${summary.station}${made}, with ${bands}. It replaces this receiver's bands, station details ` +
			`and look. This machine keeps its own address, password, modules folder and listing.${modules} ` +
			`The receiver then restarts, and listeners drop out for a few seconds.`
		);
	});

	/** "A", "A and B", "A, B and C" */
	function list(names: string[]): string {
		return names.length < 2 ? names.join('') : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
	}

	async function restore() {
		busy = 'Restoring the backup…';
		try {
			const answer = await api.restore(text);
			onRestored?.();
			const failed: string[] = [];
			for (const module of answer.modules) {
				const name = receiverLabel(module.id) ?? module.id;
				busy = `Installing the ${name} module…`;
				try {
					await installFromCatalog(module.id, name, module.origin);
				} catch (problem) {
					failed.push((problem as Error).message);
				}
			}
			if (failed.length) toast.error(`${failed.join(' ')} Install it on the Modules page after the restart.`);
			if (answer.pictures_not_restored) {
				toast.warning(
					`${answer.pictures_not_restored} of the backup's pictures could not be restored; upload them again on the Appearance page.`
				);
			}
			busy = 'Restarting the receiver…';
			const note = await restartAndReload();
			if (note) {
				restartNote = `The backup is in place and waits for a restart. ${note}`;
				busy = '';
			}
		} catch (problem) {
			toast.error((problem as ApiError).message);
			busy = '';
		}
	}
</script>

<input
	bind:this={input}
	type="file"
	accept=".json,application/json"
	class="hidden"
	aria-hidden="true"
	tabindex="-1"
	onchange={chosen}
/>

{#if busy || restartNote}
	<div class="flex items-start gap-3 px-4 py-3.5 {variant === 'link' ? 'rounded-2xl bg-card' : ''}" role="status" aria-live="polite">
		{#if busy}<LoaderCircle size={18} class="mt-0.5 shrink-0 animate-spin text-muted-foreground" />{/if}
		<span class="text-[15px] leading-relaxed">{busy || restartNote}</span>
	</div>
{:else if variant === 'link'}
	<p class="text-[15px] leading-relaxed text-muted-foreground">
		Moving from another FernSDR?
		<button type="button" class="text-foreground underline underline-offset-4" onclick={() => input?.click()}>
			Restore its backup
		</button>
		instead of setting this one up.
	</p>
{:else}
	<div class="flex min-h-13 items-center gap-3 px-4 py-2.5">
		<span class="flex min-w-0 flex-1 flex-col">
			<span class="text-[15px]">Restore a backup</span>
			<span class="text-[13px] text-muted-foreground">From this receiver or another one</span>
		</span>
		<Button variant="secondary" size="lg" class="h-10 rounded-full px-4 md:h-9" onclick={() => input?.click()}>
			Choose file
		</Button>
	</div>
{/if}

<Confirm bind:open={confirmOpen} title="Restore this backup?" {description} action="Restore" onConfirm={restore} />
