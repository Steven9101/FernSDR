<script lang="ts">
	import { fade, flip, fly } from '../lib/motion';
	import ArrowDown from '@lucide/svelte/icons/arrow-down';
	import ArrowUp from '@lucide/svelte/icons/arrow-up';
	import ChevronRight from '@lucide/svelte/icons/chevron-right';
	import Clock from '@lucide/svelte/icons/clock';
	import ExternalLink from '@lucide/svelte/icons/external-link';
	import ImageIcon from '@lucide/svelte/icons/image';
	import LayoutGrid from '@lucide/svelte/icons/layout-grid';
	import Link2 from '@lucide/svelte/icons/link-2';
	import MessageSquare from '@lucide/svelte/icons/message-square';
	import Plus from '@lucide/svelte/icons/plus';
	import Radio from '@lucide/svelte/icons/radio';
	import StickyNote from '@lucide/svelte/icons/sticky-note';
	import Trash2 from '@lucide/svelte/icons/trash-2';
	import Globe from '@lucide/svelte/icons/globe';
	import MapPin from '@lucide/svelte/icons/map-pin';
	import Sun from '@lucide/svelte/icons/sun';
	import Zap from '@lucide/svelte/icons/zap';
	import { onMount, type Component } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import Columns from '../components/Columns.svelte';
	import EmptyState from '../components/EmptyState.svelte';
	import ImageField from '../components/ImageField.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SaveBar from '../components/SaveBar.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import SliderRow from '../components/SliderRow.svelte';
	import TextRow from '../components/TextRow.svelte';
	import * as Dialog from '../components/ui/dialog/index';
	import { Button } from '../components/ui/button/index';
	import type { Theme } from '../lib/theme';
	import type { ThemeWidget } from '../../state/theme';

	/*
	What sits beside the receiver for everyone who visits: a chat, a lightning map, a clock, links
	to the operator's other receivers. The set is closed on purpose. An open one means either a
	frame pointing anywhere or a field of markup; `embed` is there for the operator who really needs
	an arbitrary page, and it says plainly what that means.
	*/
	type Field = 'url' | 'text' | 'items' | 'height';
	interface Kind {
		type: ThemeWidget['type'];
		label: string;
		description: string;
		icon: Component<{ size?: number; class?: string }>;
		fields: Field[];
	}

	const KINDS: Kind[] = [
		{ type: 'chat', label: 'Chat', description: 'Listeners talk to each other. It runs on this receiver; nothing leaves it.', icon: MessageSquare, fields: ['height'] },
		{ type: 'lightning', label: 'Lightning map', description: 'A live strike map, for explaining the crashes on 80 m.', icon: Zap, fields: ['url', 'height'] },
		{ type: 'clock', label: 'UTC clock', description: 'The time everyone on the air is using.', icon: Clock, fields: [] },
		{ type: 'space', label: 'Space weather', description: 'Solar flux, Kp, the solar wind, and which bands are open here, from NOAA and the nearest ionosonde. The receiver fetches it every ten minutes while this is on the page.', icon: Sun, fields: [] },
		{ type: 'greyline', label: 'Day and night', description: 'The world by day and night with the greyline, this station marked, and its sunrise and sunset.', icon: Globe, fields: [] },
		{ type: 'station', label: 'Station card', description: 'Where this receiver is, on a map of the region around it, with the operator, locator, antenna and what it listens with, from the Station page.', icon: MapPin, fields: [] },
		{ type: 'spots', label: 'Listeners by band', description: 'Where people are listening on this receiver right now.', icon: Radio, fields: [] },
		{ type: 'notice', label: 'Notice', description: 'A few lines of your own text. Plain text, no markup.', icon: StickyNote, fields: ['text'] },
		{ type: 'links', label: 'Links', description: 'Your other receivers, your club, your QRZ page.', icon: Link2, fields: ['items'] },
		{ type: 'image', label: 'Picture', description: 'A photo of the antenna, a QSL card, a map.', icon: ImageIcon, fields: ['url'] },
		{ type: 'embed', label: 'Embedded page', description: 'Any page in a frame. It can do whatever that page does, so only embed one you trust.', icon: ExternalLink, fields: ['url', 'height'] }
	];
	// The lightning map needs no address: empty, the receiver shows Blitzortung's, centred on its locator.
	const DEFAULT_URL: Partial<Record<ThemeWidget['type'], string>> = {};
	const kindOf = (type: ThemeWidget['type']) => KINDS.find((kind) => kind.type === type);

	// Each entry carries a key of its own for the list's animation; it never goes to the server.
	type Entry = ThemeWidget & { key: number };
	let theme = $state<Theme | null>(null);
	let widgets = $state<Entry[]>([]);
	let saved = $state('');
	let loadError = $state('');
	let busy = $state(false);
	let editing = $state<number | null>(null);
	let sheetOpen = $state(false);
	let nextKey = 0;

	const plain = (list: Entry[]): ThemeWidget[] => list.map(({ key: _key, ...widget }) => widget);

	/** Every field its kind edits, present, so the fields can bind to it. */
	function complete(widget: ThemeWidget): Entry {
		const fields = kindOf(widget.type)?.fields ?? [];
		return {
			...widget,
			title: widget.title ?? kindOf(widget.type)?.label ?? '',
			...(fields.includes('url') ? { url: widget.url ?? DEFAULT_URL[widget.type] ?? '' } : {}),
			...(fields.includes('text') ? { text: widget.text ?? '' } : {}),
			...(fields.includes('items') ? { items: widget.items ?? [] } : {}),
			...(fields.includes('height') ? { height: widget.height ?? 260 } : {}),
			key: nextKey++
		};
	}

	onMount(() => {
		api
			.readTheme()
			.then((result) => {
				theme = result.theme as Theme;
				widgets = (theme.widgets ?? []).map(complete);
				saved = JSON.stringify(plain(widgets));
			})
			.catch((problem) => (loadError = (problem as Error).message));
	});

	const dirty = $derived(theme !== null && JSON.stringify(plain(widgets)) !== saved);
	const current = $derived(widgets.find((widget) => widget.key === editing));

	function add(kind: Kind) {
		const widget = complete({ type: kind.type, title: kind.label });
		widgets = [...widgets, widget];
		// Anything with something to fill in opens straight away; a clock has nothing to ask.
		if (kind.fields.some((field) => field !== 'height')) open(widget.key);
		else toast.success(`${kind.label} added at the end`);
	}

	function open(key: number) {
		editing = key;
		sheetOpen = true;
	}

	function move(index: number, by: number) {
		const target = index + by;
		if (target < 0 || target >= widgets.length) return;
		const next = [...widgets];
		[next[index], next[target]] = [next[target], next[index]];
		widgets = next;
	}

	function remove(key: number) {
		widgets = widgets.filter((widget) => widget.key !== key);
		sheetOpen = false;
	}

	async function apply() {
		if (!theme) return;
		busy = true;
		const sending = JSON.stringify(plain(widgets));
		try {
			const result = await api.writeTheme({ ...theme, widgets: JSON.parse(sending) });
			theme = result.theme as Theme;
			const applied = (theme.widgets ?? []).map(complete);
			saved = JSON.stringify(plain(applied));
			// The receiver's version of what was sent, unless the operator kept changing things.
			if (JSON.stringify(plain(widgets)) === sending) widgets = applied;
			toast.success('Applied. Everyone listening has them now.');
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			busy = false;
		}
	}

	function summary(widget: ThemeWidget): string {
		const kind = kindOf(widget.type);
		const parts = [kind?.label ?? widget.type];
		if (widget.type === 'links') parts.push(`${widget.items?.length ?? 0} link${widget.items?.length === 1 ? '' : 's'}`);
		else if (widget.type === 'notice') parts.push(widget.text?.trim() ? `${widget.text.trim().slice(0, 40)}${widget.text.trim().length > 40 ? '...' : ''}` : 'no text yet');
		else if ((widget.type === 'image' || widget.type === 'embed') && !widget.url) parts.push('no address yet');
		if (widget.height && kind?.fields.includes('height')) parts.push(`${widget.height} px high`);
		return parts.join(' · ');
	}
</script>

<PageHeader title="Widgets" description="What sits beside the receiver for everyone who visits, in this order." />

{#if loadError}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !theme}
	<div class="flex max-w-2xl flex-col gap-3" aria-busy="true">
		<div class="shimmer h-40 rounded-2xl bg-card"></div>
	</div>
{:else}
	<Columns asideLabel="Add a widget">
		{#snippet main()}
			{#if widgets.length === 0}
				<EmptyState icon={LayoutGrid} title="Nothing added yet.">
					Widgets appear beside the receiver for everyone who visits. Add one from the list.
				</EmptyState>
			{:else}
				<SettingsGroup title="On the receiver" footer="Listeners see them top to bottom, beside the waterfall or under it on a phone.">
					{#each widgets as widget, index (widget.key)}
						{@const kind = kindOf(widget.type)}
						<div
							class="flex min-h-15 items-center gap-1 py-1.5 pr-2 pl-4"
							animate:flip={{ duration: 320 }}
							in:fly={{ y: 8, duration: 260 }}
							out:fade={{ duration: 160 }}
						>
							<button type="button" class="flex min-w-0 flex-1 items-center gap-3 py-1.5 text-left" onclick={() => open(widget.key)}>
								{#if kind}<kind.icon size={19} class="shrink-0 text-muted-foreground" />{/if}
								<span class="flex min-w-0 flex-1 flex-col">
									<span class="truncate text-[15px]">{widget.title || kind?.label}</span>
									<span class="truncate text-[13px] text-muted-foreground">{summary(widget)}</span>
								</span>
								<ChevronRight size={18} class="shrink-0 text-muted-foreground/70" />
							</button>
							<button
								type="button"
								class="pressable grid size-11 md:size-9 shrink-0 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-accent hover:text-foreground disabled:opacity-30"
								disabled={index === 0}
								aria-label="Move {widget.title || kind?.label} up"
								onclick={() => move(index, -1)}
							>
								<ArrowUp size={16} />
							</button>
							<button
								type="button"
								class="pressable grid size-11 md:size-9 shrink-0 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-accent hover:text-foreground disabled:opacity-30"
								disabled={index === widgets.length - 1}
								aria-label="Move {widget.title || kind?.label} down"
								onclick={() => move(index, 1)}
							>
								<ArrowDown size={16} />
							</button>
						</div>
					{/each}
				</SettingsGroup>
			{/if}
		{/snippet}

		{#snippet aside()}
			<SettingsGroup title="Add">
				{#each KINDS as kind (kind.type)}
					<button type="button" class="flex min-h-15 items-center gap-3 px-4 py-2.5 text-left transition-colors hover:bg-accent/50 active:bg-accent" onclick={() => add(kind)}>
						<kind.icon size={19} class="shrink-0 text-muted-foreground" />
						<span class="flex min-w-0 flex-1 flex-col">
							<span class="text-[15px]">{kind.label}</span>
							<span class="text-[13px] leading-snug text-muted-foreground">{kind.description}</span>
						</span>
						<Plus size={18} class="shrink-0 text-muted-foreground" />
					</button>
				{/each}
			</SettingsGroup>
		{/snippet}
	</Columns>

	<Dialog.Root bind:open={sheetOpen}>
		<Dialog.Content class="max-h-[88dvh] gap-4 overflow-y-auto rounded-3xl p-3 pt-5 sm:max-w-lg">
			{#if current}
				{@const kind = kindOf(current.type)}
				<div class="flex flex-col gap-1 px-4 pr-12">
					<Dialog.Title class="text-lg font-semibold">{kind?.label}</Dialog.Title>
					<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">{kind?.description}</Dialog.Description>
				</div>

				<SettingsGroup footer={current.type === 'lightning' ? lightningNote : undefined}>
					<TextRow label="Title" bind:value={current.title} placeholder={kind?.label} />
					{#if kind?.fields.includes('url') && current.type !== 'image'}
						<TextRow label="Address" bind:value={current.url}
							placeholder={current.type === 'lightning' ? 'Blitzortung, around this station' : 'https://'} inputmode="url" />
					{/if}
					{#if kind?.fields.includes('height')}
						<SliderRow label="Height" min={120} max={600} step={20} bind:value={current.height} format={(value) => `${value} px`} />
					{/if}
				</SettingsGroup>

				{#if current.type === 'image'}
					<SettingsGroup title="Picture">
						<ImageField label="Picture" value={current.url ?? ''} onChange={(url) => current && (current.url = url)} />
					</SettingsGroup>
				{/if}

				{#if kind?.fields.includes('text')}
					<SettingsGroup title="Text" footer="Plain text. Line breaks are kept.">
						<label class="block px-4 py-3 focus-within:bg-accent/40">
							<span class="sr-only">Text</span>
							<textarea
								bind:value={current.text}
								rows="4"
								maxlength="2000"
								placeholder="No text yet"
								class="field-sizing-content block max-h-72 min-h-24 w-full resize-none bg-transparent text-[15px] leading-relaxed outline-none placeholder:text-muted-foreground"
							></textarea>
						</label>
					</SettingsGroup>
				{/if}

				{#if kind?.fields.includes('items')}
					{@const items = current.items ?? []}
					<SettingsGroup title="Links">
						{#each items as item, index (index)}
							<div class="flex items-center gap-1 py-1 pr-2">
								<div class="flex min-w-0 flex-1 flex-col divide-y divide-border">
									<TextRow label="Label" bind:value={item.label} />
									<TextRow label="Address" bind:value={item.url} placeholder="https://" inputmode="url" />
								</div>
								<button
									type="button"
									class="pressable grid size-11 md:size-9 shrink-0 place-items-center rounded-full text-muted-foreground hover:bg-destructive/10 hover:text-destructive"
									aria-label="Remove the link {item.label || index + 1}"
									onclick={() => current && (current.items = items.filter((_, other) => other !== index))}
								>
									<Trash2 size={16} />
								</button>
							</div>
						{/each}
						<SettingsRow label="Add a link" tone="font-medium" onclick={() => current && (current.items = [...items, { label: '', url: '' }])} />
					</SettingsGroup>
				{/if}

				{#if current.type === 'embed'}
					<p class="px-4 text-[13px] leading-relaxed text-warning" role="note">
						An embedded page runs in your listeners' browsers. Embed only one you trust.
					</p>
				{/if}

				<div class="flex flex-col gap-2 pb-1">
					<Button size="lg" class="h-11 rounded-full text-[15px]" onclick={() => (sheetOpen = false)}>Done</Button>
					<Button variant="ghost" size="lg" class="h-11 rounded-full text-[15px] text-destructive hover:text-destructive" onclick={() => current && remove(current.key)}>
						<Trash2 /> Remove this widget
					</Button>
				</div>
			{/if}
		</Dialog.Content>
	</Dialog.Root>

	<SaveBar visible={dirty} message="Not applied yet" save="Apply to everyone" {busy} onSave={apply} onDiscard={() => (widgets = (JSON.parse(saved) as ThemeWidget[]).map(complete))} />
{/if}

{#snippet lightningNote()}
	{#if /(^|\/\/|\.)lightningmaps\.org/i.test(current?.url ?? '')}
		<span class="text-warning">lightningmaps.org refuses to be shown inside another page, so listeners would see an empty box. Leave the address empty for Blitzortung's map.</span>
	{:else}
		Empty, it shows Blitzortung's live map around this station's locator (Station page). Another address has to allow being shown in a frame; lightningmaps.org does not.
	{/if}
{/snippet}
