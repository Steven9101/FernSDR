<script lang="ts">
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import { signed } from '../lib/format';
	import { correctedPpm, MAX_PPM, pendingCorrection, summarise } from '../lib/frequency';
	import { setKey } from '../lib/ini';
	import { typedNumber } from '../lib/number';
	import SettingsGroup from './SettingsGroup.svelte';
	import SettingsRow from './SettingsRow.svelte';
	import { Button } from './ui/button/index';
	import * as Dialog from './ui/dialog/index';
	import { Input } from './ui/input/index';

	/*
	Measuring how far off the band's frequencies are, against a station whose frequency is known,
	and writing the correction into the configuration. The band is asked where the carrier shows
	eight times over two seconds and the readings are averaged; the correction takes effect when the
	receiver next starts, because every listener and the waterfall are laid out on that axis.

	One reading cannot tell two causes apart, so both corrections are offered. A crystal that is off
	makes an error that grows with frequency, and `ppm` scales the axis to match. A tuner that lands
	beside the frequency it was given, as the R820T in many RTL-SDR dongles does, is off by the same
	amount across the band, and `frequency_offset` shifts it.
	*/
	let {
		open = $bindable(false),
		bandId,
		lowHz,
		highHz
	}: { open?: boolean; bandId: string; lowHz: number; highHz: number } = $props();

	const READINGS = 8;

	let known = $state('');
	let phase = $state<'asking' | 'measuring' | 'measured'>('asking');
	let taken = $state(0);
	let problem = $state('');
	let writing = $state(false);
	let result = $state<{
		truth: number;
		mean: number;
		spread: number;
		above_floor: number;
		ppm: number;
		suggested: number;
		offset: number;
		shifted: number;
		waiting: string | null;
	} | null>(null);

	$effect(() => {
		if (!open) return;
		phase = 'asking';
		problem = '';
		result = null;
	});

	const pause = (ms: number) => new Promise((resolve) => window.setTimeout(resolve, ms));

	async function measure(event: SubmitEvent) {
		event.preventDefault();
		problem = '';
		const mhz = typedNumber(known);
		if (mhz === null) {
			problem = 'Give the frequency of the station, in MHz.';
			return;
		}
		const truth = mhz * 1e6;
		if (truth < lowHz || truth > highHz) {
			problem = `That is not on this band, which runs from ${(lowHz / 1e6).toFixed(3)} to ${(highHz / 1e6).toFixed(3)} MHz.`;
			return;
		}
		phase = 'measuring';
		taken = 0;
		const readings: number[] = [];
		let level = -160;
		let floor = -160;
		let ppm = 0;
		let offset = 0;
		let config = '';
		try {
			for (let n = 0; n < READINGS; n++) {
				const reading = await api.carrier(bandId, truth, 2000);
				if (!reading.found || reading.hz === undefined) {
					throw new Error(`No carrier stands out within a kilohertz of ${mhz.toFixed(4)} MHz. Check the station is on the air.`);
				}
				readings.push(reading.hz);
				level = reading.level_dbfs ?? level;
				floor = reading.floor_dbfs;
				ppm = reading.ppm;
				offset = reading.frequency_offset;
				taken = n + 1;
				if (n + 1 < READINGS) await pause(280);
			}
			config = (await api.readConfig()).text;
		} catch (failure) {
			problem = (failure as ApiError).message;
			phase = 'asking';
			return;
		}
		const { mean, spread } = summarise(readings);
		result = {
			truth,
			mean,
			spread,
			above_floor: level - floor,
			ppm,
			suggested: Math.round(correctedPpm(truth, mean, ppm, offset) * 100) / 100,
			offset,
			shifted: Math.round((offset + truth - mean) * 10) / 10,
			waiting: pendingCorrection(config, bandId, { ppm, offset })
		};
		phase = 'measured';
	}

	async function write(key: 'ppm' | 'frequency_offset', value: string) {
		if (!result) return;
		writing = true;
		problem = '';
		try {
			const config = await api.readConfig();
			// Read again: somebody may have written a correction since the measurement.
			const waiting = pendingCorrection(config.text, bandId, { ppm: result.ppm, offset: result.offset });
			if (waiting) throw new Error(waiting);
			const next = setKey(config.text, `band:${bandId}`, key, value);
			if (next === null) throw new Error(`The configuration has no [band:${bandId}] section to write it into.`);
			await api.writeConfig(next);
			toast.success(`Saved ${key} = ${value}. The corrected frequencies apply when the receiver restarts.`, {
				duration: 9000
			});
			open = false;
		} catch (failure) {
			problem = (failure as ApiError).message;
		} finally {
			writing = false;
		}
	}
</script>

<Dialog.Root bind:open>
	<Dialog.Content class="max-h-[88dvh] gap-4 overflow-y-auto rounded-3xl p-3 pt-5 sm:max-w-md">
		<div class="flex flex-col gap-1 px-4 pr-12">
			<Dialog.Title class="text-lg font-semibold">Measure against a known carrier</Dialog.Title>
			<Dialog.Description class="text-sm leading-relaxed text-muted-foreground">
				Pick a station whose frequency is exact: a time signal such as WWV on 10 or 15 MHz, CHU on 7.850 MHz, an AM
				broadcaster, or a beacon.
			</Dialog.Description>
		</div>

		{#if phase !== 'measured'}
			<form class="flex flex-col gap-4 px-1" onsubmit={measure}>
				<label class="flex flex-col gap-1.5 px-3 text-sm font-medium">
					Its frequency, MHz
					<Input type="text" inputmode="decimal" bind:value={known} disabled={phase === 'measuring'} class="h-11 tabular" />
				</label>
				{#if problem}<p class="px-3 text-sm leading-relaxed text-destructive" role="alert">{problem}</p>{/if}
				<Button type="submit" size="lg" class="h-11 rounded-full text-[15px]" disabled={phase === 'measuring'}>
					{#if phase === 'measuring'}
						<LoaderCircle class="animate-spin" /> Reading {taken} of {READINGS}
					{:else}
						Measure
					{/if}
				</Button>
			</form>
		{:else if result}
			{@const off = result.mean - result.truth}
			<SettingsGroup
				footer={result.above_floor < 15
					? 'The carrier is weak against the noise here, so the reading may be off. A stronger station gives a better one.'
					: `Eight readings over two seconds, varying by ${result.spread.toFixed(1)} Hz.`}
			>
				<SettingsRow label="Shows at" value="{(result.mean / 1e6).toFixed(6)} MHz" />
				<SettingsRow label="Off by" value="{signed(off, 1, true)} Hz" />
				<SettingsRow label="Above the noise" value="{Math.round(result.above_floor)} dB" />
			</SettingsGroup>
			{#if result.waiting}
				<!-- First, even when the reading is right: it is right for an axis the next start replaces. -->
				<p class="px-4 text-[15px] leading-relaxed text-warning" role="alert">{result.waiting}</p>
			{:else if Math.abs(off) < Math.max(1, 3 * result.spread)}
				<p class="px-4 text-[15px] leading-relaxed">
					It is where it should be, to within what a reading can tell. There is nothing to correct.
				</p>
			{:else}
			<SettingsGroup
				title="Correct it as"
				footer="A crystal error grows with frequency; a tuner that lands beside its frequency is off by the same amount everywhere. A second station far from this one tells them apart: the same hertz means a fixed offset."
			>
				{#if Math.abs(result.suggested) <= MAX_PPM}
					<SettingsRow
						label="A crystal error"
						detail="ppm = {result.suggested.toFixed(2)}, now {result.ppm.toFixed(2)}"
						tone="font-medium"
						onclick={() => !writing && write('ppm', result!.suggested.toFixed(2))}
					/>
				{:else}
					<SettingsRow
						label="Not a crystal error"
						detail="It would take {result.suggested.toFixed(0)} ppm, and a crystal is never off by more than {MAX_PPM}. Check the frequency you gave."
						tone="text-muted-foreground"
					/>
				{/if}
				<SettingsRow
					label="A fixed offset"
					detail="frequency_offset = {result.shifted.toFixed(1)} Hz, now {result.offset.toFixed(1)}"
					tone="font-medium"
					onclick={() => !writing && write('frequency_offset', result!.shifted.toFixed(1))}
				/>
			</SettingsGroup>
			{/if}
			{#if problem}<p class="px-4 text-sm leading-relaxed text-destructive" role="alert">{problem}</p>{/if}
			{#if writing}
				<p class="flex items-center gap-2 px-4 text-sm text-muted-foreground" role="status">
					<LoaderCircle size={16} class="animate-spin" /> Writing the configuration
				</p>
			{/if}
			<Button variant="ghost" size="lg" class="h-11 rounded-full text-[15px]" onclick={() => (phase = 'asking')}>Measure again</Button>
		{/if}
	</Dialog.Content>
</Dialog.Root>
