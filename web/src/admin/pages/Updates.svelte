<script lang="ts">
	import CircleCheck from '@lucide/svelte/icons/circle-check';
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import RefreshCw from '@lucide/svelte/icons/refresh-cw';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type UpdateView } from '../api';
	import EmptyState from '../components/EmptyState.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import { Button } from '../components/ui/button/index';
	import { Confirm } from '../components/ui/confirm/index';
	import RestoreBackup from '../components/RestoreBackup.svelte';
	import ReleaseNotes from '../components/ReleaseNotes.svelte';
	import { backupFileName } from '../lib/backup';
	import { saveFile } from '../../util/save-file';
	import { ago } from '../lib/format';
	import { machineName, releaseDate, stepLabel, updating } from '../lib/updates';

	/*
	Updates: which FernSDR runs here, whether a newer release is published and what it changes,
	and moving to it. The update is the updater's, running as root beside the receiver; this page
	asks for it and follows its account. The receiver restarts on the way, so the page loses it for
	a few seconds and then the session with it: signed in again, the result is here.
	*/
	let view = $state<UpdateView | null>(null);
	let loadError = $state('');
	// The receiver stopped answering while an update ran: it is restarting.
	let away = $state(false);
	let confirmOpen = $state(false);
	let notesOpen = $state(false);

	const checking = $derived(view?.check.state === 'checking');
	const underWay = $derived(updating(view?.status));
	const found = $derived(view?.check.state === 'done' && view.check.newer ? view.check : null);
	// About what fits in the folded height; shorter notes show whole, with nothing to open.
	const folded = $derived(!notesOpen && (found?.notes?.length ?? 0) > 700);

	async function load() {
		try {
			const previous = view;
			view = await api.updates();
			loadError = '';
			away = false;
			if (previous?.check.state === 'checking' && view.check.state === 'failed') {
				toast.error(view.check.error ?? 'The look for updates failed.');
			}
		} catch (problem) {
			if (problem instanceof ApiError) {
				loadError = problem.message;
			} else if (updating(view?.status)) {
				away = true;
			} else {
				loadError = 'The receiver did not answer.';
			}
		}
	}

	onMount(() => void load());

	// Quickly while something runs, slowly otherwise. Only derived state is read here: load()
	// assigns `view`, and reading it in this effect would make every answer ask again.
	$effect(() => {
		const timer = window.setInterval(load, checking || underWay || away ? 1500 : 15000);
		return () => window.clearInterval(timer);
	});

	async function look() {
		try {
			view = await api.checkUpdates();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	let saving = $state(false);
	async function download() {
		saving = true;
		try {
			const backup = await api.backup();
			const station = typeof backup.station === 'string' ? backup.station : '';
			saveFile(JSON.stringify(backup), backupFileName(station, new Date()), 'application/json');
			const left = Array.isArray(backup.pictures_left_out) ? backup.pictures_left_out.length : 0;
			if (left) toast.warning(`${left} of the pictures did not fit; upload them again on the new machine.`);
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			saving = false;
		}
	}

	async function update() {
		if (!found?.version) return;
		try {
			view = await api.startUpdate(found.version);
			toast.success(`Updating to ${found.version}`);
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}
</script>

<PageHeader title="Updates" description="Which version of FernSDR runs here, moving to a newer one, and moving to another computer.">
	{#snippet actions()}
		{#if view?.available}
			<Button
				variant="secondary"
				size="lg"
				class="h-11 rounded-full px-4 md:h-9"
				disabled={checking || underWay || away}
				onclick={look}
			>
				<RefreshCw class={checking ? 'animate-spin' : ''} /> Check for updates
			</Button>
		{/if}
	{/snippet}
</PageHeader>

{#if !view}
	{#if loadError}
		<p class="text-destructive" role="alert">{loadError}</p>
	{:else}
		<p class="text-muted-foreground">Loading…</p>
	{/if}
{:else}
	<div class="flex max-w-2xl flex-col gap-8">
		<SettingsGroup title="This receiver" footer={view.available ? undefined : view.unavailable}>
			<SettingsRow label="FernSDR" value={view.running} />
			<SettingsRow label="Machine" value={machineName(view.platform)} />
			{#if view.available}
				<SettingsRow
					label="Last look"
					value={checking ? 'looking now' : view.check.checked ? ago(view.check.checked * 1000) : 'not yet'}
				/>
			{/if}
		</SettingsGroup>

		{#if away || (view.status && underWay)}
			<SettingsGroup
				title="Updating"
				footer={view.status?.state === 'trial' && !away
					? 'It is kept once it has run for a minute with every band back; otherwise the version before comes back by itself.'
					: 'The receiver restarts into the new version, and you will have to sign in again.'}
			>
				<div class="flex items-start gap-3 px-4 py-3.5" role="status" aria-live="polite">
					<LoaderCircle size={18} class="mt-0.5 shrink-0 animate-spin text-muted-foreground" />
					<span class="flex min-w-0 flex-col">
						<span class="text-[15px]">
							{away ? 'The receiver is restarting' : view.status ? stepLabel(view.status) : ''}
						</span>
						{#if !away && view.status?.message}
							<span class="text-[13px] text-muted-foreground">{view.status.message}</span>
						{/if}
					</span>
				</div>
			</SettingsGroup>
		{:else if view.status}
			<SettingsGroup title="Last update">
				<SettingsRow
					label={stepLabel(view.status)}
					detail={view.status.state === 'updated' ? undefined : view.status.message}
					value={ago(view.status.time * 1000)}
					tone={view.status.state === 'updated' ? '' : 'text-warning'}
				/>
			</SettingsGroup>
		{/if}

		{#if view.check.state === 'failed' && view.check.error}
			<p class="text-[15px] text-destructive" role="alert">{view.check.error}</p>
		{/if}

		{#if found && !underWay && !away}
			<SettingsGroup
				title="Available"
				footer={`Listeners are away for a few seconds while the receiver restarts. If ${found.version} does not work within five minutes, ${view.running} comes back by itself.`}
			>
				<SettingsRow label={`FernSDR ${found.version}`} detail={found.date ? `Released ${releaseDate(found.date)}` : undefined}>
					{#snippet control()}
						<Button size="lg" class="h-10 rounded-full px-4 md:h-9" onclick={() => (confirmOpen = true)}>
							Update
						</Button>
					{/snippet}
				</SettingsRow>
				{#if found.notes}
					<!-- The signed release's notes, set as text: nothing in them becomes markup. -->
					<!-- Long notes open on request: a release's whole changelog would otherwise be the page. -->
					<div class="relative px-4 py-4 {folded ? 'max-h-80 overflow-hidden' : ''}">
						<ReleaseNotes notes={found.notes} />
						{#if folded}
							<div class="absolute inset-x-0 bottom-0 flex h-24 items-end justify-center bg-linear-to-t from-card via-card/90 to-transparent pb-3">
								<Button variant="secondary" size="lg" class="h-10 rounded-full px-4 md:h-9" onclick={() => (notesOpen = true)}>
									All changes
								</Button>
							</div>
						{/if}
					</div>
				{/if}
			</SettingsGroup>
		{:else if view.check.state === 'done' && !underWay && !away}
			<EmptyState icon={CircleCheck} title="Up to date">
				No release newer than {view.running} is published.
			</EmptyState>
		{/if}
	</div>

	{#if found?.version}
		<Confirm
			bind:open={confirmOpen}
			title={`Update to ${found.version}?`}
			description={`The receiver restarts into ${found.version}, and everyone listening drops out for a few seconds. You will have to sign in again. If it does not work within five minutes, ${view.running} comes back by itself.`}
			action="Update"
			onConfirm={update}
		/>
	{/if}
{/if}

<!-- Apart from the updater: a backup is wanted most when the rest of this page cannot load. -->
<div class="mt-8 flex max-w-2xl flex-col gap-8">
	<SettingsGroup
		title="Backup"
		footer="Bands, station details, look and pictures in one file, to move this receiver to another computer or keep for a bad day. Your password is not in it: the new machine keeps its own."
	>
		<SettingsRow label="Download a backup" detail="A file of this receiver as it is now">
			{#snippet control()}
				<Button variant="secondary" size="lg" class="h-10 rounded-full px-4 md:h-9" disabled={saving} onclick={download}>
					Download
				</Button>
			{/snippet}
		</SettingsRow>
		<RestoreBackup />
	</SettingsGroup>
</div>
