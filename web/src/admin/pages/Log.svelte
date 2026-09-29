<script lang="ts">
	import Copy from '@lucide/svelte/icons/copy';
	import ScrollText from '@lucide/svelte/icons/scroll-text';
	import Search from '@lucide/svelte/icons/search';
	import X from '@lucide/svelte/icons/x';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import EmptyState from '../components/EmptyState.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import { Button } from '../components/ui/button/index';
	import { Segmented } from '../components/ui/segmented/index';
	import { collapseRepeats, parseLogLine, type LogEntry } from '../lib/log';

	/*
	The last few hundred lines the receiver logged, newest first, so why a band went quiet is one
	glance away without a shell on the machine. Warnings and errors can be picked out, and the
	lines copied as they are for a bug report.
	*/
	type Show = 'all' | 'problems' | 'errors';

	let lines = $state<string[]>([]);
	let loaded = $state(false);
	let loadError = $state('');
	let show = $state<Show>('all');
	let query = $state('');

	onMount(() => {
		let active = true;
		let timer = 0;
		const load = async () => {
			if (!document.hidden) {
				try {
					const result = await api.log();
					if (!active) return;
					lines = result.lines;
					loaded = true;
					loadError = '';
				} catch (problem) {
					if (!active) return;
					loadError = (problem as ApiError).message;
				}
			}
			if (active) timer = window.setTimeout(load, 3000);
		};
		void load();
		return () => {
			active = false;
			window.clearTimeout(timer);
		};
	});

	const needle = $derived(query.trim().toLowerCase());
	const parsed = $derived(lines.map(parseLogLine));
	const entries = $derived(collapseRepeats(parsed).reverse());
	const wanted = (entry: LogEntry) =>
		(show === 'all' || entry.level === 'error' || (show === 'problems' && entry.level === 'warn')) &&
		(needle === '' || entry.raw.toLowerCase().includes(needle));
	const shown = $derived(entries.filter(wanted));
	// Keys for the list: two identical lines in the same millisecond are still two lines.
	const keyed = $derived.by(() => {
		const seen = new Map<string, number>();
		return shown.map((entry) => {
			const n = seen.get(entry.raw) ?? 0;
			seen.set(entry.raw, n + 1);
			return { entry, key: `${entry.raw}#${n}` };
		});
	});

	async function copy() {
		// Every matching line, oldest first, the way the file has them: what a bug report wants.
		const copied = parsed.filter(wanted).map((entry) => entry.raw);
		try {
			await navigator.clipboard.writeText(copied.join('\n'));
			toast.success(`Copied ${copied.length} line${copied.length === 1 ? '' : 's'}`);
		} catch {
			toast.error('The browser did not allow copying. Select the lines and copy them instead.');
		}
	}

	const levelTone: Record<LogEntry['level'], string> = {
		error: 'text-destructive',
		warn: 'text-warning',
		info: 'text-muted-foreground',
		debug: 'text-muted-foreground/70',
		plain: 'text-muted-foreground'
	};
	const levelWord: Record<LogEntry['level'], string> = {
		error: 'Error',
		warn: 'Warning',
		info: '',
		debug: 'Debug',
		plain: ''
	};
</script>

<PageHeader title="Log" description="What the receiver has written lately, newest first.">
	{#snippet actions()}
		<Button variant="secondary" size="lg" class="h-11 rounded-full px-4 md:h-9" disabled={shown.length === 0} onclick={copy}>
			<Copy /> Copy
		</Button>
	{/snippet}
</PageHeader>

<div class="sticky top-14 z-20 -mx-(--gutter) mb-4 flex flex-col gap-3 bg-background/85 px-(--gutter) py-3 backdrop-blur-xl sm:flex-row sm:items-center md:top-0">
	<Segmented
		label="Which lines"
		class="sm:w-80"
		options={[
			{ value: 'all', label: 'Everything' },
			{ value: 'problems', label: 'Problems' },
			{ value: 'errors', label: 'Errors' }
		]}
		value={show}
		onChange={(value) => (show = value)}
	/>
	<label class="relative flex min-w-0 flex-1 items-center">
		<Search size={17} class="pointer-events-none absolute left-3.5 text-muted-foreground" />
		<input
			type="search"
			bind:value={query}
			placeholder="Search the log"
			aria-label="Search the log"
			class="h-11 w-full min-w-0 rounded-xl bg-muted pr-10 pl-10 text-[15px] outline-none placeholder:text-muted-foreground focus-visible:ring-3 focus-visible:ring-ring/50 [&::-webkit-search-cancel-button]:hidden"
		/>
		{#if query}
			<button
				type="button"
				class="pressable absolute right-1.5 grid size-10 md:size-8 place-items-center rounded-full text-muted-foreground hover:text-foreground"
				aria-label="Clear the search"
				onclick={() => (query = '')}
			>
				<X size={16} />
			</button>
		{/if}
	</label>
</div>

{#if loadError && !loaded}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !loaded}
	<div class="flex flex-col gap-2" aria-busy="true">
		{#each [0, 1, 2, 3, 4] as placeholder (placeholder)}
			<div class="shimmer h-12 rounded-xl bg-card"></div>
		{/each}
	</div>
{:else if entries.length === 0}
	<EmptyState icon={ScrollText} title="Nothing logged yet.">
		Warnings and errors appear here as they happen. An empty log is the good case.
	</EmptyState>
{:else if shown.length === 0}
	<EmptyState icon={Search} title={needle ? 'No line matches.' : show === 'errors' ? 'No errors.' : 'No warnings or errors.'}>
		{needle ? `Nothing here contains “${query.trim()}”.` : 'Among the last lines the receiver kept, that is.'}
	</EmptyState>
{:else}
	<p class="mb-2 px-1 text-[13px] text-muted-foreground" aria-live="polite">
		{shown.length === entries.length ? `${entries.length} entries` : `${shown.length} of ${entries.length} entries`}
	</p>
	<ol class="@container flex flex-col divide-y divide-border overflow-hidden rounded-2xl bg-card">
		{#each keyed as { entry, key } (key)}
			<li class="grid grid-cols-1 gap-x-4 gap-y-0.5 px-4 py-2.5 @2xl:grid-cols-[4.5rem_8rem_minmax(0,1fr)] @2xl:items-baseline">
				<span class="flex items-baseline gap-2 text-[13px] text-muted-foreground tabular @2xl:contents">
					<span>{entry.time}</span>
					<span class="truncate {levelTone[entry.level]}">
						{#if levelWord[entry.level]}<span class="font-medium">{levelWord[entry.level]}</span>{entry.where ? ` · ${entry.where}` : ''}{:else}{entry.where}{/if}
					</span>
				</span>
				<span class="min-w-0 text-[14px] leading-relaxed break-words whitespace-pre-wrap {entry.level === 'debug' ? 'text-muted-foreground' : ''}">
					{entry.text}
					{#if entry.count > 1}
						<span class="text-[13px] whitespace-nowrap text-muted-foreground tabular">· {entry.count} times since {entry.since}</span>
					{/if}
				</span>
			</li>
		{/each}
	</ol>
{/if}
