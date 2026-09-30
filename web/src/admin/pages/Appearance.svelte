<script lang="ts">
	import Check from '@lucide/svelte/icons/check';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { PALETTES, paletteGradient } from '../../render/palettes';
	import { api, ApiError } from '../api';
	import ColorRow from '../components/ColorRow.svelte';
	import Columns from '../components/Columns.svelte';
	import ImageField from '../components/ImageField.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SaveBar from '../components/SaveBar.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import SettingsRow from '../components/SettingsRow.svelte';
	import SliderRow from '../components/SliderRow.svelte';
	import ThemePreview from '../components/ThemePreview.svelte';
	import Uploads from '../components/Uploads.svelte';
	import { Confirm } from '../components/ui/confirm/index';
	import { Segmented } from '../components/ui/segmented/index';
	import { matchesPreset, PRESETS, RECEIVER_DEFAULTS, TOKEN_GROUPS, withLook, type Theme } from '../lib/theme';

	/*
	How the receiver looks to everyone. Nothing reaches listeners until Apply, so an operator can
	try colours without strobing everyone who is listening; the preview beside the settings shows
	each change as it is made.
	*/
	let theme = $state<Theme | null>(null);
	let saved = $state('');
	let loadError = $state('');
	let busy = $state(false);
	let resetOpen = $state(false);
	let savedCount = $state(0);
	let station = $state<{ name?: string; operator?: string; location?: string }>({});
	// Which of the receiver's schemes the preview shows; unset colours are drawn in its values.
	let scheme = $state<'dark' | 'light'>('dark');

	onMount(() => {
		api
			.readTheme()
			.then((result) => {
				theme = normalise(result.theme as Theme);
				saved = JSON.stringify(theme);
			})
			.catch((problem) => (loadError = (problem as Error).message));
		api
			.readStation()
			.then((result) => (station = result.station as typeof station))
			.catch(() => undefined);
	});

	/** Fills in what the page edits, so a missing field and its default compare equal. */
	function normalise(value: Theme): Theme {
		return {
			...value,
			colors: { ...(value.colors ?? {}) },
			background: { image: '', opacity: 0.35, blur: 0, position: 'cover', ...(value.background ?? {}) },
			palette: value.palette ?? 'classic',
			meter: value.meter ?? 'bar'
		};
	}

	const dirty = $derived(theme !== null && JSON.stringify(theme) !== saved);

	function setColor(key: string, value: string | null) {
		if (!theme) return;
		const colors = { ...(theme.colors ?? {}) };
		if (value === null) delete colors[key];
		else colors[key] = value;
		theme.colors = colors;
	}

	async function apply() {
		if (!theme) return;
		busy = true;
		const sending = JSON.stringify(theme);
		try {
			const current = (await api.readTheme()).theme as Theme;
			const result = await api.writeTheme(withLook(JSON.parse(sending), current));
			const applied = normalise(result.theme as Theme);
			saved = JSON.stringify(applied);
			// The receiver's version of what was sent, unless the operator kept changing things.
			if (JSON.stringify(theme) === sending) theme = applied;
			savedCount++;
			toast.success('Applied. Everyone listening sees it now.');
		} catch (problem) {
			toast.error((problem as ApiError).message);
		} finally {
			busy = false;
		}
	}

	async function reset() {
		try {
			const result = await api.resetTheme();
			theme = normalise(result.theme as Theme);
			saved = JSON.stringify(theme);
			savedCount++;
			toast.success('Back to the built-in look, for everyone.');
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}

	const meters: { value: NonNullable<Theme['meter']>; label: string; detail: string }[] = [
		{ value: 'bar', label: 'Bar', detail: 'The default: a level and its recent peak' },
		{ value: 'needle', label: 'Needle', detail: 'An analogue movement against a printed scale' },
		{ value: 'numeric', label: 'Numbers', detail: 'The level and the peak as figures, for an instrument' },
		{ value: 'history', label: 'Trace', detail: 'The last half minute: fading or coming up' }
	];
</script>

<PageHeader title="Appearance" description="How the receiver looks to everyone. Nothing changes for listeners until you apply it." />

{#if loadError}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !theme}
	<div class="flex max-w-2xl flex-col gap-3" aria-busy="true">
		<div class="shimmer h-28 rounded-2xl bg-card"></div>
		<div class="shimmer h-64 rounded-2xl bg-card"></div>
	</div>
{:else}
	{@const current = theme}
	{@const background = current.background!}
	<Columns asideFirst asideLabel="Preview">
		{#snippet aside()}
			<ThemePreview theme={current} {station} bind:scheme />
		{/snippet}

		{#snippet main()}
			<SettingsGroup title="Start from" footer="A preset sets the colours below; change any of them afterwards.">
				<fieldset class="grid grid-cols-2 gap-2 p-3 sm:grid-cols-4">
					<legend class="sr-only">Colour presets</legend>
					{#each PRESETS as preset (preset.name)}
						{@const active = matchesPreset(current.colors, preset.colors)}
						{@const colors = { ...RECEIVER_DEFAULTS.dark, ...preset.colors }}
						<label
							class="pressable flex cursor-pointer flex-col gap-2 rounded-xl p-1.5 pb-2 transition-colors hover:bg-accent/60 has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-ring {active
								? 'bg-accent/70 ring-2 ring-foreground/70'
								: ''}"
						>
							<input
								type="radio"
								name="preset"
								class="sr-only"
								checked={active}
								onchange={() => (current.colors = { ...preset.colors })}
							/>
							<span class="flex h-12 overflow-hidden rounded-lg shadow-[0_0_0_1px_var(--border)]" aria-hidden="true">
								{#each ['background', 'card', 'foreground', 'primary', 'signal'] as key (key)}
									<span class="flex-1" style="background: {colors[key]}"></span>
								{/each}
							</span>
							<span class="flex items-center gap-1 px-1 text-[13px] font-medium">
								{preset.name}
								{#if active}<Check size={14} aria-hidden="true" />{/if}
							</span>
						</label>
					{/each}
				</fieldset>
			</SettingsGroup>

			{#each TOKEN_GROUPS as group (group.title)}
				<SettingsGroup title={group.title}>
					{#each group.tokens as token (token.key)}
						<ColorRow
							label={token.label}
							hint={token.hint}
							value={current.colors?.[token.key]}
							fallback={RECEIVER_DEFAULTS[scheme][token.key]}
							onChange={(value) => setColor(token.key, value)}
						/>
					{/each}
				</SettingsGroup>
			{/each}

			<SettingsGroup
				title="Background picture"
				footer="It sits behind the whole page, and the receiver's panels turn translucent over it."
			>
				<ImageField label="Background picture" value={background.image} onChange={(image) => (background.image = image)} />
				{#if background.image}
					<SliderRow label="Strength" min={0} max={1} step={0.01} bind:value={background.opacity} format={(value) => `${Math.round(value * 100)}%`} />
					<SliderRow label="Blur" min={0} max={40} bind:value={background.blur} format={(value) => `${value} px`} />
					<div class="flex items-center justify-between gap-3 px-4 py-2.5">
						<span class="text-[15px]">Fit</span>
						<Segmented
							label="How the picture fills the page"
							class="w-44"
							options={[
								{ value: 'cover', label: 'Fill' },
								{ value: 'tile', label: 'Tile' }
							]}
							value={background.position === 'tile' ? 'tile' : 'cover'}
							onChange={(value) => (background.position = value)}
						/>
					</div>
				{/if}
			</SettingsGroup>

			<SettingsGroup title="Waterfall colours" footer="What a new visitor starts with. Everyone can pick their own under Display.">
				<fieldset class="flex flex-col divide-y divide-border">
					<legend class="sr-only">Waterfall colours</legend>
					{#each PALETTES as palette (palette.id)}
						{@const active = (current.palette ?? 'classic') === palette.id}
						<label
							class="flex min-h-13 cursor-pointer items-center gap-3 px-4 py-2.5 transition-colors hover:bg-accent/50 has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-ring has-[:focus-visible]:ring-inset"
						>
							<input type="radio" name="palette" value={palette.id} class="sr-only" bind:group={current.palette} />
							<span class="h-7 w-16 shrink-0 rounded-md shadow-[0_0_0_1px_var(--border)]" style="background: {paletteGradient(palette.id)}" aria-hidden="true"></span>
							<span class="flex min-w-0 flex-1 flex-col">
								<span class="text-[15px] {active ? 'font-semibold' : ''}">{palette.label}</span>
								<span class="text-[13px] text-muted-foreground">{palette.description}</span>
							</span>
							{#if active}<Check size={18} class="shrink-0" aria-hidden="true" />{/if}
						</label>
					{/each}
				</fieldset>
			</SettingsGroup>

			<SettingsGroup title="Signal meter" footer="All of them read the same calibrated level.">
				<fieldset class="flex flex-col divide-y divide-border">
					<legend class="sr-only">Signal meter</legend>
					{#each meters as option (option.value)}
						{@const active = (current.meter ?? 'bar') === option.value}
						<label
							class="flex min-h-13 cursor-pointer items-center gap-3 px-4 py-2.5 transition-colors hover:bg-accent/50 has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-ring has-[:focus-visible]:ring-inset"
						>
							<input type="radio" name="meter" value={option.value} class="sr-only" bind:group={current.meter} />
							<span class="flex min-w-0 flex-1 flex-col">
								<span class="text-[15px] {active ? 'font-semibold' : ''}">{option.label}</span>
								<span class="text-[13px] text-muted-foreground">{option.detail}</span>
							</span>
							{#if active}<Check size={18} class="shrink-0" aria-hidden="true" />{/if}
						</label>
					{/each}
				</fieldset>
			</SettingsGroup>

			<Uploads refresh={savedCount} />

			<SettingsGroup footer="Removes every colour, the picture and the choices above, for everyone listening, at once.">
				<SettingsRow label="Back to the built-in look" tone="font-medium text-destructive" onclick={() => (resetOpen = true)} />
			</SettingsGroup>
		{/snippet}
	</Columns>

	<SaveBar visible={dirty} message="Not applied yet" save="Apply to everyone" {busy} onSave={apply} onDiscard={() => (theme = normalise(JSON.parse(saved)))} />

	<Confirm
		bind:open={resetOpen}
		title="Go back to the built-in look?"
		description="Every colour, the background picture, the waterfall colours and the meter go back to the receiver's own, on every open page. Pictures stay on the receiver."
		action="Reset"
		destructive
		onConfirm={reset}
	/>
{/if}
