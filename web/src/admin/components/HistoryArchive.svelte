<script lang="ts">
	import { bytes } from '../lib/format';
	import ChoiceSheet from './ChoiceSheet.svelte';
	import SettingsGroup from './SettingsGroup.svelte';
	import SettingsRow from './SettingsRow.svelte';

	/*
	Whether this band keeps a record of its waterfall, and who may read it. What it costs on disk is
	said before it is switched on, and so is what it means: a record of a band is also a record of
	who transmitted when.
	*/
	type Access = 'off' | 'private' | 'public';

	let {
		access,
		hours,
		bins,
		interval,
		onChange
	}: {
		access: Access;
		hours: number;
		bins: number;
		interval: number;
		onChange: (next: { history?: Access; history_hours?: number }) => void;
	} = $props();

	const sizeFor = (span: number) => 48 + Math.max(1, Math.round((span * 3600) / interval)) * (8 + bins);
	const accessNames: Record<Access, string> = { off: 'Off', private: 'Only you', public: 'Anyone' };

	let choosingAccess = $state(false);
	let choosingSpan = $state(false);
</script>

<SettingsGroup
	title="Waterfall history"
	footer={access === 'off'
		? 'Nothing is recorded.'
		: `The file is ${bytes(sizeFor(hours))} and is made at that size straight away, so it can never fill the disk. Changing how long it keeps starts the record again.${access === 'public' ? ' Anyone with the link can read it.' : ''}`}
>
	<SettingsRow label="Who can look back" value={accessNames[access]} onclick={() => (choosingAccess = true)} />
	{#if access !== 'off'}
		<SettingsRow label="Keeps" value="{hours} hours" onclick={() => (choosingSpan = true)} />
	{/if}
</SettingsGroup>

<ChoiceSheet
	bind:open={choosingAccess}
	title="Who can look back"
	options={[
		{ value: 'off' as Access, label: 'Off', detail: 'Nothing is recorded' },
		{ value: 'private' as Access, label: 'Only you', detail: 'In this panel' },
		{ value: 'public' as Access, label: 'Anyone', detail: 'A record of who transmitted when, public' }
	]}
	value={access}
	onChange={(next) => onChange({ history: next })}
/>

<ChoiceSheet
	bind:open={choosingSpan}
	title="How long it keeps"
	options={[6, 12, 24, 48, 72].map((span) => ({ value: span, label: `${span} hours`, detail: `${bytes(sizeFor(span))} on disk` }))}
	value={hours}
	onChange={(span) => onChange({ history_hours: span })}
/>
