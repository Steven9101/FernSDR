<script lang="ts">
	import { fly } from '../lib/motion';
	import Check from '@lucide/svelte/icons/check';
	import Copy from '@lucide/svelte/icons/copy';
	import Download from '@lucide/svelte/icons/download';
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import Package from '@lucide/svelte/icons/package';
	import RefreshCw from '@lucide/svelte/icons/refresh-cw';
	import Trash2 from '@lucide/svelte/icons/trash-2';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type CatalogRelease, type InstalledModule, type ModuleDevice, type ModulesView } from '../api';
	import Columns from '../components/Columns.svelte';
	import EmptyState from '../components/EmptyState.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import { Button } from '../components/ui/button/index';
	import { Confirm } from '../components/ui/confirm/index';
	import * as Dialog from '../components/ui/dialog/index';
	import { Switch } from '../components/ui/switch/index';
	import { ago, bytes } from '../lib/format';
	import ChoiceSheet from '../components/ChoiceSheet.svelte';
	import { readSection, sectionNames, setSectionValue } from '../lib/ini-sections';

	/*
	Modules: the programs that bring hardware in. What an operator does here, in the order they
	tend to do it: install one, find the device it sees so a band can name it, later take an update
	or go back to the version that worked. Every change is a job the receiver runs on its own
	thread; the page follows it instead of waiting.
	*/
	let view = $state<ModulesView | null>(null);
	let loadError = $state('');
	let pending = $state<{ title: string; description: string; action: string; destructive?: boolean; run: () => Promise<ModulesView> } | null>(null);
	let confirmOpen = $state(false);

	let versionsOf = $state<string | null>(null);
	let versionsOpen = $state(false);
	const shownVersions = $derived(view?.installed.find((module) => module.id === versionsOf));

	const busy = $derived(view?.jobs.some((job) => job.state === 'queued' || job.state === 'running') ?? false);
	const running = $derived(view?.jobs.find((job) => job.state === 'running' || job.state === 'queued'));

	// The bands each module runs and the device each names, from the configuration, so a device
	// found here says which band has it and can be given to one.
	let configText = $state('');
	const moduleBands = $derived.by(() => {
		const out: Record<string, { id: string; device: string }[]> = {};
		for (const name of sectionNames(configText, 'band')) {
			const values = readSection(configText, name);
			if (values?.get('source') !== 'module') continue;
			const module = values.get('module') ?? '';
			(out[module] ??= []).push({ id: name.slice('band:'.length), device: values.get('module.device') ?? '' });
		}
		return out;
	});
	const sameDevice = (a: string, b: string) => a.trim().toLowerCase() === b.trim().toLowerCase();

	let giving = $state<{ module: string; setting: string; name: string } | null>(null);
	let givingOpen = $state(false);

	function giveTo(bandId: string) {
		const target = giving;
		if (!target) return;
		ask({
			title: `Give it to band ${bandId}?`,
			description: `Band ${bandId} uses ${target.name} (module.device = ${target.setting}) from now on. It restarts, and anyone listening to it hears a short gap.`,
			action: 'Give and restart',
			run: async () => {
				const { text } = await api.readConfig();
				await api.writeConfig(setSectionValue(text, `band:${bandId}`, 'module.device', target.setting));
				await api.restartBand(bandId);
				configText = (await api.readConfig()).text;
				return api.modules();
			}
		});
	}

	async function load() {
		try {
			const previous = view;
			api.readConfig().then((config) => (configText = config.text), () => {});
			view = await api.modules();
			loadError = '';
			// A job that finished since the last look says how it went, once.
			for (const job of view.jobs) {
				const before = previous?.jobs.find((entry) => entry.number === job.number);
				if (!before || before.state === job.state) continue;
				if (job.state === 'done') toast.success(job.message ?? job.summary);
				else if (job.state === 'failed') toast.error(`${job.summary}: ${job.message ?? 'failed'}`);
			}
		} catch (problem) {
			loadError = (problem as ApiError).message;
		}
	}

	onMount(() => void load());

	// Quickly while a job runs, slowly otherwise, for jobs started from another tab. Only `busy`
	// may be read here: load() reads and assigns `view`, so calling it from this effect would
	// make every answer ask again.
	$effect(() => {
		const timer = window.setInterval(load, busy ? 1200 : 10000);
		return () => window.clearInterval(timer);
	});

	async function start(run: () => Promise<ModulesView>) {
		try {
			view = await run();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	function ask(next: NonNullable<typeof pending>) {
		pending = next;
		confirmOpen = true;
	}

	function restartNote(module: InstalledModule): string {
		return module.bands.length === 0
			? ''
			: ` ${module.bands.length === 1 ? 'Band' : 'Bands'} ${module.bands.join(', ')} restart; listeners stay connected and hear a short gap.`;
	}

	function releaseState(release: CatalogRelease): 'none' | 'installed' | 'available' {
		if (!release.asset || !release.id || !release.version) return 'none';
		const installed = view?.installed.find((module) => module.id === release.id);
		return installed?.versions.some((version) => version.version === release.version) ? 'installed' : 'available';
	}

	function origin(module: InstalledModule): string {
		return module.origin === 'file' ? 'Installed from a file' : `From ${module.origin}`;
	}

	function deviceSetting(device: ModuleDevice, index: number): string {
		return device.serial ? `serial:${device.serial}` : `index:${device.index ?? index}`;
	}

	async function copy(text: string) {
		try {
			await navigator.clipboard.writeText(text);
			toast.success(`Copied ${text}`);
		} catch {
			// Clipboard access needs a secure page and permission; say what to copy instead.
			toast.message(`Copy this into the band: ${text}`);
		}
	}
</script>

<PageHeader
	title="Modules"
	description="Programs that bring hardware in, such as an RTL-SDR dongle. A band uses one with source = module."
>
	{#snippet actions()}
		<Button
			variant="secondary"
			size="lg"
			class="rounded-full px-4"
			disabled={busy || !view || view.catalog.length === 0 || !view.downloader}
			onclick={() => start(api.checkModules)}
		>
			<RefreshCw class={busy ? 'animate-spin' : ''} /> Check for updates
		</Button>
	{/snippet}
</PageHeader>

{#if loadError && !view}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !view}
	<div class="flex max-w-2xl flex-col gap-3">
		<div class="shimmer h-40 rounded-2xl bg-card"></div>
		<div class="shimmer h-24 rounded-2xl bg-card"></div>
	</div>
{:else}
	{@const shown = view}
	<Columns asideLabel="Catalog and this receiver">
		{#snippet main()}
			{#if running}
				<p class="flex items-center gap-3 rounded-2xl bg-card px-4 py-3.5 text-[15px]" role="status" transition:fly={{ y: -8, duration: 240 }}>
					<LoaderCircle size={18} class="shrink-0 animate-spin text-muted-foreground" />
					{running.summary}{running.state === 'queued' ? ', waiting' : ''}
				</p>
			{/if}

			{#if !shown.downloader}
				<p class="rounded-2xl bg-card px-4 py-3.5 text-[15px] leading-relaxed">
					<span class="font-medium text-destructive">curl is not installed</span>, so the catalog cannot be read.
					Install it with <code class="rounded bg-muted px-1.5 py-0.5 text-sm">sudo apt install curl</code>, or copy a
					package to the machine and run
					<code class="rounded bg-muted px-1.5 py-0.5 text-sm break-all">fernsdr --install-module file.fernmod fernsdr.conf</code>.
				</p>
			{/if}

			{#if shown.installed.length === 0}
				<EmptyState icon={Package} title="No modules are installed.">
					Install one from the catalog, or copy a package to the machine and run fernsdr --install-module.
				</EmptyState>
			{/if}

			{#each shown.installed as module (module.id)}
				{@const active = module.versions.find((version) => version.version === module.active)}
				{@const devices = shown.devices[module.id]}
				{@const name = active?.name ?? module.id}
				<SettingsGroup title={name} footer={active?.description}>
					<SettingsRow
						label="Switched on"
						detail={active?.kind === 'decoder'
							? 'A decoder: set it up on the Decoders page'
							: module.bands.length === 0
								? 'No band uses it'
								: `Used by ${module.bands.join(', ')}`}
					>
						{#snippet control()}
							<!-- Shows what the receiver says, not the last tap: switching off asks first, and a
							cancelled or failed switch must not be left looking done. -->
							<Switch
								bind:checked={
									() => module.enabled,
									(checked) =>
										checked
											? start(() => api.enableModule(module.id, true))
											: ask({
													title: `Switch ${name} off?`,
													description: `${module.bands.length ? `Bands ${module.bands.join(', ')} stop receiving` : 'No band uses it'} until you switch it on again. It stays installed.`,
													action: 'Switch off',
													run: () => api.enableModule(module.id, false)
												})
								}
								disabled={busy}
								aria-label="{name} switched on"
							/>
						{/snippet}
					</SettingsRow>
					{#if active?.requires?.length}
						<div class="flex flex-col gap-1 px-4 py-3">
							<span class="text-[15px]">Needs first</span>
							{#each active.requires as need (need)}
								<span class="text-[13px] leading-relaxed text-warning">{need}</span>
							{/each}
							<span class="text-[13px] text-muted-foreground">The module cannot bring this along; install it on this machine as its instructions say.</span>
						</div>
					{/if}
					{#if module.update}
						{@const update = module.update}
						<SettingsRow
							label="Update to {update.version}"
							detail="Version {module.active} stays installed, to go back to"
							tone="font-semibold"
							onclick={() =>
								ask({
									title: `Update ${name} to ${update.version}?`,
									description: `The receiver downloads it, checks it, and makes it the version bands use.${restartNote(module)} Version ${module.active} stays installed, so you can go back.`,
									action: 'Update',
									run: () => api.installModule(update.repository, update.tag, update.asset, true)
								})}
						/>
					{/if}
					<SettingsRow
						label="Version"
						detail={origin(module)}
						value={module.active}
						onclick={() => {
							versionsOf = module.id;
							versionsOpen = true;
						}}
					/>
				</SettingsGroup>

				<SettingsGroup title="{name} devices">
					{#snippet footer()}
						{#if !devices}
							Ask the module which devices it can see, then name one in a band as module.device.
						{:else if devices.error}
							<span class="text-destructive">{devices.error}</span>
						{:else if devices.devices.length === 0}
							None found {ago(devices.fetched_ms)}. Plug it in. On a service built from source, run
							tools/source-install.sh --service --usb once so the receiver may open USB devices.
						{:else}
							Found {ago(devices.fetched_ms)}. Give a device to a band here, or put its line in the band's section.
							One device serves one band.
						{/if}
					{/snippet}
					{#each devices?.devices ?? [] as device, index (index)}
						{@const setting = deviceSetting(device, index)}
						{@const holder = (moduleBands[module.id] ?? []).find((band) => sameDevice(band.device, setting))}
						{@const deviceName = `${device.name ?? 'Unnamed device'}${device.tuner ? ` · ${device.tuner}` : ''}`}
						<SettingsRow
							label={deviceName}
							detail={holder
								? `Band ${holder.id} · module.device = ${setting}`
								: device.usable
									? `module.device = ${setting}`
									: `In use${device.error ? `: ${device.error}` : ''}`}
						>
							{#snippet control()}
								{#if !holder && device.usable && (moduleBands[module.id] ?? []).length > 0}
									<Button
										variant="secondary"
										class="h-11 rounded-full px-3.5 md:h-9"
										disabled={busy}
										onclick={() => {
											giving = { module: module.id, setting, name: deviceName };
											givingOpen = true;
										}}
									>
										Give to a band
									</Button>
								{/if}
								<button
									type="button"
									class="pressable -mr-2 grid size-11 md:size-10 shrink-0 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
									aria-label="Copy module.device = {setting}"
									title="Copy"
									onclick={() => copy(`module.device = ${setting}`)}
								>
									<Copy size={17} />
								</button>
							{/snippet}
						</SettingsRow>
					{/each}
					<SettingsRow
						label={devices ? 'Look again' : 'Find devices'}
						tone="font-medium"
						onclick={() => start(() => api.findDevices(module.id))}
					/>
				</SettingsGroup>
			{/each}
		{/snippet}

		{#snippet aside()}
			<SettingsGroup title="Catalog">
				{#snippet footer()}
					{#if shown.catalog.length === 0}
						No repositories are listed in [modules] catalog. That section changes only in the file on the machine.
					{:else}
						Nothing is downloaded unless you ask. The catalog is set in the file on the machine.
					{/if}
				{/snippet}
				{#each shown.available as repository (repository.repository)}
					<div class="flex flex-col gap-0.5 px-4 py-3">
						<span class="truncate text-[15px] font-medium">{repository.repository}</span>
						<span class="text-[13px] {repository.error ? 'text-destructive' : 'text-muted-foreground'}">
							{repository.error ?? `Checked ${ago(repository.fetched_ms)}`}
						</span>
					</div>
					{#each repository.releases ?? [] as release (release.tag)}
						{@const status = releaseState(release)}
						<div class="flex min-h-13 items-center gap-3 py-2 pr-2 pl-4">
							<span class="flex min-w-0 flex-1 flex-col">
								<span class="text-[15px] tabular">
									{release.version ?? release.tag}
									{#if release.prerelease}<span class="text-[13px] text-warning">test release</span>{/if}
								</span>
								<span class="text-[13px] text-muted-foreground tabular">
									{release.published.slice(0, 10)}{release.size ? ` · ${bytes(release.size)}` : ''}
								</span>
							</span>
							{#if status === 'installed'}
								<span class="flex items-center gap-1 pr-2 text-[13px] text-muted-foreground"><Check size={15} /> Installed</span>
							{:else if status === 'none'}
								<span class="pr-2 text-right text-[13px] text-muted-foreground">None for {shown.platform || 'this machine'}</span>
							{:else}
								<Button
									variant="secondary"
									class="h-11 rounded-full px-3.5 md:h-9"
									disabled={busy}
									onclick={() => start(() => api.installModule(repository.repository, release.tag, release.asset!, false))}
								>
									<Download /> Install
								</Button>
							{/if}
						</div>
					{/each}
					{#if repository.fetched_ms && !repository.error && (repository.releases ?? []).length === 0}
						<p class="px-4 py-3 text-[15px] text-muted-foreground">No releases yet.</p>
					{/if}
				{/each}
			</SettingsGroup>

			{#if shown.jobs.length > 0}
				<SettingsGroup title="Activity">
					{#each shown.jobs as job (job.number)}
						<div class="flex flex-col gap-0.5 px-4 py-3">
							<span class="text-[15px]">{job.summary}</span>
							<span class="text-[13px] break-words {job.state === 'failed' ? 'text-destructive' : 'text-muted-foreground'}">
								{job.state === 'queued' ? 'Waiting' : job.state === 'running' ? 'Working' : job.state === 'failed' ? `Failed: ${job.message}` : job.message}
							</span>
						</div>
					{/each}
				</SettingsGroup>
			{/if}

			<SettingsGroup title="This receiver">
				<SettingsRow label="Platform" value={shown.platform || 'No packages exist for it'} />
				<SettingsRow label="Downloads with" value={shown.downloader || 'Nothing installed'} />
				<div class="flex flex-col gap-0.5 px-4 py-3">
					<span class="text-[15px]">Modules directory</span>
					<span class="text-[13px] break-all text-muted-foreground">{shown.directory}</span>
				</div>
			</SettingsGroup>
		{/snippet}
	</Columns>
{/if}

<Dialog.Root bind:open={versionsOpen}>
	<Dialog.Content class="gap-3 rounded-3xl p-3 pt-5 sm:max-w-md">
		{#if shownVersions}
			{@const module = shownVersions}
			<div class="flex flex-col gap-1 px-3 pr-12">
				<Dialog.Title class="text-lg font-semibold">Versions of {module.id}</Dialog.Title>
				<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">
					Bands run the one in use. Older ones stay here to go back to.
				</Dialog.Description>
			</div>
			<ul class="flex flex-col">
				{#each module.versions as version (version.version)}
					{@const inUse = version.version === module.active}
					<li class="flex min-h-13 items-center gap-2 rounded-xl py-2 pr-1 pl-3">
						<span class="flex min-w-0 flex-1 flex-col">
							<span class="text-[15px] tabular {inUse ? 'font-semibold' : ''}">{version.version}</span>
							<span class="text-[13px] text-muted-foreground">{inUse ? 'In use · ' : ''}{bytes(version.size)}</span>
						</span>
						{#if !inUse}
							<Button
								variant="secondary"
								class="h-11 rounded-full px-3.5 md:h-9"
								disabled={busy}
								onclick={() => {
									versionsOpen = false;
									ask({
										title: `Use ${module.id} ${version.version}?`,
										description: `Bands run ${version.version} instead of ${module.active}.${restartNote(module)}`,
										action: 'Use this version',
										run: () => api.activateModule(module.id, version.version)
									});
								}}
							>
								Use
							</Button>
						{:else}
							<Check size={18} class="mr-2 shrink-0" />
						{/if}
						{#if !inUse || module.bands.length === 0}
							<button
								type="button"
								class="pressable grid size-11 md:size-10 shrink-0 place-items-center rounded-full text-muted-foreground hover:bg-destructive/10 hover:text-destructive disabled:opacity-40"
								disabled={busy}
								aria-label="Remove {module.id} {version.version}"
								onclick={() => {
									versionsOpen = false;
									ask({
										title: `Remove ${module.id} ${version.version}?`,
										description: 'The program is deleted from this machine. Installing it again means downloading it again.',
										action: 'Remove',
										destructive: true,
										run: () => api.removeModule(module.id, version.version)
									});
								}}
							>
								<Trash2 size={16} />
							</button>
						{/if}
					</li>
				{/each}
			</ul>
		{/if}
	</Dialog.Content>
</Dialog.Root>

{#if giving}
	{@const target = giving}
	<ChoiceSheet
		bind:open={givingOpen}
		title="Which band gets {target.name}?"
		description="Bands set up with {target.module}. A band already holding another device lets that one go."
		options={(moduleBands[target.module] ?? []).map((band) => ({
			value: band.id,
			label: band.id,
			detail: band.device ? `Now module.device = ${band.device}` : 'Now the first device found'
		}))}
		value=""
		onChange={(bandId) => {
			givingOpen = false;
			giveTo(bandId);
		}}
	/>
{/if}

{#if pending}
	<Confirm
		bind:open={confirmOpen}
		title={pending.title}
		description={pending.description}
		action={pending.action}
		destructive={pending.destructive}
		onConfirm={() => pending && start(pending.run)}
	/>
{/if}
