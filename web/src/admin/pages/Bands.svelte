<script lang="ts">
	import ChevronLeft from '@lucide/svelte/icons/chevron-left';
	import ChevronRight from '@lucide/svelte/icons/chevron-right';
	import Plus from '@lucide/svelte/icons/plus';
	import RotateCw from '@lucide/svelte/icons/rotate-cw';
	import { setupStep } from '../lib/setup';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import Calibration from '../components/Calibration.svelte';
	import CarrierCalibration from '../components/CarrierCalibration.svelte';
	import ChoiceSheet from '../components/ChoiceSheet.svelte';
	import Columns from '../components/Columns.svelte';
	import AnimatedNumber from '../components/AnimatedNumber.svelte';
	import EditSheet from '../components/EditSheet.svelte';
	import EmptyState from '../components/EmptyState.svelte';
	import FigureRow from '../components/FigureRow.svelte';
	import HistoryArchive from '../components/HistoryArchive.svelte';
	import ListenerList from '../components/ListenerList.svelte';
	import ModuleInput from '../components/ModuleInput.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import RetryRing from '../components/RetryRing.svelte';
	import SaveBar from '../components/SaveBar.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import SpectrumShape from '../components/SpectrumShape.svelte';
	import TextRow from '../components/TextRow.svelte';
	import StatusBadge from '../components/StatusBadge.svelte';
	import Waterfall from '../components/Waterfall.svelte';
	import { Button } from '../components/ui/button/index';
	import { Confirm } from '../components/ui/confirm/index';
	import { Slider } from '../components/ui/slider/index';
	import { Switch } from '../components/ui/switch/index';
	import { bitrate, dbfs, megahertz, signed } from '../lib/format';
	import { bandCondition, bandUplink, conditionDetail, live } from '../lib/live.svelte';
	import { clockRange, complement, DAYLIGHT, describeHours, hoursNow, NIGHT, usesSun } from '../lib/hours';
	import { setSectionValue } from '../lib/ini-sections';
	import { href, navigate } from '../lib/router.svelte';

	let { id }: { id?: string } = $props();

	interface BandSettings {
		id: string;
		name: string;
		calibration: string;
		history: 'off' | 'private' | 'public';
		history_hours: number;
		history_bins: number;
		history_interval: number;
		history_bytes: number;
		noise_floor: number;
		noise_blanker: number;
		max_bandwidth: number;
		max_user_bitrate: number;
		default_audio_bitrate: number;
		iq_swap: boolean;
		dc_remove: boolean;
		iq_balance: boolean;
		hours: string;
		on_air: boolean;
		next_change: number;
		/** The bands taking turns with this one on its input. */
		partners: { id: string; name: string; hours: string }[];
		/** Whether the station has a grid square, so sunrise and sunset are its own. */
		located: boolean;
		fixed: {
			low: number;
			high: number;
			center: number;
			sample_rate: number;
			signal: string;
			source: string;
			running: boolean;
			listeners: number;
			ppm: number;
			frequency_offset: number;
		};
	}

	/** What each setting is called, for saying what is about to change. */
	const names: Partial<Record<keyof BandSettings, string>> = {
		name: 'name',
		calibration: 'calibration',
		history: 'history',
		history_hours: 'history length',
		noise_blanker: 'noise blanker',
		max_bandwidth: 'widest filter',
		max_user_bitrate: 'per-listener limit',
		default_audio_bitrate: 'audio quality',
		iq_swap: 'I/Q swap',
		dc_remove: 'centre spike removal',
		iq_balance: 'I/Q balance'
	};
	const editable = Object.keys(names) as (keyof BandSettings)[];

	let settings = $state<BandSettings[] | null>(null);
	let saved = $state<BandSettings[]>([]);
	let loadError = $state('');
	let busy = $state(false);
	let confirmRestart = $state(false);
	let measuringCarrier = $state(false);
	let sheet = $state<'quality' | 'ceiling' | 'filter' | 'blanker' | 'hours' | null>(null);
	let customOpen = $state(false);
	let customFrom = $state('06:00');
	let customUntil = $state('18:00');
	let sheetOpen = $state(false);
	let blanker = $state(0);

	onMount(() => {
		api
			.readBands()
			.then((result) => {
				settings = structuredClone(result.bands as BandSettings[]);
				saved = structuredClone(result.bands as BandSettings[]);
			})
			.catch((problem) => (loadError = (problem as Error).message));
	});

	const band = $derived(settings?.find((entry) => entry.id === id));
	const before = $derived(saved.find((entry) => entry.id === id));
	const current = $derived(live.state?.bands.find((entry) => entry.id === id));
	const changed = $derived(
		band && before ? editable.filter((key) => JSON.stringify(band[key]) !== JSON.stringify(before[key])) : []
	);

	function open(which: NonNullable<typeof sheet>) {
		sheet = which;
		if (which === 'blanker' && band) blanker = Math.round(band.noise_blanker * 100);
		sheetOpen = true;
	}

	/** Sensible steps, plus whatever the band is set to now if the file said something else. */
	function steps(values: [number, string, string?][], setting: number) {
		const options = values.map(([value, label, detail]) => ({ value, label, detail }));
		if (!values.some(([value]) => value === setting)) {
			options.push({ value: setting, label: `${setting >= 1000 ? Math.round(setting / 1000) : setting}`, detail: 'set in the configuration' });
		}
		return options.sort((a, b) => a.value - b.value);
	}

	async function apply() {
		if (!band) return;
		busy = true;
		const values = () => JSON.stringify(Object.fromEntries(editable.map((key) => [key, $state.snapshot(band[key])])));
		const sending = values();
		try {
			const result = await api.writeBand(band.id, JSON.parse(sending));
			saved = structuredClone(result.bands as BandSettings[]);
			// The receiver's version of what was sent, unless the operator kept editing meanwhile.
			if (values() === sending) settings = structuredClone(result.bands as BandSettings[]);
			toast.success(`${band.name} updated, for everyone listening`);
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			busy = false;
		}
	}

	function discard() {
		if (!settings || !before) return;
		// A snapshot, not structuredClone: `before` is reactive state, and a proxy cannot be cloned.
		const original = $state.snapshot(before);
		settings = settings.map((entry) => (entry.id === original.id ? original : entry));
	}

	/**
	 * Hours go into the configuration file, which stays the one place they are
	 * kept, and apply as it is saved. A band taking turns with one other on its
	 * input gets the other hours in the same save, so the two never overlap in
	 * between and the operator sets one thing, not two.
	 */
	async function saveHours(hours: string) {
		if (!band) return;
		const partner = band.partners.length === 1 ? band.partners[0] : null;
		const other = partner ? complement(hours) : null;
		const { text } = await api.readConfig();
		let next = setSectionValue(text, `band:${band.id}`, 'hours', hours);
		if (partner && other) next = setSectionValue(next, `band:${partner.id}`, 'hours', other);
		await api.writeConfig(next);
		// Only the schedule's fields are taken back: edits waiting in the save
		// bar, on this band or another, stay as the operator left them.
		const fresh = (await api.readBands()).bands as BandSettings[];
		const merge = (list: BandSettings[]) =>
			list.map((entry) => {
				const now = fresh.find((candidate) => candidate.id === entry.id);
				return now
					? { ...entry, hours: now.hours, on_air: now.on_air, next_change: now.next_change, partners: now.partners, located: now.located }
					: entry;
			});
		if (settings) settings = merge($state.snapshot(settings) as BandSettings[]);
		saved = merge($state.snapshot(saved) as BandSettings[]);
		await live.refresh();
		toast.success(
			partner && other
				? `${band.name}: ${describeHours(hours)}. ${partner.name}: ${describeHours(other)}.`
				: `${band.name}: ${describeHours(hours)}`
		);
	}

	function chooseHours(value: string) {
		if (value !== 'custom') {
			saveHours(value).catch((problem) => toast.error((problem as ApiError).message));
			return;
		}
		const range = band ? clockRange(band.hours) : null;
		customFrom = range?.from ?? '06:00';
		customUntil = range?.until ?? '18:00';
		customOpen = true;
	}

	const hoursChoice = (hours: string) => (['always', DAYLIGHT, NIGHT].includes(hours) ? hours : 'custom');

	// The setup flow's radio and band steps, which find the radio and suggest what fits it.
	function addBand() {
		setupStep('radio');
		navigate({ page: 'setup' });
	}

	async function restart() {
		if (!current) return;
		try {
			const result = await api.restartBand(current.id);
			if (result.ok) toast.success(`${current.name} is restarting`);
			else toast.message(result.note ?? 'A restart is already under way');
			await live.refresh();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	const sourceNames: Record<string, string> = {
		test: 'Synthetic signal',
		file: 'File or pipe',
		stdin: 'Standard input',
		pipe: 'Pipe',
		udp: 'UDP stream',
		module: 'Hardware module'
	};
	const kbit = (bits: number) => `${Math.round(bits / 1000)} kbit/s`;
	const khz = (hz: number) => `${(hz / 1000).toFixed(hz % 1000 ? 1 : 0)} kHz`;
	const blankerName = (value: number) => (value === 0 ? 'Off' : `${Math.round(value * 100)}%`);
</script>

{#if !id}
	<PageHeader title="Bands" description="Each band is one stretch of spectrum from one input.">
		{#snippet actions()}
			<Button size="lg" class="h-11 rounded-full px-4 md:h-9" onclick={addBand}><Plus /> Add a band</Button>
		{/snippet}
	</PageHeader>
	{#if loadError}
		<p class="text-destructive">{loadError}</p>
	{:else if !live.state}
		<div class="shimmer h-40 rounded-2xl bg-card"></div>
	{:else if live.state.bands.length === 0}
		<EmptyState title="No bands are configured.">Add a [band:...] section under Configuration.</EmptyState>
	{:else}
		<ul class="@container flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
			{#each live.state.bands as entry (entry.id)}
				{@const condition = bandCondition(entry)}
				<li>
					<a
						href={href({ page: 'bands', id: entry.id })}
						class="group grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-6 gap-y-3 px-4 py-3.5 transition-colors hover:bg-accent/50 active:bg-accent @2xl:grid-cols-[15rem_minmax(0,1fr)_auto] @2xl:py-4"
					>
						<span class="flex min-w-0 flex-col gap-0.5">
							<span class="truncate text-[15px] font-semibold">{entry.name}</span>
							<span class="truncate text-[13px] text-muted-foreground tabular">
								{megahertz(entry.low)} to {megahertz(entry.high)} MHz · {entry.listeners} listening
							</span>
						</span>
						<span class="col-span-2 row-start-2 flex min-h-11 min-w-0 items-center @2xl:col-span-1 @2xl:row-start-auto @2xl:min-h-12">
							{#if condition === 'receiving'}
								<SpectrumShape band={entry.id} class="h-11 w-full @2xl:h-12" />
							{:else}
								<span class="flex min-w-0 flex-col text-[13px]">
									<StatusBadge {condition} class="text-[13px]" />
									<span class="line-clamp-2 text-muted-foreground">{conditionDetail(entry.status || entry.error)}</span>
								</span>
							{/if}
						</span>
						<ChevronRight
							size={18}
							class="col-start-2 row-start-1 shrink-0 text-muted-foreground/70 transition-transform duration-300 ease-(--ease-spring) group-hover:translate-x-0.5 @2xl:col-start-3"
						/>
					</a>
				</li>
			{/each}
		</ul>
	{/if}
{:else}
	<div class="relative isolate">
		<a
			href={href({ page: 'bands' })}
			class="pressable -ml-2 inline-flex h-10 items-center gap-0.5 rounded-full pr-3 pl-1 text-[15px] font-medium text-muted-foreground hover:text-foreground md:mt-4"
		>
			<ChevronLeft size={22} /> Bands
		</a>

		{#if loadError}
			<p class="text-destructive">{loadError}</p>
		{:else if !settings || !live.state}
			<div class="mt-4 flex flex-col gap-4">
				<div class="shimmer h-12 w-2/3 max-w-sm rounded-2xl bg-card"></div>
				<div class="shimmer h-36 rounded-3xl bg-card md:h-56"></div>
			</div>
		{:else if !band || !current}
			<EmptyState title="There is no band called {id}.">It may have been renamed in the configuration.</EmptyState>
		{:else}
			{@const condition = bandCondition(current)}
			<header class="flex flex-wrap items-end justify-between gap-x-6 gap-y-3 pt-2 pb-6">
				<div class="flex min-w-0 flex-col gap-1">
					<h1 class="text-[1.875rem] leading-tight font-bold tracking-[-0.035em] md:text-[2.125rem]">{current.name}</h1>
					<p class="text-[15px] text-muted-foreground tabular">
						{megahertz(band.fixed.low)} to {megahertz(band.fixed.high)} MHz · {sourceNames[band.fixed.source] ?? band.fixed.source}
					</p>
				</div>
				<div class="flex items-center gap-3">
					<StatusBadge {condition} />
					<Button variant="secondary" size="lg" class="h-11 rounded-full px-4 md:h-9" disabled={current.restarting} onclick={() => (confirmRestart = true)}>
						<RotateCw /> Restart
					</Button>
				</div>
			</header>

			<!-- Keyed, so moving to another band starts its own history instead of drawing it under the last one's. -->
			{#key band.id}<Waterfall band={band.id} receiving={condition === 'receiving'} />{/key}

			<div class="mt-8">
				<Columns asideLabel="On air">
					{#snippet main()}
						{#if condition !== 'receiving' && condition !== 'off-air'}
							<SettingsGroup title="Why it is not receiving">
								<div class="flex flex-col gap-2 px-4 py-4">
									<p class="text-[15px] leading-relaxed">
										{conditionDetail(current.status || current.error) || 'The input stopped.'}
									</p>
									{#if current.retry_in !== undefined}<RetryRing seconds={current.retry_in} />{/if}
								</div>
								<SettingsRow label={condition === 'retrying' ? 'Try now' : 'Restart the band'} tone="font-medium" onclick={() => (confirmRestart = true)} />
							</SettingsGroup>
						{/if}

						{#if current.module}<ModuleInput band={current} />{/if}

						{@const partnerNames = band.partners.map((partner) => partner.name).join(', ')}
						<SettingsGroup
							title="On the air"
							footer={[
								band.partners.length
									? `Takes turns on its input with ${partnerNames}, which ${band.partners.length === 1 ? 'is' : 'are'} on the air at the other hours.`
									: 'Off the air, the band stops and frees its input; listeners on it move to the band that has the input then.',
								usesSun(band.hours) && !band.located
									? 'Without a grid square on the Station page, sunrise and sunset are taken as 06:00 and 18:00 UTC.'
									: ''
							]
								.filter(Boolean)
								.join(' ')}
						>
							<SettingsRow
								label="Hours"
								detail={hoursNow(current.on_air, current.next_change) ?? undefined}
								value={describeHours(band.hours)}
								onclick={() => open('hours')}
							/>
						</SettingsGroup>

						<SettingsGroup title="What listeners get" footer="Applies to everyone listening, without a restart.">
							<TextRow label="Name" bind:value={band.name} placeholder={band.id} />
							<SettingsRow label="Audio quality" detail="What a listener starts on" value={kbit(band.default_audio_bitrate)} onclick={() => open('quality')} />
							<SettingsRow label="Per listener" detail="Audio and waterfall together" value={kbit(band.max_user_bitrate)} onclick={() => open('ceiling')} />
							<SettingsRow label="Widest filter" value={khz(band.max_bandwidth)} onclick={() => open('filter')} />
							<SettingsRow label="Noise blanker" value={blankerName(band.noise_blanker)} onclick={() => open('blanker')} />
						</SettingsGroup>

						{@const iq = band.fixed.signal !== 'real'}
						<SettingsGroup
							title="Front end"
							footer="Corrections for the receiver's own flaws, made before the waterfall and every listener see the band."
						>
							{#if iq}
								<SettingsRow label="Swap I and Q" detail="For an input that delivers the spectrum mirrored">
									{#snippet control()}<Switch bind:checked={band.iq_swap} aria-label="Swap I and Q" />{/snippet}
								</SettingsRow>
							{/if}
							<SettingsRow
								label="Remove the centre spike"
								detail={current.input?.dc_offset_dbfs !== undefined
									? `Taking out an offset of ${dbfs(current.input.dc_offset_dbfs)}`
									: 'The DC offset a zero-IF receiver such as an RTL-SDR adds'}
							>
								{#snippet control()}<Switch bind:checked={band.dc_remove} aria-label="Remove the centre spike" />{/snippet}
							</SettingsRow>
							{#if iq}
								<SettingsRow
									label="Balance I and Q"
									detail={current.input?.image_rejection_db !== undefined
										? `Mirror images were ${Math.round(current.input.image_rejection_db)} dB down; now corrected`
										: 'Takes out the mirror images strong signals leave on the other side'}
								>
									{#snippet control()}<Switch bind:checked={band.iq_balance} aria-label="Balance I and Q" />{/snippet}
								</SettingsRow>
							{/if}
						</SettingsGroup>

						<SettingsGroup
							title="Frequency accuracy"
							footer="A crystal correction stretches the whole frequency axis, as a crystal error does; an offset shifts it. Both are set in the configuration and apply from the receiver's next start."
						>
							<SettingsRow label="Crystal correction" value="{signed(band.fixed.ppm, 2, true)} ppm" />
							<SettingsRow label="Frequency offset" value="{signed(band.fixed.frequency_offset, 1, true)} Hz" />
							<SettingsRow label="Measure against a known carrier" tone="font-medium" onclick={() => (measuringCarrier = true)} />
						</SettingsGroup>

						<Calibration
							bandId={band.id}
							value={band.calibration}
							lowHz={band.fixed.low}
							highHz={band.fixed.high}
							noiseFloorDbfs={band.noise_floor}
							onChange={(value) => (band.calibration = value)}
						/>

						<HistoryArchive
							access={band.history}
							hours={band.history_hours}
							bins={band.history_bins}
							interval={band.history_interval}
							onChange={(next) => {
								if (next.history) band.history = next.history;
								if (next.history_hours) band.history_hours = next.history_hours;
							}}
						/>
					{/snippet}

					{#snippet aside()}
						{@const series = live.bands[band.id]}
						<section data-settings-group class="flex flex-col gap-2">
							<h2 class="px-4 text-sm font-medium text-muted-foreground">Right now</h2>
							<div class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
								<FigureRow label="Listening" series={series?.listeners}>
									<AnimatedNumber value={current.listeners} />
								</FigureRow>
								<FigureRow label="Going out" series={series?.uplink} seriesLabel="What this band sends, over the last few minutes">
									<AnimatedNumber value={live.state ? bandUplink(live.state, band.id) : 0} format={(value) => bitrate(value)} />
								</FigureRow>
								<FigureRow
									label="Noise floor"
									series={(series?.floor ?? []).filter((value) => value > -159)}
									tone="text-signal"
								>
									{current.noise_floor > -159 ? dbfs(current.noise_floor) : 'Not measured yet'}
								</FigureRow>
							</div>
						</section>
						<ListenerList band={band.id} />
						<SettingsGroup title="Wiring" footer="Fixed when the band starts. Change these under Configuration, then restart the receiver.">
							<SettingsRow label="Source" value={sourceNames[band.fixed.source] ?? band.fixed.source} />
							<SettingsRow label="Sample rate" value="{(band.fixed.sample_rate / 1e6).toFixed(3)} Msps" />
							<SettingsRow label="Centre" value="{megahertz(band.fixed.center, 4)} MHz" />
							<SettingsRow label="Signal" value={band.fixed.signal === 'iq' ? 'IQ' : 'Real'} />
						</SettingsGroup>
					{/snippet}
				</Columns>
			</div>

			{#if sheet === 'hours'}
				<ChoiceSheet
					bind:open={sheetOpen}
					title="Hours on the air"
					description={band.partners.length === 1
						? `${band.partners[0].name} shares this input and gets the other hours.`
						: 'Sunrise and sunset are those at the station, and follow the seasons.'}
					options={[
						...(band.partners.length ? [] : [{ value: 'always', label: 'Always', detail: 'the default' }]),
						{ value: DAYLIGHT, label: 'In daylight', detail: 'sunrise to sunset, for 20 m and up' },
						{ value: NIGHT, label: 'At night', detail: 'sunset to sunrise, for 40 m and down' },
						{ value: 'custom', label: 'Set times', detail: 'from and until, in UTC' }
					]}
					value={hoursChoice(band.hours)}
					onChange={chooseHours}
				/>
			{:else if sheet === 'quality'}
				<ChoiceSheet
					bind:open={sheetOpen}
					title="Audio quality"
					description="What a listener starts on. Each can change their own."
					options={steps([[24000, '24 kbit/s', 'voice over a slow link'], [32000, '32 kbit/s', 'voice'], [48000, '48 kbit/s', 'the default, good for digital modes'], [64000, '64 kbit/s', 'crowded digital segments'], [96000, '96 kbit/s', 'broadcast music']], band.default_audio_bitrate)}
					value={band.default_audio_bitrate}
					onChange={(value) => (band.default_audio_bitrate = value)}
				/>
			{:else if sheet === 'ceiling'}
				<ChoiceSheet
					bind:open={sheetOpen}
					title="Per listener"
					description="The most one listener's audio and waterfall may take together."
					options={steps([[64000, '64 kbit/s'], [100000, '100 kbit/s', 'the default'], [150000, '150 kbit/s'], [200000, '200 kbit/s'], [300000, '300 kbit/s']], band.max_user_bitrate)}
					value={band.max_user_bitrate}
					onChange={(value) => (band.max_user_bitrate = value)}
				/>
			{:else if sheet === 'filter'}
				<ChoiceSheet
					bind:open={sheetOpen}
					title="Widest filter"
					description="The widest passband one listener may ask for."
					options={steps([[3000, '3 kHz', 'SSB and CW only'], [6000, '6 kHz'], [10000, '10 kHz', 'AM broadcast'], [12000, '12 kHz'], [20000, '20 kHz', 'the default, room for FM']], band.max_bandwidth)}
					value={band.max_bandwidth}
					onChange={(value) => (band.max_bandwidth = value)}
				/>
			{:else if sheet === 'blanker'}
				<ChoiceSheet
					bind:open={sheetOpen}
					title="Noise blanker"
					description="Takes out ignition sparks, mains buzz and switch clicks. It cannot touch continuous noise, and more than a percent or two of blanking takes signal with it."
					options={[
						{ value: 0, label: 'Off', detail: 'the default' },
						{ value: 25, label: 'Light' },
						{ value: 50, label: 'Medium' },
						{ value: 75, label: 'Strong' }
					]}
					value={Math.round(band.noise_blanker * 100)}
					onChange={(value) => (band.noise_blanker = value / 100)}
				>
					{#snippet extra()}
						<div class="flex flex-col gap-2 border-t border-border pt-4">
							<div class="flex justify-between text-sm"><span class="font-medium">Exactly</span><span class="text-muted-foreground tabular">{blanker}%</span></div>
							<Slider label="Noise blanker strength" min={0} max={100} step={1} bind:value={blanker} onValueCommit={(value) => (band.noise_blanker = value / 100)} />
						</div>
					{/snippet}
				</ChoiceSheet>
			{/if}

			<SaveBar
				visible={changed.length > 0}
				message="Changed: {changed.map((key) => names[key]).join(', ')}"
				save="Apply"
				{busy}
				onSave={apply}
				onDiscard={discard}
			/>

			<EditSheet
				bind:open={customOpen}
				title="Hours on the air"
				description="In UTC. Until a time earlier than the start runs past midnight."
				disabled={!customFrom || !customUntil || customFrom === customUntil}
				onSave={() => saveHours(`${customFrom}-${customUntil}`)}
			>
				<div class="grid grid-cols-2 gap-3">
					<label class="flex flex-col gap-1.5 text-sm font-medium">
						From
						<input type="time" bind:value={customFrom} required class="h-11 rounded-xl border border-input bg-transparent px-3 text-base tabular" />
					</label>
					<label class="flex flex-col gap-1.5 text-sm font-medium">
						Until
						<input type="time" bind:value={customUntil} required class="h-11 rounded-xl border border-input bg-transparent px-3 text-base tabular" />
					</label>
				</div>
			</EditSheet>

			<CarrierCalibration bind:open={measuringCarrier} bandId={band.id} lowHz={band.fixed.low} highHz={band.fixed.high} />

			<Confirm
				bind:open={confirmRestart}
				title="Restart {current.name}?"
				description="The band stops and starts again. Its listeners stay connected and hear a short gap. A module band also takes the module settings saved in the configuration since it started."
				action="Restart"
				onConfirm={restart}
			/>
		{/if}
	</div>
{/if}
