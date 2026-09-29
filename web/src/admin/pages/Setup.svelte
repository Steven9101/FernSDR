<script lang="ts">
	import Check from '@lucide/svelte/icons/check';
	import ChevronDown from '@lucide/svelte/icons/chevron-down';
	import Copy from '@lucide/svelte/icons/copy';
	import ExternalLink from '@lucide/svelte/icons/external-link';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type HardwareView, type InstalledModule, type ModulesView, type UsbRadio } from '../api';
	import LocatorPicker from '../components/LocatorPicker.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import TextRow from '../components/TextRow.svelte';
	import { Button } from '../components/ui/button/index';
	import { Switch } from '../components/ui/switch/index';
	import {
		bandSection,
		dayAndNight,
		freeId,
		suggestionsFor,
		targetsFrom,
		type Suggestion,
		type SuggestionGroup,
		type Tuning
	} from '../lib/band-suggestions';
	import { readSection, sectionNames, writeSection } from '../lib/ini-sections';
	import { copyText } from '../../util/clipboard';
	import { bandPlan, loadBandPlan } from '../../state/bandplan';
	import { finishSetup, setupDone, setupStep, SETUP_STEPS, type SetupStep } from '../lib/setup';

	/*
	Setting a receiver up, from the first sign-in: one question at a time, in the order an operator
	can answer them, each saved as it is answered so the flow can be left and taken up again (after
	the receiver restarts, too). Everything here can also be done on the other pages; this only puts
	it in order and says what each thing is for.
	*/

	const titles: Record<SetupStep, string> = {
		password: 'Your password',
		station: 'Your station',
		radio: 'Your radio',
		bands: 'What to listen to',
		listeners: 'Who can listen',
		done: 'Ready'
	};
	let step = $state<SetupStep>(setupStep());
	const index = $derived(SETUP_STEPS.indexOf(step));

	function go(next: SetupStep) {
		step = next;
		setupStep(next);
		window.scrollTo({ top: 0 });
	}
	const nextStep = () => go(SETUP_STEPS[Math.min(index + 1, SETUP_STEPS.length - 1)]);

	// --- 1. password -------------------------------------------------------------------------

	let choosing = $state(false);
	let current = $state('');
	let next = $state('');
	let again = $state('');
	let passwordBusy = $state('');
	const passwordProblem = $derived(
		next && next.length < 12 ? `${next.length} characters; at least 12.` : again && again !== next ? 'The two differ.' : ''
	);
	async function savePassword() {
		try {
			await api.changePassword(current, next, (message) => (passwordBusy = message));
			toast.success('Your own password is set.');
			current = next = again = '';
			nextStep();
		} catch (problem) {
			const error = problem as ApiError;
			toast.error(error.status === 403 ? 'The current password is not right.' : error.message);
		} finally {
			passwordBusy = '';
		}
	}

	// --- 2. station --------------------------------------------------------------------------

	let station = $state({ name: '', operator: '', location: '', grid: '', band_plan: 'auto', public_host: '', sdr_list: false });
	let mapOpen = $state(false);
	let stationBusy = $state(false);
	async function loadStation() {
		const answer = (await api.readStation()).station as Partial<typeof station>;
		station = { ...station, ...answer };
		if (station.name === 'FernSDR') station.name = '';
	}
	async function saveStation() {
		stationBusy = true;
		try {
			const { name, operator, location, grid } = $state.snapshot(station);
			await api.writeStation({ name: name.trim() || 'FernSDR', operator, location, grid });
			nextStep();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			stationBusy = false;
		}
	}

	// --- 3. radio ----------------------------------------------------------------------------

	let hardware = $state<HardwareView | null>(null);
	let modules = $state<ModulesView | null>(null);
	let looking = $state(false);
	let installing = $state('');
	let chosen = $state<UsbRadio | null>(null);

	async function look() {
		looking = true;
		try {
			[hardware, modules] = await Promise.all([api.hardware(), api.modules()]);
			const usable = hardware.radios.filter((radio) => radio.module && installed(radio.module));
			if (!chosen || !hardware.radios.some((radio) => radio.port === chosen?.port)) chosen = usable[0] ?? null;
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			looking = false;
		}
	}

	function installed(module: string): InstalledModule | undefined {
		return modules?.installed.find((entry) => entry.id === module);
	}

	async function waitForJobs(): Promise<ModulesView> {
		for (let i = 0; i < 600; i++) {
			const view = await api.modules();
			if (!view.jobs.some((job) => job.state === 'queued' || job.state === 'running')) return view;
			await new Promise((resolve) => setTimeout(resolve, 1000));
		}
		throw new Error('The receiver is still busy with modules; try again in a minute.');
	}

	async function install(radio: UsbRadio) {
		installing = radio.module;
		try {
			let view = modules ?? (await api.modules());
			const find = () =>
				view.available.flatMap((repository) => (repository.releases ?? []).map((release) => ({ repository, release })))
					.find(({ release }) => release.id === radio.module && release.asset && !release.prerelease);
			if (!find()) {
				view = await api.checkModules();
				view = await waitForJobs();
			}
			const found = find();
			if (!found) {
				toast.error(`No ${radio.name} module for this computer was found in the module catalog.`);
				return;
			}
			await api.installModule(found.repository.repository, found.release.tag, found.release.asset!, true);
			view = await waitForJobs();
			const failed = view.jobs.find((job) => job.state === 'failed');
			if (failed && !view.installed.some((module) => module.id === radio.module)) {
				toast.error(failed.message ?? 'The module could not be installed.');
				return;
			}
			modules = view;
			chosen = radio;
			toast.success(`The ${radio.name} module is installed.`);
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			installing = '';
		}
	}

	const requires = $derived.by(() => {
		const module = chosen ? installed(chosen.module) : undefined;
		return module?.versions.find((version) => version.version === module.active)?.requires ?? [];
	});
	const driverInTheWay = (radio: UsbRadio) => hardware?.drivers.find((driver) => driver.module === radio.module);

	// --- 4. bands ----------------------------------------------------------------------------

	let suggestions = $state<Suggestion[]>([]);
	// The first group is open and the rest are one line each: forty bands at once is a list to
	// read, not a choice to make.
	let opened = $state<string[]>([]);
	let picked = $state<string[]>([]);
	let replaceTest = $state(true);
	let applying = $state('');
	const groupNames: Record<SuggestionGroup, string> = {
		everything: 'Everything at once',
		amateur: 'Amateur radio',
		broadcast: 'Broadcast',
		aviation: 'Aviation and marine',
		other: 'Other'
	};
	const tuning = $derived.by((): Tuning | undefined => {
		const module = chosen ? installed(chosen.module) : undefined;
		return module?.versions.find((version) => version.version === module.active)?.tuning;
	});

	async function loadSuggestions() {
		if (!tuning) {
			suggestions = [];
			return;
		}
		await loadBandPlan(station.band_plan, station.grid, 2e9);
		const plan = bandPlan.value;
		suggestions = suggestionsFor(tuning, plan ? targetsFrom(plan.bands, plan.segments) : []);
		if (opened.length === 0 && suggestions.length) {
			const groups = suggestions.map((suggestion) => suggestion.group);
			opened = [groups.includes('everything') ? 'everything' : groups.includes('amateur') ? 'amateur' : groups[0]];
		}
	}

	function toggle(id: string) {
		if (picked.includes(id)) picked = picked.filter((entry) => entry !== id);
		// One radio runs one band at a time: two take turns, by day and by night.
		else picked = [...picked, id].slice(-2);
	}
	const pickedSuggestions = $derived(suggestions.filter((suggestion) => picked.includes(suggestion.id)));
	const turns = $derived(pickedSuggestions.length === 2 ? dayAndNight(pickedSuggestions[0], pickedSuggestions[1]) : null);

	function deviceSetting(): string {
		// Only where there is more than one radio for the module: a single one is found by itself.
		const same = hardware?.radios.filter((radio) => radio.module === chosen?.module) ?? [];
		if (same.length < 2 || !chosen) return '';
		return chosen.serial ? `serial:${chosen.serial}` : `index:${same.indexOf(chosen)}`;
	}

	async function applyBands() {
		if (!chosen || pickedSuggestions.length === 0) return;
		applying = 'Writing the bands…';
		try {
			let { text } = await api.readConfig();
			const taken = sectionNames(text, 'band').map((name) => name.slice('band:'.length));
			if (replaceTest) {
				for (const name of sectionNames(text, 'band')) {
					if ((readSection(text, name)?.get('source') ?? 'test') === 'test') {
						text = writeSection(text, name, null);
						taken.splice(taken.indexOf(name.slice('band:'.length)), 1);
					}
				}
			}
			pickedSuggestions.forEach((suggestion, i) => {
				const id = freeId(suggestion.id, taken);
				taken.push(id);
				text = writeSection(text, `band:${id}`, bandSection(suggestion, chosen!.module, deviceSetting(), turns?.[i]));
			});
			await api.writeConfig(text);
			await restartAndContinue();
		} catch (problem) {
			toast.error((problem as ApiError).message);
			applying = '';
		}
	}

	// A new band starts with the receiver. Where it can restart itself, it does, and the flow waits
	// for it to answer again; the page then signs in afresh, on the step after.
	async function restartAndContinue() {
		const view = hardware ?? (await api.hardware());
		// A band added later, from the Bands page, needs no questions about listeners again.
		setupStep(setupDone() ? 'done' : 'listeners');
		if (!view.can_restart) {
			applying = '';
			restartNote = view.restart_note ?? 'Restart the receiver to start the new bands.';
			return;
		}
		applying = 'Restarting the receiver…';
		await api.restart();
		await new Promise((resolve) => setTimeout(resolve, 1500));
		for (let i = 0; i < 90; i++) {
			try {
				const answer = await fetch('/api/admin/session', { cache: 'no-store' });
				if (answer.ok) break;
			} catch {
				// Not back yet.
			}
			await new Promise((resolve) => setTimeout(resolve, 1000));
		}
		// Sessions end with the receiver: signing in again lands here, on the next step.
		location.reload();
	}
	let restartNote = $state('');

	// --- 5. listeners --------------------------------------------------------------------------

	let listenersBusy = $state(false);
	async function saveListeners() {
		listenersBusy = true;
		try {
			await api.writeStation({ public_host: station.public_host.trim(), sdr_list: station.sdr_list });
			nextStep();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			listenersBusy = false;
		}
	}

	onMount(() => {
		void loadStation().catch(() => {});
		void look();
	});

	$effect(() => {
		if (step === 'bands') void loadSuggestions();
	});

	const receiverAddress = $derived(`${location.protocol}//${location.host}/`);
	const sdrplayCommand = 'sudo sh ~/Downloads/SDRplay_RSP_API-Linux-*.run';
	async function copyCommand() {
		if (await copyText(sdrplayCommand)) toast.success('Copied');
		else toast.error('Select the command and copy it instead');
	}
</script>

<div class="mx-auto flex max-w-2xl flex-col gap-8 pt-6 pb-10 md:pt-10">
	<header class="flex flex-col gap-4">
		{#if step !== 'done'}<p class="text-[13px] text-muted-foreground tabular sm:hidden">Step {index + 1} of {SETUP_STEPS.length - 1}</p>{/if}
		<ol class="hidden flex-wrap gap-x-4 gap-y-1 text-[13px] text-muted-foreground sm:flex" aria-label="Steps">
			{#each SETUP_STEPS.slice(0, -1) as entry, n (entry)}
				<li class="flex items-center gap-1.5 {entry === step ? 'font-medium text-foreground' : ''}" aria-current={entry === step ? 'step' : undefined}>
					<span class="grid size-5 place-items-center rounded-full text-[11px] tabular {n < index ? 'bg-primary text-primary-foreground' : entry === step ? 'bg-foreground text-background' : 'bg-muted'}">
						{#if n < index}<Check size={12} />{:else}{n + 1}{/if}
					</span>
					{titles[entry]}
				</li>
			{/each}
		</ol>
		<h1 class="text-[1.875rem] leading-tight font-bold tracking-[-0.035em] md:text-[2.125rem]">{titles[step]}</h1>
	</header>

	{#if step === 'password'}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			You signed in with the password the installer made up. Keep it if it is written down somewhere safe, or choose
			one of your own now. It never leaves this browser: the receiver only ever sees proof that you know it.
		</p>
		{#if choosing}
			<SettingsGroup footer={passwordProblem || 'At least 12 characters.'}>
				<TextRow label="Current" type="password" autocomplete="current-password" bind:value={current} placeholder="The one you signed in with" />
				<TextRow label="New" type="password" autocomplete="new-password" bind:value={next} placeholder="At least 12 characters" />
				<TextRow label="Again" type="password" autocomplete="new-password" bind:value={again} placeholder="The new one again" />
			</SettingsGroup>
		{/if}
		<div class="flex flex-wrap gap-3">
			{#if choosing}
				<Button size="lg" class="h-11 rounded-full px-5" disabled={!current || next.length < 12 || next !== again || passwordBusy !== ''} onclick={savePassword}>
					{passwordBusy || 'Set my password'}
				</Button>
			{:else}
				<Button size="lg" class="h-11 rounded-full px-5" onclick={() => (choosing = true)}>Choose my own</Button>
			{/if}
			<Button variant="secondary" size="lg" class="h-11 rounded-full px-5" onclick={nextStep}>Keep the one I have</Button>
		</div>
	{:else if step === 'station'}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			What listeners see at the top of the page. The location on the map sets the band plan, the local time of sunrise
			and sunset, and where the receiver shows in the directory.
		</p>
		<SettingsGroup>
			<TextRow label="Name" bind:value={station.name} placeholder="Such as Riverside WebSDR" />
			<TextRow label="Callsign" bind:value={station.operator} placeholder="Yours, if you have one" />
			<TextRow label="Place" bind:value={station.location} placeholder="Town and country" />
			<SettingsRow label="On the map" value={station.grid || 'Not set'} onclick={() => (mapOpen = true)} />
		</SettingsGroup>
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5" disabled={stationBusy} onclick={saveStation}>Save and go on</Button>
			<Button variant="ghost" size="lg" class="h-11 rounded-full px-5" onclick={() => go('password')}>Back</Button>
		</div>
		<LocatorPicker bind:open={mapOpen} grid={station.grid} onPick={(grid) => (station.grid = grid)} />
	{:else if step === 'radio'}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			Plug the radio into this computer. Each kind of radio has a small program, a module, that runs it; the receiver
			fetches it for you.
		</p>
		{#if !hardware}
			<div class="shimmer h-24 rounded-2xl bg-card"></div>
		{:else if hardware.radios.length === 0}
			<SettingsGroup title="No radio found" footer="FernSDR recognises RTL-SDR sticks, the RX-888 and SDRplay's RSPs by their USB ids.">
				<div class="flex flex-col gap-2 px-4 py-4 text-[15px] leading-relaxed">
					<p>Is it plugged into this computer, and not into a USB hub without its own power? Try another USB port, then look again.</p>
					<p class="text-muted-foreground">
						A radio FernSDR has no module for can still feed it through a pipe from its own program; the guide shows how.
						You can also <a class="text-foreground underline underline-offset-4" href="https://github.com/Steven9101/FernSDR/issues/new/choose" target="_blank" rel="noreferrer">ask for support for your radio</a>.
					</p>
				</div>
			</SettingsGroup>
		{:else}
			<SettingsGroup title="Found on this computer">
				{#each hardware.radios as radio (radio.port)}
					{@const module = radio.module ? installed(radio.module) : undefined}
					{@const driver = driverInTheWay(radio)}
					<div class="flex flex-wrap items-center justify-between gap-3 px-4 py-3.5">
						<div class="flex min-w-0 flex-col gap-0.5">
							<span class="text-[15px] font-medium">{radio.name}</span>
							<span class="text-[13px] text-muted-foreground">
								{#if !radio.module}
									No module yet. It can feed FernSDR through a pipe; see the guide.
								{:else if driver}
									Linux's TV driver holds it. Restart the computer once; FernSDR keeps that driver away from now on.
								{:else if module}
									Ready{radio.serial ? `, serial ${radio.serial}` : ''}
								{:else}
									Needs its module, about a megabyte
								{/if}
							</span>
						</div>
						{#if radio.module && !module}
							<Button size="lg" class="h-10 rounded-full px-4" disabled={installing !== ''} onclick={() => install(radio)}>
								{installing === radio.module ? 'Installing…' : 'Install'}
							</Button>
						{:else if module}
							<Button variant={chosen?.port === radio.port ? 'default' : 'secondary'} size="lg" class="h-10 rounded-full px-4" onclick={() => (chosen = radio)}>
								{chosen?.port === radio.port ? 'Chosen' : 'Use this one'}
							</Button>
						{/if}
					</div>
				{/each}
			</SettingsGroup>
		{/if}
		{#if chosen?.module === 'sdrplay' && requires.length}
			<SettingsGroup title="SDRplay's own software" footer="SDRplay does not allow others to pass their software on, so it comes from their site. The receiver finds it once it is installed.">
				<div class="flex flex-col gap-3 px-4 py-4 text-[15px] leading-relaxed">
					<p>1. Download the API for Linux from <a class="underline underline-offset-4" href="https://www.sdrplay.com/api/" target="_blank" rel="noreferrer">sdrplay.com/api <ExternalLink size={13} class="inline" /></a>.</p>
					<p>2. On this computer, open a terminal and run the installer you downloaded:</p>
					<div class="flex items-center gap-2 rounded-xl bg-muted px-3 py-2">
						<span class="min-w-0 flex-1 break-all text-[14px]">{sdrplayCommand}</span>
						<Button variant="ghost" size="icon" aria-label="Copy the command" onclick={copyCommand}><Copy /></Button>
					</div>
					<p>3. Come back here and look again.</p>
				</div>
			</SettingsGroup>
		{/if}
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5" disabled={!chosen} onclick={nextStep}>Go on with this radio</Button>
			<Button variant="secondary" size="lg" class="h-11 rounded-full px-5" disabled={looking} onclick={look}>{looking ? 'Looking…' : 'Look again'}</Button>
			<Button variant="ghost" size="lg" class="h-11 rounded-full px-5" onclick={() => go('listeners')}>Keep the test signal for now</Button>
		</div>
	{:else if step === 'bands'}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			Choose what {chosen?.name ?? 'the radio'} listens to. One radio shows one band at a time; choose two, and they take
			turns: the lower one at night, when it carries far, the higher one by day.
		</p>
		{#if !tuning}
			<SettingsGroup>
				<div class="px-4 py-4 text-[15px] leading-relaxed">
					This module does not say what its radio can do, so there is nothing to suggest. Add a band by hand under
					Configuration.
				</div>
			</SettingsGroup>
		{:else if suggestions.length === 0}
			<div class="shimmer h-40 rounded-2xl bg-card"></div>
		{:else}
			{#each Object.keys(groupNames) as group (group)}
				{@const inGroup = suggestions.filter((suggestion) => suggestion.group === group)}
				{@const chosenHere = inGroup.filter((suggestion) => picked.includes(suggestion.id)).length}
				{#if inGroup.length && !opened.includes(group)}
					<button
						type="button"
						class="pressable flex items-center justify-between rounded-2xl bg-card px-4 py-3.5 text-left"
						aria-expanded="false"
						onclick={() => (opened = [...opened, group])}
					>
						<span class="text-[15px] font-medium">{groupNames[group as SuggestionGroup]}</span>
						<span class="flex items-center gap-2 text-[13px] text-muted-foreground tabular">
							{chosenHere ? `${chosenHere} chosen · ` : ''}{inGroup.length}
							<ChevronDown size={16} />
						</span>
					</button>
				{:else if inGroup.length}
					<SettingsGroup title={groupNames[group as SuggestionGroup]}>
						{#each inGroup as suggestion (suggestion.id)}
							{@const on = picked.includes(suggestion.id)}
							{@const turn = turns ? turns[pickedSuggestions.findIndex((entry) => entry.id === suggestion.id)] : undefined}
							<SettingsRow
								label={suggestion.name}
								detail="{(suggestion.low / 1e6).toFixed(3)} to {(suggestion.high / 1e6).toFixed(3)} MHz{suggestion.whole ? '' : ', a part of the band'}{on && turn ? (turn === 'sunset-sunrise' ? ', at night' : ', by day') : ''}"
							>
								{#snippet control()}<Switch checked={on} onCheckedChange={() => toggle(suggestion.id)} aria-label={suggestion.name} />{/snippet}
							</SettingsRow>
						{/each}
					</SettingsGroup>
				{/if}
			{/each}
			<SettingsGroup>
				<SettingsRow label="Remove the test signal" detail="The synthetic band that is there until a radio is">
					{#snippet control()}<Switch bind:checked={replaceTest} aria-label="Remove the test signal" />{/snippet}
				</SettingsRow>
			</SettingsGroup>
		{/if}
		{#if restartNote}
			<SettingsGroup title="One more step">
				<p class="px-4 py-4 text-[15px] leading-relaxed">{restartNote} Then come back here.</p>
			</SettingsGroup>
		{/if}
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5" disabled={pickedSuggestions.length === 0 || applying !== ''} onclick={applyBands}>
				{applying || (pickedSuggestions.length === 2 ? 'Add both bands' : 'Add this band')}
			</Button>
			<Button variant="ghost" size="lg" class="h-11 rounded-full px-5" onclick={() => go('radio')}>Back</Button>
		</div>
	{:else if step === 'listeners'}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			Anyone who can reach this address can listen: <span class="font-medium text-foreground">{receiverAddress}</span>.
			At home that is everyone on your network. For listeners on the internet, your router has to pass the receiver's
			port on to this computer, or it runs on a server; the guide shows both.
		</p>
		<SettingsGroup footer="sdr-list.xyz is a public directory of web receivers. Listing needs the address people reach the receiver at from the internet.">
			<TextRow label="Public address" bind:value={station.public_host} placeholder="Such as radio.example.org" />
			<SettingsRow label="List it on sdr-list.xyz">
				{#snippet control()}<Switch bind:checked={station.sdr_list} disabled={!station.public_host.trim()} aria-label="List it on sdr-list.xyz" />{/snippet}
			</SettingsRow>
		</SettingsGroup>
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5" disabled={listenersBusy} onclick={saveListeners}>Save and finish</Button>
			<Button variant="secondary" size="lg" class="h-11 rounded-full px-5" onclick={nextStep}>Only at home for now</Button>
		</div>
	{:else}
		<p class="text-[15px] leading-relaxed text-muted-foreground">
			The receiver is set up. Everything chosen here can be changed later on the Station, Bands and Modules pages.
		</p>
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5" href={receiverAddress} target="_blank">Open the receiver</Button>
			<Button variant="secondary" size="lg" class="h-11 rounded-full px-5" onclick={finishSetup}>Go to the overview</Button>
		</div>
	{/if}
</div>
