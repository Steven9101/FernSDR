<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import ChoiceSheet from '../components/ChoiceSheet.svelte';
	import EditSheet from '../components/EditSheet.svelte';
	import Columns from '../components/Columns.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SaveBar from '../components/SaveBar.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import SpectrumShape from '../components/SpectrumShape.svelte';
	import TextRow from '../components/TextRow.svelte';
	import LocatorPicker from '../components/LocatorPicker.svelte';
	import { locatorCentre } from '../../util/locator';
	import { Switch } from '../components/ui/switch/index';
	import { ago } from '../lib/format';
	import { live } from '../lib/live.svelte';
	import { planFor } from '../../state/bandplan';

	/*
	Who runs this receiver and where, as listeners see it. Everything here applies the moment it is
	saved, on every page already open, and lives in a file beside the configuration rather than in
	it: the configuration is the receiver's wiring, hand-edited and full of comments, and rewriting
	it to change a callsign would lose them.
	*/
	interface Station {
		name: string;
		operator: string;
		location: string;
		grid: string;
		antenna: string;
		contact: string;
		website: string;
		notice: string;
		max_users: number;
		listener_timeout: number;
		sdr_list: boolean;
		public_host: string;
		public_port: number;
		band_plan: string;
	}

	interface Listing {
		enabled: boolean;
		state: 'off' | 'waiting' | 'listed' | 'failing';
		detail: string;
		listed_ms: number;
	}

	const names: Record<keyof Station, string> = {
		name: 'name',
		operator: 'operator',
		location: 'location',
		grid: 'grid square',
		antenna: 'antenna',
		contact: 'contact',
		website: 'website',
		notice: 'notice',
		max_users: 'listener limit',
		listener_timeout: 'listener timeout',
		sdr_list: 'sdr-list.xyz listing',
		public_host: 'public address',
		public_port: 'public port',
		band_plan: 'band plan'
	};


	// Changing the password: the sheet's fields, and what it is doing while
	// the slow key derivation runs.
	let passwordOpen = $state(false);
	let passwordCurrent = $state('');
	let passwordNext = $state('');
	let passwordRepeat = $state('');
	let passwordStep = $state('');
	const passwordHint = $derived(
		passwordNext && passwordNext.length < 12
			? `${passwordNext.length} characters; at least 12.`
			: passwordRepeat && passwordRepeat !== passwordNext
				? 'The two new passwords differ.'
				: passwordNext && passwordNext === passwordCurrent
					? 'That is the password it has now.'
					: ''
	);
	const passwordReady = $derived(
		passwordCurrent.length > 0 && passwordNext.length >= 12 && passwordNext === passwordRepeat && passwordNext !== passwordCurrent
	);

	async function changePassword() {
		// The receiver ends every session on the change and the page signs in
		// again at once, seconds later over plain HTTP where the browser
		// derives the key without Web Crypto. The panel's own polling in
		// between would see the gap and sign it out, so it waits.
		live.stop();
		try {
			await api.changePassword(passwordCurrent, passwordNext, (step) => (passwordStep = step));
			toast.success('Password changed. You are signed in with the new one.');
			passwordCurrent = passwordNext = passwordRepeat = '';
		} catch (problem) {
			const error = problem as ApiError;
			throw new Error(error.status === 403 ? 'The current password is not right.' : error.message);
		} finally {
			passwordStep = '';
			live.start();
		}
	}
	let station = $state<Station | null>(null);
	let saved = $state<Station | null>(null);
	let server = $state<{ server: Record<string, unknown>; config_path: string } | null>(null);
	let loadError = $state('');
	let busy = $state(false);
	let limitOpen = $state(false);
	let timeoutOpen = $state(false);
	let exactLimit = $state('');
	let listing = $state<Listing | null>(null);
	let planOpen = $state(false);

	const REGIONS: Record<number, string> = {
		1: 'Region 1: Europe, Africa, the Middle East, Russia',
		2: 'Region 2: the Americas',
		3: 'Region 3: Asia and Oceania'
	};
	const PLANS: { value: string; label: string; detail?: string }[] = [
		{ value: 'auto', label: 'Automatic' },
		{ value: 'r1', label: 'IARU Region 1', detail: 'Europe, Africa, the Middle East, Russia' },
		{ value: 'r2', label: 'IARU Region 2', detail: 'the Americas' },
		{ value: 'r3', label: 'IARU Region 3', detail: 'Asia and Oceania' },
		{ value: 'us', label: 'United States', detail: 'FCC Part 97' },
		{ value: 'ca', label: 'Canada' },
		{ value: 'gb', label: 'United Kingdom' },
		{ value: 'de', label: 'Germany' },
		{ value: 'au', label: 'Australia' },
		{ value: 'jp', label: 'Japan' },
		{ value: 'none', label: 'None', detail: 'no plan over the waterfall' }
	];

	let pickerOpen = $state(false);

	/** Where the grid square is, so a typo that lands in the sea shows before it is saved. */
	function gridDetail(grid: string): string {
		if (!grid.trim()) return 'Or from where this browser is';
		const centre = locatorCentre(grid);
		if (!centre) return 'Not a locator: two letters, two digits, and two more letters for the small square';
		const lat = `${Math.abs(centre.lat).toFixed(2)}° ${centre.lat >= 0 ? 'N' : 'S'}`;
		const lon = `${Math.abs(centre.lon).toFixed(2)}° ${centre.lon >= 0 ? 'E' : 'W'}`;
		return `Around ${lat}, ${lon}`;
	}

	/** What "Automatic" works out from the grid square, in words. */
	function automatic(grid: string): string {
		const plan = planFor('auto', grid);
		return /^[A-R]{2}\d{2}/i.test(grid.trim()) && plan
			? `${REGIONS[plan.region]}, from ${grid.trim().toUpperCase()}`
			: 'Region 1 until the grid square is filled in';
	}

	// The report goes out once a minute; looking every ten seconds shows its
	// outcome soon after it happens without keeping the receiver busy.
	function readListing() {
		api
			.readDirectory()
			.then((result) => (listing = result as Listing))
			.catch(() => undefined);
	}
	readListing();
	const listingTimer = setInterval(readListing, 10_000);
	onDestroy(() => clearInterval(listingTimer));

	// The server's reasons end with their own question mark at times.
	const sentence = (text: string) => (/[.?!]$/.test(text) ? text : `${text}.`);
	const listingText = $derived.by(() => {
		if (!listing) return '';
		if (listing.state === 'listed') return `Listed. Last report ${ago(listing.listed_ms)}.`;
		if (listing.state === 'waiting') return 'The first report goes out within a minute.';
		if (listing.state === 'failing') return `Not reaching sdr-list.xyz: ${sentence(listing.detail)}`;
		return listing.enabled && listing.detail ? `Not listed: ${sentence(listing.detail)}` : 'Not listed.';
	});
	onMount(() => {
		api
			.readStation()
			.then((result) => {
				station = { ...(result.station as Station) };
				saved = { ...(result.station as Station) };
			})
			.catch((problem) => (loadError = (problem as Error).message));
		api
			.readServer()
			.then((result) => (server = result))
			.catch(() => undefined);
	});

	const changed = $derived(
		station && saved ? (Object.keys(names) as (keyof Station)[]).filter((key) => station![key] !== saved![key]) : []
	);

	async function save() {
		if (!station) return;
		busy = true;
		const sending = $state.snapshot(station);
		// Only what was changed here: a field this page loaded long ago may
		// have been changed since in the configuration editor, and sending it
		// back unchanged would put the old value over the new one.
		const edits = Object.fromEntries(changed.map((key) => [key, sending[key]]));
		try {
			const result = await api.writeStation(edits);
			saved = { ...(result.station as Station) };
			// The receiver's version of what was sent, unless the operator kept typing meanwhile.
			if (JSON.stringify($state.snapshot(station)) === JSON.stringify(sending)) station = { ...(result.station as Station) };
			toast.success('Saved. Every open page shows it now.');
			void live.refresh();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			busy = false;
		}
	}

	function openLimit() {
		if (!station) return;
		exactLimit = String(station.max_users);
		limitOpen = true;
	}

	function serverValue(value: unknown): string {
		if (Array.isArray(value)) return value.length ? value.join(', ') : 'None';
		return String(value);
	}

	const serverRows: [string, string][] = [
		['bind', 'Listens on'],
		['port', 'Port'],
		['websocket_path', 'WebSocket path'],
		['max_connections', 'Connections at most'],
		['idle_timeout', 'Idle connections close after'],
		['trusted_proxies', 'Trusted proxies']
	];
</script>

<PageHeader title="Station" description="Who runs this receiver and where. Saved changes appear on every open page at once." />

{#if loadError}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !station}
	<div class="flex max-w-2xl flex-col gap-3" aria-busy="true">
		<div class="shimmer h-56 rounded-2xl bg-card"></div>
		<div class="shimmer h-32 rounded-2xl bg-card"></div>
	</div>
{:else}
	{@const current = station}
	<Columns asideLabel="How it looks, and the server">
		{#snippet main()}
			<SettingsGroup
				title="On the receiver"
				footer="The name and the operator, usually a callsign, head every page; location and antenna run along its bottom edge."
			>
				<TextRow label="Name" bind:value={current.name} />
				<TextRow label="Operator" bind:value={current.operator} autocapitalize="characters" />
				<TextRow label="Location" bind:value={current.location} />
				<TextRow label="Antenna" bind:value={current.antenna} />
			</SettingsGroup>

			<SettingsGroup
				title="Also published"
				footer="The grid square, a Maidenhead locator such as JO62qm, goes into the public status that lists of receivers read, and picks the band plan when that is automatic. The band plan is drawn over the waterfall and names each frequency; national plans follow the country's own rules. Contact, an email address or a page, and the website travel with the station details to every open page."
			>
				<TextRow label="Grid square" bind:value={current.grid} autocapitalize="characters" />
				<SettingsRow
					label="Find it on a map"
					detail={gridDetail(current.grid)}
					onclick={() => (pickerOpen = true)}
				/>
				<SettingsRow
					label="Band plan"
					value={PLANS.find((plan) => plan.value === current.band_plan)?.label ?? current.band_plan}
					detail={current.band_plan === 'auto' ? automatic(current.grid) : undefined}
					onclick={() => (planOpen = true)}
				/>
				<TextRow label="Contact" bind:value={current.contact} inputmode="email" />
				<TextRow label="Website" bind:value={current.website} inputmode="url" />
			</SettingsGroup>

			<SettingsGroup
				title="Directory"
				footer="sdr-list.xyz shows receivers on a map and in a list. Listed, this receiver tells it the name, grid square, antenna, bands and number of listeners once a minute, and the map pin is moved a little so the exact place stays private. It drops out five minutes after the last report, so switching this off is enough to leave."
			>
				<SettingsRow label="List on sdr-list.xyz" detail="Needs the grid square above and the address listeners use">
					{#snippet control()}<Switch bind:checked={current.sdr_list} aria-label="List on sdr-list.xyz" />{/snippet}
				</SettingsRow>
				{#if current.sdr_list || current.public_host}
					<TextRow label="Public address" bind:value={current.public_host} placeholder="sdr.example.org" inputmode="url" autocapitalize="none" />
					<TextRow
						label="Public port"
						bind:value={
							() => (current.public_port ? String(current.public_port) : ''),
							(text) => (current.public_port = Math.min(65535, Number(text.replace(/\D/g, '')) || 0))
						}
						placeholder={server ? `${serverValue(server.server.port)}, the receiver's own` : "The receiver's own"}
						inputmode="numeric"
					/>
				{/if}
				{#if listing && (listing.enabled || listing.state !== 'off')}
					<p
						role="status"
						class="px-4 py-3 text-[13px] {listing.state === 'failing' || (listing.enabled && listing.state === 'off')
							? 'text-destructive'
							: 'text-muted-foreground'}"
					>
						{listingText}
					</p>
				{/if}
			</SettingsGroup>

			<SettingsGroup title="Notice" footer="A banner everyone sees above the receiver, such as when it will be down for work. Leave it empty for none.">
				<label class="block px-4 py-3 transition-colors focus-within:bg-accent/40">
					<span class="sr-only">Notice</span>
					<textarea
						bind:value={current.notice}
						rows="3"
						maxlength="2000"
						placeholder="No notice"
						class="field-sizing-content block max-h-60 min-h-[4.5rem] w-full resize-none bg-transparent text-[15px] leading-relaxed outline-none placeholder:text-muted-foreground"
					></textarea>
				</label>
			</SettingsGroup>

			<SettingsGroup
				title="Capacity"
				footer="Beyond the limit, a new listener is told the receiver is full instead of joining a stream that cannot keep up. Each listener takes about half a percent of a CPU core and 80 kbit/s."
			>
				<SettingsRow label="Listener limit" value={String(current.max_users)} onclick={openLimit} />
				<SettingsRow
					label="Let idle listeners go"
					value={current.listener_timeout ? `after ${current.listener_timeout} min` : 'Never'}
					onclick={() => (timeoutOpen = true)}
				/>
			</SettingsGroup>
		{/snippet}

		{#snippet aside()}
			<section class="flex flex-col gap-2" aria-label="How listeners see it">
				<h2 class="px-4 text-sm font-medium text-muted-foreground">How listeners see it</h2>
				<div class="overflow-hidden rounded-2xl bg-card ring-1 ring-border" aria-hidden="true">
					<div class="flex items-baseline gap-2 border-b border-border px-4 py-3">
						<span class="truncate text-[15px] font-semibold">{current.name || 'Unnamed receiver'}</span>
						{#if current.operator}<span class="truncate text-[13px] text-muted-foreground">{current.operator}</span>{/if}
					</div>
					{#if current.notice.trim()}
						<p class="border-b border-border bg-muted/60 px-4 py-2 text-[13px] leading-relaxed break-words whitespace-pre-line">
							{current.notice}
						</p>
					{/if}
					<div class="h-20 bg-muted/40 px-2 pt-3">
						{#if live.state?.bands[0]}<SpectrumShape band={live.state.bands[0].id} bins={128} class="h-full w-full" />{/if}
					</div>
					<div class="flex gap-3 border-t border-border px-4 py-2 text-[12px] text-muted-foreground">
						<span class="truncate">{current.location || 'No location'}</span>
						{#if current.antenna}<span class="truncate">{current.antenna}</span>{/if}
					</div>
				</div>
			</section>

			<SettingsGroup title="Sign-in" footer="The password for this panel. At least 12 characters; neither the old nor the new one is sent over the network.">
				<SettingsRow label="Change password" tone="font-medium" onclick={() => (passwordOpen = true)} />
			</SettingsGroup>

			{#if server}
				<SettingsGroup title="Server" footer="Read once when the receiver starts. Change these in the configuration file, then restart the receiver.">
					{#each serverRows as [key, label] (key)}
						{#if server.server[key] !== undefined}
							<SettingsRow {label} value="{serverValue(server.server[key])}{key === 'idle_timeout' ? ' s' : ''}" />
						{/if}
					{/each}
					<div class="flex flex-col gap-0.5 px-4 py-3">
						<span class="text-[15px]">Configuration file</span>
						<span class="text-[13px] break-all text-muted-foreground">{server.config_path}</span>
					</div>
				</SettingsGroup>
			{/if}
		{/snippet}
	</Columns>

	<ChoiceSheet
		bind:open={planOpen}
		title="Band plan"
		description="Which plan listeners see over the waterfall. Automatic follows the grid square's ITU region; near a region's border or for a national plan, choose it here."
		options={PLANS.map((plan) => (plan.value === 'auto' ? { ...plan, detail: automatic(current.grid) } : plan))}
		value={current.band_plan}
		onChange={(value) => (current.band_plan = value)}
	/>

	<LocatorPicker bind:open={pickerOpen} grid={current.grid} onPick={(grid) => (current.grid = grid)} />

	<ChoiceSheet
		bind:open={timeoutOpen}
		title="Let idle listeners go"
		description="A listener who has not tuned or touched anything for this long is asked whether they are still listening, and their place is freed a minute later if nobody answers. For a busy receiver whose places fill with forgotten tabs."
		options={[0, 15, 30, 60, 120, 240, current.listener_timeout]
			.filter((value, index, all) => all.indexOf(value) === index)
			.sort((a, b) => a - b)
			.map((value) => ({ value, label: value ? `After ${value} minutes` : 'Never', detail: value === 0 ? 'the default' : undefined }))}
		value={current.listener_timeout}
		onChange={(value) => (current.listener_timeout = value)}
	/>

	<ChoiceSheet
		bind:open={limitOpen}
		title="Listener limit"
		description="How many people may listen at once."
		options={[25, 50, 100, 200, 300, 500, current.max_users]
			.filter((value, index, all) => all.indexOf(value) === index)
			.sort((a, b) => a - b)
			.map((value) => ({ value, label: String(value), detail: value === 200 ? 'the default' : undefined }))}
		value={current.max_users}
		onChange={(value) => (current.max_users = value)}
	>
		{#snippet extra()}
			<form
				class="flex items-center gap-3 border-t border-border pt-4"
				onsubmit={(event) => {
					event.preventDefault();
					const value = Math.round(Number(exactLimit));
					if (!Number.isFinite(value) || value < 1 || value > 100000) {
						toast.error('The limit must be between 1 and 100000');
						return;
					}
					current.max_users = value;
					limitOpen = false;
				}}
			>
				<label for="exact-limit" class="text-sm font-medium">Exactly</label>
				<input
					id="exact-limit"
					bind:value={exactLimit}
					inputmode="numeric"
					class="h-10 min-w-0 flex-1 rounded-xl bg-muted px-3 text-right text-[15px] tabular outline-none focus-visible:ring-3 focus-visible:ring-ring/50"
				/>
				<button type="submit" class="pressable h-10 rounded-full bg-primary px-4 text-sm font-medium text-primary-foreground">Set</button>
			</form>
		{/snippet}
	</ChoiceSheet>

	<EditSheet
		bind:open={passwordOpen}
		title="Change password"
		description="Signs everyone out of the panel, then signs you in again with the new password."
		action={passwordStep || 'Change'}
		disabled={!passwordReady || passwordStep !== ''}
		onSave={changePassword}
	>
		<div class="flex flex-col gap-3">
			<label class="flex flex-col gap-1.5 text-sm font-medium">
				Current password
				<input type="password" autocomplete="current-password" bind:value={passwordCurrent}
					class="h-11 rounded-xl border border-input bg-transparent px-3 text-base" />
			</label>
			<label class="flex flex-col gap-1.5 text-sm font-medium">
				New password
				<input type="password" autocomplete="new-password" bind:value={passwordNext}
					class="h-11 rounded-xl border border-input bg-transparent px-3 text-base" />
			</label>
			<label class="flex flex-col gap-1.5 text-sm font-medium">
				New password again
				<input type="password" autocomplete="new-password" bind:value={passwordRepeat}
					class="h-11 rounded-xl border border-input bg-transparent px-3 text-base" />
			</label>
			{#if passwordHint}<p class="text-[13px] text-muted-foreground">{passwordHint}</p>{/if}
		</div>
	</EditSheet>

	<SaveBar
		visible={changed.length > 0}
		message="Changed: {changed.map((key) => names[key]).join(', ')}"
		{busy}
		onSave={save}
		onDiscard={() => saved && (station = { ...saved })}
	/>
{/if}
