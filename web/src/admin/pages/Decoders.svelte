<script lang="ts">
	import Plus from '@lucide/svelte/icons/plus';
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type DecoderStatus, type InstalledModule, type ModulesView, type SpotStatus } from '../api';
	import { ago } from '../lib/format';
	import Columns from '../components/Columns.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import TextRow from '../components/TextRow.svelte';
	import { Button } from '../components/ui/button/index';
	import { Confirm } from '../components/ui/confirm/index';
	import * as Dialog from '../components/ui/dialog/index';
	import { Switch } from '../components/ui/switch/index';
	import {
		channelKey,
		decoderSection,
		formatDial,
		parseChannels,
		suggestedChannels,
		validDecoderId,
		withModuleSettings,
		type Channel,
		type SettingSpec
	} from '../lib/decoders';
	import { Segmented } from '../components/ui/segmented/index';
	import { readSection, sectionNames, writeSection } from '../lib/ini-sections';
	import { live } from '../lib/live.svelte';

	/*
	Decoders: programs that listen to a few kilohertz of a band each and say what they decoded, such
	as FT8. Each is a [decoder:<id>] section of the configuration; this page writes those sections
	and the receiver applies them at once, restarting only the decoder that changed. What it shows
	of each is what the receiver reports: whether the module runs, what went to it and what came
	back, and its last words.
	*/
	interface Configured {
		id: string;
		values: Map<string, string>;
		channels: Channel[];
	}

	let configText = $state<string | null>(null);
	let statuses = $state<DecoderStatus[]>([]);
	let kept = $state(0);
	let spots = $state<SpotStatus | null>(null);
	let modules = $state<InstalledModule[]>([]);
	let view = $state<ModulesView | null>(null);
	let loadError = $state('');
	let saving = $state(false);

	const configured = $derived.by((): Configured[] => {
		const text = configText;
		if (text === null) return [];
		return sectionNames(text, 'decoder').filter((name, index, all) => all.indexOf(name) === index).map((name) => {
			const values = readSection(text, name) ?? new Map<string, string>();
			return { id: name.slice('decoder:'.length), values, channels: parseChannels(values.get('channels') ?? '') ?? [] };
		});
	});
	const decoderModules = $derived(
		modules.filter((module) => module.versions.find((version) => version.version === module.active)?.kind === 'decoder')
	);
	const bands = $derived(live.state?.bands ?? []);

	async function loadStatus() {
		try {
			const answer = await api.decoders();
			statuses = answer.decoders;
			kept = answer.kept;
			spots = answer.spots ?? null;
		} catch (problem) {
			loadError = (problem as ApiError).message;
		}
	}

	async function load() {
		try {
			const [config, installed] = await Promise.all([api.readConfig(), api.modules()]);
			configText = config.text;
			modules = installed.installed;
			view = installed;
			loadError = '';
		} catch (problem) {
			loadError = (problem as ApiError).message;
		}
		await loadStatus();
	}

	onMount(() => {
		void load();
		const timer = window.setInterval(loadStatus, 5000);
		return () => window.clearInterval(timer);
	});

	/**
	 * Writes section `id` through the config editor's own path, then reloads. `change` gets the
	 * section as the file has it now, read afresh so an edit made in the editor since this page
	 * loaded is kept, and returns what it should become (null removes it).
	 */
	async function save(
		id: string,
		change: (current: Map<string, string> | null) => Map<string, string> | null,
		done: string
	): Promise<boolean> {
		saving = true;
		try {
			const { text } = await api.readConfig();
			const name = `decoder:${id}`;
			const answer = await api.writeConfig(writeSection(text, name, change(readSection(text, name))));
			const waiting = answer.decoders_waiting?.[id];
			if (waiting) toast.warning(`${done}. It applies when the receiver restarts: ${waiting}.`);
			else toast.success(done);
			await load();
			return true;
		} catch (problem) {
			toast.error((problem as ApiError).message);
			return false;
		} finally {
			saving = false;
		}
	}

	function bandName(id: string): string {
		return bands.find((band) => band.id === id)?.name ?? id;
	}

	// Turning FT8 on, for a receiver that decodes nothing yet: one button that installs Fern-FT8
	// from the module catalog when it is not there, and decodes the FT8 frequencies the bands
	// cover. The catalog is the file's [modules] catalog: the panel installs only from the
	// repositories the owner of the machine listed.
	const FT8_REPOSITORY = 'steven9101/fern-ft8';
	const ft8Channels = $derived(suggestedChannels(bands, 'ft8'));
	const ft8Installed = $derived(decoderModules.find((module) => module.id === 'ft8'));
	const ft8Listed = $derived(view?.catalog.some((repository) => repository.toLowerCase() === FT8_REPOSITORY) ?? false);
	const ft8Catalog = $derived(view?.available.find((repository) => repository.repository.toLowerCase() === FT8_REPOSITORY));
	const ft8Release = $derived(
		ft8Catalog?.releases?.find((release) => release.asset && release.id === 'ft8' && !release.prerelease) ??
			ft8Catalog?.releases?.find((release) => release.asset && release.id === 'ft8')
	);
	let enabling = $state('');

	async function waitForJobs() {
		for (let i = 0; i < 600; i++) {
			view = await api.modules();
			if (!view.jobs.some((job) => job.state === 'queued' || job.state === 'running')) return view;
			await new Promise((resolve) => setTimeout(resolve, 1000));
		}
		throw new Error('The receiver is still busy with modules; try again in a minute.');
	}

	async function turnOnFt8() {
		try {
			if (!ft8Installed) {
				if (!ft8Release && ft8Listed && !ft8Catalog?.fetched_ms) {
					enabling = 'Looking for Fern-FT8…';
					view = await api.checkModules();
					await waitForJobs();
				}
				const release = ft8Release;
				if (!release || !ft8Catalog) {
					toast.error('No Fern-FT8 package for this receiver was found in the module catalog.');
					return;
				}
				enabling = `Installing Fern-FT8 ${release.version}…`;
				view = await api.installModule(ft8Catalog.repository, release.tag, release.asset!, true);
				const after = await waitForJobs();
				const failed = after.jobs.find((job) => job.state === 'failed');
				if (failed && !after.installed.some((module) => module.id === 'ft8')) {
					toast.error(failed.message ?? 'Fern-FT8 could not be installed.');
					return;
				}
				modules = after.installed;
			}
			enabling = 'Turning on FT8…';
			const channels = ft8Channels;
			await save(freeName(), () => decoderSection({ module: 'ft8', channels, public: false }, null),
				`FT8 decoding on ${channels.length} ${channels.length === 1 ? 'channel' : 'channels'}`);
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			enabling = '';
		}
	}

	// What the reporting switch says under it: what it needs, how it went.
	function spotDetail(on: boolean): string {
		if (!on) return 'Operators everywhere see who heard them, on pskreporter.info';
		if (spots?.problem) return `Not reporting: ${spots.problem}`;
		if (spots?.error) return `Last attempt failed: ${spots.error}`;
		if (spots?.last_sent_ms) return `${spots.sent.toLocaleString()} spots sent, last ${ago(spots.last_sent_ms)}; every five minutes`;
		return spots?.waiting ? `${spots.waiting} spots waiting; they go every five minutes` : 'Sends every five minutes, once decodes arrive';
	}

	const stateLabel: Record<string, string> = {
		starting: 'Starting',
		running: 'Decoding',
		waiting: 'Waiting to try again',
		stopped: 'Stopped'
	};

	function stateTone(state: string | undefined): string {
		if (state === 'waiting' || state === 'starting') return 'text-warning';
		if (state === 'stopped' || state === undefined) return 'text-destructive';
		return 'text-muted-foreground';
	}

	// The form, for a new decoder (editing null) or an existing one.
	let formOpen = $state(false);
	let editing = $state<string | null>(null);
	let formId = $state('');
	let formModule = $state('');
	let formPublic = $state(false);
	let formChannels = $state<string[]>([]);
	let formOther = $state('');
	let formError = $state('');
	// The module's own settings, as `module.<key>` in the section, by key.
	let formSettings = $state<Record<string, string>>({});
	const formSpecs = $derived.by((): SettingSpec[] => {
		const module = modules.find((m) => m.id === formModule);
		const active = module?.versions.find((v) => v.version === module.active);
		return (active?.settings ?? []) as SettingSpec[];
	});
	const formModuleName = $derived(
		modules.find((m) => m.id === formModule)?.versions.find((v) => v.version === modules.find((m) => m.id === formModule)?.active)?.name ?? formModule
	);
	function settingValue(spec: SettingSpec): string {
		return formSettings[spec.key] ?? (spec.default === undefined ? '' : String(spec.default));
	}
	// A short range of whole numbers or a few choices reads best as buttons.
	function buttonsFor(spec: SettingSpec): { value: string; label: string }[] | null {
		if (spec.type === 'choice' && (spec.choices ?? []).length <= 4) {
			return (spec.choices ?? []).map((c) => ({ value: c, label: c }));
		}
		if (spec.type === 'number' && spec.min !== undefined && spec.max !== undefined &&
			Number.isInteger(spec.min) && Number.isInteger(spec.max) && spec.max - spec.min <= 4) {
			return Array.from({ length: spec.max - spec.min + 1 }, (_, i) => ({ value: String(spec.min! + i), label: String(spec.min! + i) }));
		}
		return null;
	}

	const formMode = $derived((editing ? configured.find((d) => d.id === editing)?.values.get('mode') : undefined) ?? 'ft8');
	const offered = $derived.by(() => {
		const suggested = suggestedChannels(bands, formMode);
		const current = editing ? (configured.find((d) => d.id === editing)?.channels ?? []) : [];
		const all = [...suggested];
		for (const channel of current) if (!all.some((c) => channelKey(c) === channelKey(channel))) all.push(channel);
		return all;
	});

	function openForm(decoder: Configured | null) {
		editing = decoder?.id ?? null;
		formId = decoder?.id ?? freeName();
		formModule = decoder?.values.get('module') ?? decoderModules[0]?.id ?? '';
		formPublic = decoder ? decoder.values.get('public') === 'yes' : false;
		formChannels = decoder ? decoder.channels.map(channelKey) : suggestedChannels(bands, 'ft8').map(channelKey);
		formOther = '';
		formError = '';
		formSettings = {};
		for (const [key, value] of decoder?.values ?? []) {
			if (key.startsWith('module.')) formSettings[key.slice('module.'.length)] = value;
		}
		formOpen = true;
	}

	// ft8, then ft8-2, ft8-3: a name that is free, which the operator may keep.
	function freeName(): string {
		for (let n = 1; ; n++) {
			const name = n === 1 ? 'ft8' : `ft8-${n}`;
			if (!configured.some((d) => d.id === name)) return name;
		}
	}

	function toggleChannel(key: string, on: boolean) {
		formChannels = on ? [...formChannels, key] : formChannels.filter((k) => k !== key);
	}

	async function submitForm() {
		const id = editing ?? formId.trim();
		const extra = parseChannels(formOther);
		if (!validDecoderId(id)) {
			formError = 'The name takes lowercase letters, digits and -, up to 32.';
		} else if (!editing && configured.some((d) => d.id === id)) {
			formError = `There is already a decoder called ${id}.`;
		} else if (!formModule) {
			formError = 'Choose the module that decodes.';
		} else if (extra === null) {
			formError = 'Write other channels as band:frequency, such as 40m:7074000.';
		} else {
			const keys = [...new Set([...formChannels, ...extra.map(channelKey)])];
			const channels = parseChannels(keys.join(' ')) ?? [];
			if (channels.length === 0) {
				formError = 'Choose at least one channel.';
			} else {
				formError = '';
				const form = { module: formModule, channels, public: formPublic };
				const keepOthers = editing !== null;
				const specs = formSpecs;
				const chosen = { ...formSettings };
				try {
					withModuleSettings(new Map(), specs, chosen);
				} catch (problem) {
					formError = (problem as Error).message;
					return;
				}
				if (await save(id, (current) => withModuleSettings(decoderSection(form, keepOthers ? current : null), specs, chosen),
					editing ? `Decoder ${id} changed` : `Decoder ${id} added`)) {
					formOpen = false;
				}
				return;
			}
		}
	}

	let removing = $state<string | null>(null);
	let confirmOpen = $state(false);
	let logOf = $state<string | null>(null);
	let logOpen = $state(false);
	const shownLog = $derived(statuses.find((s) => s.id === logOf)?.log ?? []);

	async function restart(id: string) {
		try {
			await api.restartDecoder(id);
			toast.success(`Decoder ${id} restarts`);
			await loadStatus();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}
</script>

<PageHeader
	title="Decoders"
	description="Programs that listen to a few kilohertz of a band and decode what they hear, such as FT8. Listeners see a decoder's results when you make it public."
>
	{#snippet actions()}
		<Button size="lg" class="rounded-full px-4" disabled={decoderModules.length === 0 || configText === null} onclick={() => openForm(null)}>
			<Plus /> Add decoder
		</Button>
	{/snippet}
</PageHeader>

{#if loadError && configText === null}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if configText === null}
	<div class="flex max-w-2xl flex-col gap-3">
		<div class="shimmer h-40 rounded-2xl bg-card"></div>
		<div class="shimmer h-24 rounded-2xl bg-card"></div>
	</div>
{:else}
	<Columns asideLabel="What was decoded">
		{#snippet main()}
			{#if configured.length === 0}
				<SettingsGroup title="FT8">
					{#snippet footer()}
						{#if ft8Channels.length === 0}
							None of your bands covers an FT8 frequency with 4 kHz to spare. Add a band that does, such
							as 7,074 or 14,074 kHz, and come back.
						{:else if ft8Installed || ft8Release}
							Decodes stay on this page until you make them public. FernSDR runs the decoder at the lowest
							CPU priority, in a sandbox that keeps it from files and TCP as far as this machine's kernel
							allows.
						{:else if !ft8Listed}
							Fern-FT8 is not in this receiver's module catalog. Add Steven9101/Fern-FT8 to catalog under
							[modules] in the configuration file on the machine, or install a package there with
							fernsdr --install-module.
						{:else if !view?.downloader}
							The receiver cannot download modules: curl is not installed on the machine.
						{:else if ft8Catalog?.error}
							<span class="text-destructive">{ft8Catalog.error}</span>
						{:else if ft8Catalog?.fetched_ms}
							Fern-FT8 has no release for {view?.platform || 'this machine'} yet.
						{:else}
							Fern-FT8 comes from the module catalog; turning it on looks it up and installs it.
						{/if}
					{/snippet}
					<div class="flex flex-col gap-3 px-4 py-4">
						<p class="text-[15px] leading-relaxed">
							Decode FT8 around the clock
							{#if ft8Channels.length > 0}
								on {ft8Channels.map((c) => `${bandName(c.band)} ${formatDial(c.dial)}`).join(', ')}.
							{:else}
								on the bands this receiver has.
							{/if}
						</p>
						<Button
							size="lg"
							class="h-11 self-start rounded-full px-5"
							disabled={ft8Channels.length === 0 || !!enabling || saving ||
								(!ft8Installed && !ft8Release && (!ft8Listed || !view?.downloader || !!ft8Catalog?.fetched_ms))}
							onclick={turnOnFt8}
						>
							{#if enabling}<LoaderCircle class="animate-spin" />{enabling}
							{:else if ft8Installed}Turn on FT8
							{:else}Install Fern-FT8 and turn on FT8{/if}
						</Button>
					</div>
				</SettingsGroup>
			{/if}

			{#each configured as decoder (decoder.id)}
				{@const status = statuses.find((s) => s.id === decoder.id)}
				<SettingsGroup title={decoder.id}>
					{#snippet footer()}
						{#if status?.message}<span class={status.state === 'running' ? '' : 'text-warning'}>{status.message}</span>
						{:else}Module {decoder.values.get('module')}{status?.version ? ` ${status.version}` : ''}, {decoder.channels.length}
							{decoder.channels.length === 1 ? 'channel' : 'channels'}.{/if}
					{/snippet}
					<SettingsRow label="State" detail={status?.restarts ? `Restarted ${status.restarts} ${status.restarts === 1 ? 'time' : 'times'}` : undefined}>
						{#snippet control()}
							<span class="text-sm font-medium {stateTone(status?.state)}">{stateLabel[status?.state ?? ''] ?? 'Not running'}</span>
						{/snippet}
					</SettingsRow>
					<SettingsRow label="Report spots to PSK Reporter" detail={spotDetail(decoder.values.get('report') === 'pskreporter')}>
						{#snippet control()}
							<Switch
								bind:checked={
									() => decoder.values.get('report') === 'pskreporter',
									(checked) =>
										void save(
											decoder.id,
											(current) => current && new Map(current).set('report', checked ? 'pskreporter' : 'none'),
											checked ? `${decoder.id} reports to PSK Reporter` : `${decoder.id} reports nothing`
										)
								}
								disabled={saving}
								aria-label="Report {decoder.id}'s spots to PSK Reporter"
							/>
						{/snippet}
					</SettingsRow>
					<SettingsRow label="Shown to listeners" detail="In the Decodes tab and at /api/decodes">
						{#snippet control()}
							<Switch
								bind:checked={
									() => decoder.values.get('public') === 'yes',
									(checked) =>
										void save(
											decoder.id,
											(current) => current && new Map(current).set('public', checked ? 'yes' : 'no'),
											checked ? `Listeners see ${decoder.id} from now on` : `${decoder.id} is for you only`
										)
								}
								disabled={saving}
								aria-label="Show {decoder.id} to listeners"
							/>
						{/snippet}
					</SettingsRow>
					{#each decoder.channels as channel (channelKey(channel))}
						{@const stats = status?.channels.find((c) => c.band === channel.band && Math.round(c.dial) === channel.dial)}
						<SettingsRow
							label="{bandName(channel.band)} · {formatDial(channel.dial)}"
							detail={stats
								? `${stats.decodes.toLocaleString()} decoded${stats.frames_dropped ? `, ${stats.frames_dropped.toLocaleString()} frames lost` : ''}`
								: 'Not running'}
						/>
					{/each}
					{#if status?.rejected}
						<SettingsRow label="Refused decodes" value={status.rejected.toLocaleString()} detail={status.last_rejection} />
					{/if}
					<SettingsRow label="Change" tone="font-medium" onclick={() => openForm(decoder)} />
					{#if status}
						<SettingsRow label="Module log" value={`${status.log.length} lines`} onclick={() => { logOf = decoder.id; logOpen = true; }} />
						<SettingsRow label="Restart" tone="font-medium" onclick={() => restart(decoder.id)} />
					{/if}
					<SettingsRow label="Remove" tone="font-medium text-destructive" onclick={() => { removing = decoder.id; confirmOpen = true; }} />
				</SettingsGroup>
			{/each}
		{/snippet}

		{#snippet aside()}
			<SettingsGroup title="Decodes" footer="Kept for a day, up to 20,000. Listeners see only those of public decoders.">
				<SettingsRow label="Kept now" value={kept.toLocaleString()} />
				<SettingsRow label="Decoder modules" value={decoderModules.length ? decoderModules.map((m) => m.id).join(', ') : 'None'} />
			</SettingsGroup>
			<SettingsGroup title="How a channel works">
				<p class="px-4 py-3 text-[13px] leading-relaxed text-muted-foreground">
					A channel is 4 kHz of the band starting at its dial frequency, as a transceiver on USB would hear
					it. It costs the receiver one small filter, whether or not anyone listens. Decoder modules run
					with no access to files or the network.
				</p>
			</SettingsGroup>
		{/snippet}
	</Columns>
{/if}

<Dialog.Root bind:open={formOpen}>
	<Dialog.Content class="max-h-[88dvh] gap-4 overflow-y-auto rounded-3xl p-3 pt-5 sm:max-w-lg">
		<div class="flex flex-col gap-1 px-4 pr-12">
			<Dialog.Title class="text-lg font-semibold">{editing ? `Change ${editing}` : 'Add a decoder'}</Dialog.Title>
			<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">
				Applied as soon as you save; other decoders keep running.
			</Dialog.Description>
		</div>
		<form class="flex flex-col gap-4" onsubmit={(event) => { event.preventDefault(); void submitForm(); }}>
			<SettingsGroup>
				{#if !editing}
					<TextRow label="Name" bind:value={formId} placeholder="Lowercase letters and digits" autocapitalize="none" />
				{/if}
				{#if decoderModules.length > 1}
					{#each decoderModules as module (module.id)}
						<label class="flex min-h-13 items-center gap-3 px-4 py-2.5">
							<input type="radio" name="decoder-module" value={module.id} bind:group={formModule} class="size-4 accent-foreground" />
							<span class="text-[15px]">{module.versions.find((v) => v.version === module.active)?.name ?? module.id}</span>
						</label>
					{/each}
				{:else}
					<SettingsRow label="Module" value={formModule ? formModuleName : 'None installed'} />
				{/if}
				<SettingsRow label="Shown to listeners">
					{#snippet control()}<Switch bind:checked={formPublic} aria-label="Shown to listeners" />{/snippet}
				</SettingsRow>
			</SettingsGroup>
			{#if formSpecs.length > 0}
				<SettingsGroup title="{formModuleName} settings">
					{#each formSpecs as spec (spec.key)}
						{@const buttons = buttonsFor(spec)}
						{#if spec.type === 'boolean'}
							<SettingsRow label={spec.label} detail={spec.help}>
								{#snippet control()}
									<Switch
										bind:checked={() => settingValue(spec) === 'true', (on) => (formSettings[spec.key] = on ? 'true' : 'false')}
										aria-label={spec.label}
									/>
								{/snippet}
							</SettingsRow>
						{:else if buttons}
							<div class="flex flex-col gap-2 px-4 py-3">
								<span class="text-[15px]">{spec.label}</span>
								<Segmented
									label={spec.label}
									options={buttons}
									value={settingValue(spec)}
									onChange={(value) => (formSettings[spec.key] = value)}
								/>
								{#if spec.help}<span class="text-[13px] leading-relaxed text-muted-foreground">{spec.help}</span>{/if}
							</div>
						{:else}
							<TextRow
								label={spec.label}
								bind:value={() => settingValue(spec), (value) => (formSettings[spec.key] = value)}
								inputmode={spec.type === 'number' ? 'decimal' : 'text'}
								placeholder={spec.default === undefined ? 'Not set' : String(spec.default)}
							/>
						{/if}
					{/each}
				</SettingsGroup>
			{/if}
			<SettingsGroup title="Channels" footer={offered.length === 0 ? `None of your bands covers an ${formMode.toUpperCase()} frequency with 4 kHz to spare; add one below.` : `The ${formMode.toUpperCase()} frequencies your bands cover.`}>
				{#each offered as channel (channelKey(channel))}
					{@const key = channelKey(channel)}
					<label class="flex min-h-13 items-center gap-3 px-4 py-2.5">
						<input type="checkbox" class="size-4 accent-foreground" checked={formChannels.includes(key)}
							onchange={(event) => toggleChannel(key, event.currentTarget.checked)} />
						<span class="flex-1 text-[15px]">{bandName(channel.band)}</span>
						<span class="text-[15px] text-muted-foreground tabular">{formatDial(channel.dial)}</span>
					</label>
				{/each}
				<TextRow label="Others" bind:value={formOther} placeholder="band:frequency" autocapitalize="none" />
			</SettingsGroup>
			{#if formError}<p class="px-4 text-sm text-destructive" role="alert">{formError}</p>{/if}
			<div class="flex flex-col gap-2 pb-1">
				<Button type="submit" size="lg" class="h-11 rounded-full text-[15px]" disabled={saving}>
					{#if saving}<LoaderCircle class="animate-spin" />{/if}
					{editing ? 'Save' : 'Add'}
				</Button>
			</div>
		</form>
	</Dialog.Content>
</Dialog.Root>

<Dialog.Root bind:open={logOpen}>
	<Dialog.Content class="max-h-[88dvh] gap-3 overflow-y-auto rounded-3xl p-3 pt-5 sm:max-w-2xl">
		<div class="flex flex-col gap-1 px-4 pr-12">
			<Dialog.Title class="text-lg font-semibold">What {logOf} said</Dialog.Title>
			<Dialog.Description class="text-sm text-muted-foreground">The module's last 200 lines, newest last.</Dialog.Description>
		</div>
		{#if shownLog.length}
			<pre class="max-h-[60dvh] overflow-auto rounded-2xl bg-card p-4 text-[12px] leading-relaxed whitespace-pre-wrap">{shownLog.join('\n')}</pre>
		{:else}
			<p class="px-4 pb-3 text-[15px] text-muted-foreground">Nothing yet.</p>
		{/if}
	</Dialog.Content>
</Dialog.Root>

<Confirm
	bind:open={confirmOpen}
	title="Remove {removing}?"
	description="Its section leaves the configuration and the decoder stops. The decodes it made stay until they age out."
	action="Remove"
	destructive
	onConfirm={() => removing && save(removing, () => null, `Decoder ${removing} removed`)}
/>
