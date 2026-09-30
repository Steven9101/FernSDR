<script lang="ts">
	import ArrowRight from '@lucide/svelte/icons/arrow-right';
	import CircleAlert from '@lucide/svelte/icons/circle-alert';
	import RefreshCw from '@lucide/svelte/icons/refresh-cw';
	import RotateCw from '@lucide/svelte/icons/rotate-cw';
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type BandState } from '../api';
	import AnimatedNumber from '../components/AnimatedNumber.svelte';
	import Columns from '../components/Columns.svelte';
	import FigureRow from '../components/FigureRow.svelte';
	import ListenerList from '../components/ListenerList.svelte';
	import MutedList from '../components/MutedList.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import RetryRing from '../components/RetryRing.svelte';
	import Sparkline from '../components/Sparkline.svelte';
	import StatusBadge from '../components/StatusBadge.svelte';
	import { Button } from '../components/ui/button/index';
	import { Confirm } from '../components/ui/confirm/index';
	import { ago, bitrate, dbfs, megahertz, share } from '../lib/format';
	import { bandCondition, conditionDetail, isTrouble, live } from '../lib/live.svelte';
	import { parseLogLine, recentProblems, type LogEntry } from '../lib/log';
	import { dismissSetup, setupDone } from '../lib/setup';
	import { href } from '../lib/router.svelte';
	import Check from '@lucide/svelte/icons/check';

	let now = $state(Date.now());
	$effect(() => {
		const timer = window.setInterval(() => (now = Date.now()), 1000);
		return () => window.clearInterval(timer);
	});

	const bands = $derived(live.state?.bands ?? []);
	const troubled = $derived(bands.filter((band) => isTrouble(bandCondition(band))));
	const receiving = $derived(bands.filter((band) => bandCondition(band) === 'receiving').length);
	const uplink = $derived(live.uplink.at(-1) ?? 0);

	// Problems the log already knows about, so the reason for a quiet band is one glance away.
	// The same failure every thirty seconds is one problem, not five.
	let problems = $state<LogEntry[]>([]);
	$effect(() => {
		let active = true;
		const load = () =>
			api
				.log()
				.then((result) => {
					if (active) problems = recentProblems(result.lines.map(parseLogLine), 5);
				})
				.catch(() => undefined);
		load();
		const timer = window.setInterval(load, 10000);
		return () => {
			active = false;
			window.clearInterval(timer);
		};
	});

	let restarting = $state<BandState | null>(null);
	let confirmOpen = $state(false);

	async function restart(band: BandState) {
		try {
			const result = await api.restartBand(band.id);
			if (result.ok) toast.success(`${band.name} is restarting`);
			else toast.message(result.note ?? 'A restart is already under way');
			await live.refresh();
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}


	// What is left of setting the receiver up, until it is done or the
	// operator hides it: a station with a name and a place, and a radio.
	let setup = $state<{ station: boolean; radio: boolean } | null>(null);
	if (!setupDone()) {
		Promise.all([api.readStation(), api.readBands()])
			.then(([station, bands]) => {
				const values = station.station as { name?: string; grid?: string };
				const sources = (bands.bands as { fixed: { source: string } }[]).map((band) => band.fixed.source);
				const next = { station: !!values.grid && !!values.name && values.name !== 'FernSDR', radio: sources.some((source) => source !== 'test') };
				setup = next.station && next.radio ? null : next;
			})
			.catch(() => {});
	}
</script>

<PageHeader
	title="Overview"
	description={live.updated ? `Updated ${ago(live.updated, now)}` : 'Waiting for the receiver'}
>
	{#snippet actions()}
		<Button variant="secondary" size="lg" class="h-11 rounded-full px-4 md:h-9" onclick={() => live.refresh()}>
			<RefreshCw /> Refresh
		</Button>
	{/snippet}
</PageHeader>

{#if setup}
	<article class="mb-8 flex flex-col gap-4 rounded-3xl bg-card p-5 md:p-6">
		<div class="flex flex-col gap-1">
			<h2 class="text-lg font-semibold">Finish setting up</h2>
			<p class="text-[15px] text-muted-foreground">A few questions, one at a time, and the receiver listens with your radio.</p>
		</div>
		<ul class="flex flex-col gap-2 text-[15px]">
			{#each [['Its name and its place on the map', setup.station], ['A radio instead of the test signal', setup.radio]] as [label, done] (label)}
				<li class="flex items-center gap-2.5">
					<span class="grid size-5 place-items-center rounded-full {done ? 'bg-primary text-primary-foreground' : 'border border-border'}">
						{#if done}<Check size={12} />{/if}
					</span>
					<span class={done ? 'text-muted-foreground line-through' : ''}>{label}</span>
				</li>
			{/each}
		</ul>
		<div class="flex flex-wrap gap-3">
			<Button size="lg" class="h-11 rounded-full px-5 md:h-9" href={href({ page: 'setup' })}>Continue <ArrowRight /></Button>
			<Button variant="ghost" size="lg" class="h-11 rounded-full px-4 md:h-9" onclick={() => { dismissSetup(); setup = null; }}>Hide</Button>
		</div>
	</article>
{/if}

{#if live.error && live.state}
	<p class="mb-6 flex items-center gap-2 rounded-2xl bg-destructive/10 px-4 py-3 text-sm text-destructive" role="alert">
		<CircleAlert size={16} /> The receiver did not answer: {live.error}. Showing what it said last.
	</p>
{/if}

{#if troubled.length > 0}
	<div class="mb-8 flex flex-col gap-3">
		{#each troubled as band (band.id)}
			{@const condition = bandCondition(band)}
			<article class="flex flex-col gap-4 rounded-3xl bg-card p-5 md:flex-row md:items-center md:p-6">
				<div class="flex min-w-0 flex-1 flex-col gap-1.5">
					<div class="flex flex-wrap items-center gap-x-3 gap-y-1">
						<StatusBadge {condition} />
						<span class="font-semibold">{band.name}</span>
					</div>
					{#if conditionDetail(band.status || band.error)}
						<p class="text-[15px] leading-relaxed text-muted-foreground">
							{conditionDetail(band.status || band.error)}
						</p>
					{/if}
					{#if band.retry_in !== undefined}
						<RetryRing seconds={band.retry_in} />
					{/if}
				</div>
				<div class="flex shrink-0 gap-2">
					<Button
						variant="secondary"
						size="lg"
						class="rounded-full px-4"
						disabled={band.restarting}
						onclick={() => {
							restarting = band;
							confirmOpen = true;
						}}
					>
						<RotateCw /> {condition === 'retrying' ? 'Try now' : 'Restart'}
					</Button>
					<Button href={href({ page: 'bands', id: band.id })} size="lg" class="h-11 rounded-full px-4 md:h-9">
						Open <ArrowRight />
					</Button>
				</div>
			</article>
		{/each}
	</div>
{/if}

<Columns asideFirst asideLabel="Right now">
	{#snippet aside()}
		<section data-settings-group class="flex flex-col gap-2">
			<h2 class="px-4 text-sm font-medium text-muted-foreground">Right now</h2>
			<div class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
				<FigureRow label="Listening" series={live.listeners}>
					<AnimatedNumber value={live.state?.users ?? 0} />
					<span class="text-sm font-normal text-muted-foreground">of {live.state?.max_users ?? 0}</span>
				</FigureRow>
				<FigureRow label="Going out" series={live.uplink} seriesLabel="What the receiver sends, over the last few minutes">
					<AnimatedNumber value={uplink} format={(value) => bitrate(value)} />
				</FigureRow>
				<FigureRow label="Bands receiving">
					<AnimatedNumber value={receiving} />
					<span class="text-sm font-normal text-muted-foreground">of {bands.length}</span>
				</FigureRow>
			</div>
		</section>
		<MutedList />
	{/snippet}

	{#snippet main()}
		<section class="flex flex-col gap-2">
			<h2 class="px-4 text-sm font-medium text-muted-foreground">Bands</h2>
			{#if !live.state}
				<div class="flex flex-col gap-2">
					{#each [0, 1] as placeholder (placeholder)}
						<div class="shimmer h-20 rounded-2xl bg-card"></div>
					{/each}
				</div>
			{:else}
				<ul class="@container flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
					{#each bands as band (band.id)}
						{@const condition = bandCondition(band)}
						{@const series = live.bands[band.id]}
						<li>
							<a
								href={href({ page: 'bands', id: band.id })}
								class="group grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-4 gap-y-3 px-4 py-3.5 transition-colors hover:bg-accent/50 active:bg-accent @md:grid-cols-[minmax(0,1fr)_8.5rem_auto]"
							>
								<span class="flex min-w-0 flex-col gap-0.5">
									<span class="truncate text-[15px] font-semibold">{band.name}</span>
									<span class="truncate text-[13px] text-muted-foreground tabular">
										{megahertz(band.low)} to {megahertz(band.high)} MHz
									</span>
									<span class="flex flex-wrap gap-x-2 text-[13px]">
										<StatusBadge {condition} class="text-[13px]" />
										{#if band.module}
											<span class="truncate text-muted-foreground">
												{band.module.device?.name ?? `${band.module.module} ${band.module.version}`}{band.module.state === 'streaming' && band.module.dropped > 0
													? ` · ${band.module.dropped.toLocaleString()} samples dropped`
													: ''}
											</span>
											{#if band.module.state === 'streaming' && (band.module.clipping ?? 0) > 1e-4}
												<span class="text-warning">clipping {share(band.module.clipping ?? 0)}</span>
											{/if}
										{/if}
									</span>
								</span>
								<span class="col-span-2 row-start-2 grid grid-cols-2 gap-4 @md:col-span-1 @md:row-start-auto @md:grid-cols-1 @md:gap-2">
									<span class="flex items-center justify-between gap-2">
										<span class="text-[13px] text-muted-foreground tabular">
											<AnimatedNumber value={band.listeners} /> listening
										</span>
										<Sparkline values={series?.listeners ?? []} width={52} height={18} label="{band.name} listeners" />
									</span>
									<span class="flex items-center justify-between gap-2">
										<span class="text-[13px] text-muted-foreground tabular">
											{band.noise_floor > -159 ? dbfs(band.noise_floor, 0) : 'no floor yet'}
										</span>
										<Sparkline
											values={(series?.floor ?? []).filter((value) => value > -159)}
											width={52}
											height={18}
											class="text-signal"
											label="{band.name} noise floor"
										/>
									</span>
								</span>
								<ArrowRight
									size={18}
									class="col-start-2 row-start-1 text-muted-foreground transition-transform duration-300 ease-(--ease-spring) group-hover:translate-x-1 @md:col-start-3"
								/>
							</a>
						</li>
					{/each}
				</ul>
			{/if}
		</section>

		<ListenerList />

		{#if problems.length > 0}
			<section class="flex flex-col gap-2">
				<div class="flex items-baseline justify-between gap-4 px-4">
					<h2 class="text-sm font-medium text-muted-foreground">Recent problems</h2>
					<a href={href({ page: 'log' })} class="-my-2 py-2 text-sm font-medium text-foreground hover:underline">
						Whole log
					</a>
				</div>
				<ul class="flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
					{#each problems as problem, index (index)}
						<li class="flex flex-col gap-0.5 px-4 py-3">
							<span class="flex flex-wrap items-baseline gap-x-2 text-[13px] text-muted-foreground tabular">
								{problem.time}
								<span class={problem.level === 'error' ? 'text-destructive' : 'text-warning'}>{problem.where}</span>
								{#if problem.count > 1}<span>· {problem.count} times since {problem.since}</span>{/if}
							</span>
							<span class="min-w-0 text-[14px] leading-relaxed break-words">{problem.text}</span>
						</li>
					{/each}
				</ul>
			</section>
		{/if}
	{/snippet}
</Columns>

{#if restarting}
	<Confirm
		bind:open={confirmOpen}
		title="Restart {restarting.name}?"
		description="The band stops and starts again. Its listeners stay connected and hear a short gap. A module band also takes the module settings saved in the configuration since it started."
		action="Restart"
		onConfirm={() => restarting && restart(restarting)}
	/>
{/if}
