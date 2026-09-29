<script lang="ts">
	import Minus from '@lucide/svelte/icons/minus';
	import { api } from '../api';
	import { formatCalibration, offsetAt, parseCalibration } from '../lib/calibration';
	import { dbfs, signed } from '../lib/format';
	import { typedNumber } from '../lib/number';
	import EditSheet from './EditSheet.svelte';
	import SettingsGroup from './SettingsGroup.svelte';
	import SettingsRow from './SettingsRow.svelte';
	import { Input } from './ui/input/index';

	/*
	The S-meter calibration: a few measurements, each a frequency and the correction there, and
	the line the receiver draws between them. The guided way in is to feed a known level and let
	the receiver read itself, because doing the subtraction by hand is where the sign gets lost.
	*/
	let {
		bandId,
		value,
		lowHz,
		highHz,
		noiseFloorDbfs,
		onChange
	}: {
		bandId: string;
		value: string;
		lowHz: number;
		highHz: number;
		noiseFloorDbfs: number;
		onChange: (value: string) => void;
	} = $props();

	const points = $derived(parseCalibration(value));

	let measuring = $state(false);
	let adding = $state(false);
	let atMhz = $state('');
	let levelDbm = $state('-73');
	let offsetDb = $state('0');

	function openMeasure() {
		atMhz = ((lowHz + highHz) / 2 / 1e6).toFixed(3);
		measuring = true;
	}

	/** Where on this band, in Hz, or why not. */
	function frequency(): number {
		const mhz = typedNumber(atMhz);
		if (mhz === null) throw new Error('Say where, in MHz.');
		const hz = mhz * 1e6;
		if (hz < lowHz || hz > highHz) {
			throw new Error(`That is outside this band, which runs from ${(lowHz / 1e6).toFixed(3)} to ${(highHz / 1e6).toFixed(3)} MHz.`);
		}
		return hz;
	}

	function openAdd() {
		const middle = (lowHz + highHz) / 2;
		atMhz = (middle / 1e6).toFixed(3);
		// At the correction already in force there, so a new point changes nothing until edited.
		offsetDb = String(Math.round(offsetAt(points, middle) * 10) / 10);
		adding = true;
	}

	async function measure() {
		const hz = frequency();
		const known = typedNumber(levelDbm);
		if (known === null) throw new Error('Say what level the generator gives, in dBm.');
		const reading = await api.level(bandId, hz);
		if (reading.dbfs < -159) throw new Error('Nothing has been measured on this band yet.');
		const measured = Math.round((known - reading.dbfs) * 10) / 10;
		onChange(formatCalibration([...points.filter((point) => Math.abs(point.hz - hz) > 1), { hz, offsetDb: measured }]));
	}

	function add() {
		const hz = frequency();
		const offset = typedNumber(offsetDb);
		if (offset === null) throw new Error('Say the offset, in dB.');
		onChange(formatCalibration([...points.filter((point) => Math.abs(point.hz - hz) > 1), { hz, offsetDb: offset }]));
	}

	const WIDTH = 480;
	const HEIGHT = 72;
	const curve = $derived.by(() => {
		if (points.length === 0) return null;
		const offsets = points.map((point) => point.offsetDb);
		const pad = Math.max(1, (Math.max(...offsets) - Math.min(...offsets)) * 0.25);
		const top = Math.max(...offsets) + pad;
		const bottom = Math.min(...offsets) - pad;
		const y = (db: number) => 6 + ((top - db) / (top - bottom)) * (HEIGHT - 12);
		const x = (hz: number) => ((hz - lowHz) / Math.max(1, highHz - lowHz)) * WIDTH;
		const path = Array.from({ length: 97 }, (_, i) => {
			const hz = lowHz + ((highHz - lowHz) * i) / 96;
			return `${i ? 'L' : 'M'}${x(hz).toFixed(1)},${y(offsetAt(points, hz)).toFixed(1)}`;
		}).join(' ');
		return { path, x, y };
	});

	const floorDbm = $derived(
		points.length > 0 && noiseFloorDbfs > -159 ? noiseFloorDbfs + offsetAt(points, (lowHz + highHz) / 2) : null
	);
</script>

<SettingsGroup title="S-meter calibration">
	{#each points as point, index (index)}
		<SettingsRow label="{(point.hz / 1e6).toFixed(3)} MHz" detail="S9 reads {dbfs(-73 - point.offsetDb)}" value="{signed(point.offsetDb, 1, true)} dB">
			{#snippet control()}
				<button
					type="button"
					class="pressable -mr-1 grid size-11 md:size-9 shrink-0 place-items-center rounded-full text-muted-foreground hover:bg-destructive/10 hover:text-destructive"
					aria-label="Remove the point at {(point.hz / 1e6).toFixed(3)} MHz"
					onclick={() => onChange(formatCalibration(points.filter((_, i) => i !== index)))}
				>
					<Minus size={18} />
				</button>
			{/snippet}
		</SettingsRow>
	{/each}
	{#if curve}
		<div class="px-4 py-3">
			<svg viewBox="0 0 {WIDTH} {HEIGHT}" preserveAspectRatio="none" class="h-16 w-full text-signal" role="img" aria-label="The correction across the band">
				<path d={curve.path} fill="none" stroke="currentColor" stroke-width="2" vector-effect="non-scaling-stroke" />
			</svg>
		</div>
	{/if}
	<SettingsRow label="Measure with a generator" tone="font-medium" onclick={openMeasure} />
	<SettingsRow label="Add a point by hand" onclick={openAdd} />
	{#snippet footer()}
		{#if points.length === 0}
			No calibration: the meter reads relative to full scale and says so.
		{:else if floorDbm !== null}
			The noise floor of {dbfs(noiseFloorDbfs)} works out at {signed(floorDbm, 0)} dBm. A quiet site sits near
			-125; a figure far from that means a sign or a digit is wrong.
		{:else}
			Between points the correction is interpolated; beyond them it holds.
		{/if}
	{/snippet}
</SettingsGroup>

<EditSheet
	bind:open={measuring}
	title="Measure with a generator"
	description="Feed a known level into the antenna input, say what and where, and the receiver reads itself."
	action="Read it"
	onSave={measure}
>
	<div class="grid grid-cols-2 gap-3">
		<label class="flex flex-col gap-1.5 text-sm font-medium">
			Level, dBm
			<Input type="number" step="0.1" bind:value={levelDbm} class="h-11 tabular" />
		</label>
		<label class="flex flex-col gap-1.5 text-sm font-medium">
			At, MHz
			<Input type="number" step="0.001" bind:value={atMhz} class="h-11 tabular" />
		</label>
	</div>
</EditSheet>

<EditSheet
	bind:open={adding}
	title="Add a point"
	description="A generator at −73 dBm that reads −50 dBFS is an offset of −23 dB."
	action="Add"
	onSave={add}
>
	<div class="grid grid-cols-2 gap-3">
		<label class="flex flex-col gap-1.5 text-sm font-medium">
			At, MHz
			<Input type="number" step="0.001" bind:value={atMhz} class="h-11 tabular" />
		</label>
		<label class="flex flex-col gap-1.5 text-sm font-medium">
			Offset, dB
			<Input type="number" step="0.1" bind:value={offsetDb} class="h-11 tabular" />
		</label>
	</div>
</EditSheet>
