<script lang="ts">
	import { toast } from 'svelte-sonner';
	import { api, ApiError, type BandState, type LiveSetting } from '../api';
	import { share } from '../lib/format';
	import { live } from '../lib/live.svelte';
	import { typedNumber } from '../lib/number';
	import ChoiceSheet from './ChoiceSheet.svelte';
	import EditSheet from './EditSheet.svelte';
	import SettingsGroup from './SettingsGroup.svelte';
	import SettingsRow from './SettingsRow.svelte';
	import * as Dialog from './ui/dialog/index';
	import { Input } from './ui/input/index';
	import { Switch } from './ui/switch/index';

	/*
	The module behind a band: what it runs on, whether it keeps up, and the settings it can change
	while listeners stay on. The value shown is the one the module reports back, so a gain snapped
	to the tuner's nearest step shows as that step.
	*/
	let { band }: { band: BandState } = $props();

	const run = $derived(band.module!);
	let choosing = $state<LiveSetting | null>(null);
	let choosingOpen = $state(false);
	let editing = $state<LiveSetting | null>(null);
	let editingOpen = $state(false);
	let draft = $state('');
	let logOpen = $state(false);
	let log = $state<string[]>([]);

	function current(setting: LiveSetting): string {
		const value = run.settings?.[setting.key];
		return value === undefined ? '' : String(value);
	}

	/** Sends one setting to the running module; throws, with the reason, if it cannot. */
	async function send(setting: LiveSetting, raw: unknown) {
		let value: string | number | boolean;
		if (setting.type === 'number') {
			const number = typedNumber(raw);
			if (number === null) throw new Error(`${setting.label} needs a number.`);
			if ((setting.min !== undefined && number < setting.min) || (setting.max !== undefined && number > setting.max)) {
				throw new Error(`${setting.label} goes from ${setting.min ?? 'any'} to ${setting.max ?? 'any'}${setting.unit ? ` ${setting.unit}` : ''}.`);
			}
			value = number;
		} else if (setting.type === 'boolean') {
			value = raw === true;
		} else {
			value = String(raw ?? '');
		}
		await api.setModule(band.id, { [setting.key]: value });
	}

	/** For controls without a sheet to say it in. */
	function sendOrSay(setting: LiveSetting, raw: unknown) {
		send(setting, raw)
			.then(() => live.refresh())
			.catch((problem) => toast.error((problem as ApiError).message));
	}

	// A switch shows the value it was moved to while the module is being asked, then whatever the
	// module reports. A refused change falls back to the reported value instead of staying moved.
	let moving = $state<Record<string, boolean>>({});
	function flip(setting: LiveSetting, next: boolean) {
		moving[setting.key] = next;
		send(setting, next)
			.then(() => live.refresh())
			.catch((problem) => toast.error((problem as ApiError).message))
			.finally(() => delete moving[setting.key]);
	}

	async function showLog() {
		log = run.log;
		logOpen = true;
		try {
			log = (await api.moduleLog(band.id)).log;
		} catch (problem) {
			toast.error((problem as ApiError).message);
		}
	}
</script>

<SettingsGroup title="Input">
	<SettingsRow label="Module" value="{run.module} {run.version}" />
	{#if run.device?.name}<SettingsRow label="Device" value={String(run.device.name)} />{/if}
	{#if run.device?.serial !== undefined}<SettingsRow label="Serial" value={String(run.device.serial)} />{/if}
	{#if run.state === 'streaming'}
		{#if run.center !== undefined}
			<SettingsRow
				label="Tuned to"
				value="{run.center.toLocaleString(undefined, { minimumFractionDigits: 1, maximumFractionDigits: 2 })} Hz"
			/>
		{/if}
		{#if run.gain_now !== undefined}
			<SettingsRow label="Gain now" detail="Chosen by the module" value="{run.gain_now.toFixed(1)} dB" />
		{/if}
		{#if run.clipping !== undefined}
			<SettingsRow
				label="Clipping"
				detail="Samples at the converter's limit, in the last second"
				value={run.clipping > 0 ? share(run.clipping) : 'none'}
				tone={run.clipping > 1e-4 ? 'text-warning' : ''}
			/>
		{/if}
		<SettingsRow label="Samples dropped" value={run.dropped.toLocaleString()} />
		{#each run.live as setting (setting.key)}
			{#if setting.type === 'boolean'}
				<SettingsRow label={setting.label}>
					{#snippet control()}
						<Switch
							bind:checked={() => moving[setting.key] ?? current(setting) === 'true', (checked) => flip(setting, checked)}
							aria-label={setting.label}
						/>
					{/snippet}
				</SettingsRow>
			{:else if setting.type === 'choice'}
				<SettingsRow
					label={setting.label}
					value={current(setting)}
					onclick={() => {
						choosing = setting;
						choosingOpen = true;
					}}
				/>
			{:else}
				<SettingsRow
					label={setting.label}
					value="{current(setting)}{setting.unit && current(setting) ? ` ${setting.unit}` : ''}"
					onclick={() => {
						editing = setting;
						draft = current(setting);
						editingOpen = true;
					}}
				/>
			{/if}
		{/each}
	{:else if run.exit}
		<SettingsRow label="Last run" detail={run.exit} />
	{/if}
	{#if run.log.length > 0}
		<SettingsRow label="What the module said" onclick={showLog} />
	{/if}
	{#snippet footer()}
		{#if run.last_set && !run.last_set.ok}
			<span class="text-destructive">The module refused the last change: {run.last_set.message}</span>
		{:else if run.live.length > 0 && run.state === 'streaming'}
			Changes reach the device at once, and listeners stay on.
		{/if}
	{/snippet}
</SettingsGroup>

{#if choosing}
	{@const setting = choosing}
	<ChoiceSheet
		bind:open={choosingOpen}
		title={setting.label}
		options={(setting.choices ?? []).map((choice) => ({ value: choice, label: choice }))}
		value={current(setting)}
		onChange={(value) => sendOrSay(setting, value)}
	/>
{/if}

{#if editing}
	{@const setting = editing}
	<EditSheet
		bind:open={editingOpen}
		title={setting.label}
		description={setting.type === 'number' && setting.min !== undefined && setting.max !== undefined
			? `From ${setting.min} to ${setting.max}${setting.unit ? ` ${setting.unit}` : ''}.`
			: undefined}
		action="Set"
		onSave={() => send(setting, draft).then(() => live.refresh())}
	>
		<label class="flex flex-col gap-1.5 text-sm font-medium">
			{setting.label}{setting.unit ? `, ${setting.unit}` : ''}
			<Input
				type={setting.type === 'number' ? 'number' : 'text'}
				step="any"
				min={setting.min}
				max={setting.max}
				bind:value={draft}
				class="h-11 tabular"
			/>
		</label>
	</EditSheet>
{/if}

<Dialog.Root bind:open={logOpen}>
	<Dialog.Content class="gap-3 rounded-3xl p-5 sm:max-w-2xl">
		<Dialog.Title class="pr-10 text-lg font-semibold">What {run.module} said</Dialog.Title>
		<pre class="max-h-[60dvh] overflow-auto rounded-2xl bg-muted/60 p-4 text-xs leading-relaxed break-words whitespace-pre-wrap">{log.join('\n')}</pre>
	</Dialog.Content>
</Dialog.Root>
