<script lang="ts">
	import { fly } from '../lib/motion';
	import CircleAlert from '@lucide/svelte/icons/circle-alert';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';
	import { api, ApiError } from '../api';
	import Columns from '../components/Columns.svelte';
	import IniEditor from '../components/IniEditor.svelte';
	import PageHeader from '../components/PageHeader.svelte';
	import SaveBar from '../components/SaveBar.svelte';
	import SettingsGroup from '../components/SettingsGroup.svelte';
	import type { Section } from '../config-schema';

	/*
	The file the receiver starts from, for what the other pages do not cover: adding a band,
	changing its input, the server's socket. The receiver checks a file before writing it, by
	building a throwaway receiver from it, and refuses one that would not start, with the reason;
	the file is replaced in one step, so a failed save cannot leave half a configuration behind.
	*/
	type Change = { band: string; key: string; running: string; configured: string; band_restart?: boolean };

	let text = $state('');
	let saved = $state('');
	let path = $state('');
	let version = $state('');
	let loaded = $state(false);
	let loadError = $state('');
	let refusal = $state('');
	let busy = $state(false);
	let changes = $state<Change[]>([]);
	let section = $state<Section | null>(null);

	onMount(() => {
		api
			.readConfig()
			.then((config) => {
				text = config.text;
				saved = config.text;
				path = config.path;
				version = config.version;
				loaded = true;
			})
			.catch((problem) => (loadError = (problem as Error).message));
	});

	const dirty = $derived(loaded && text !== saved);

	async function save() {
		busy = true;
		refusal = '';
		// What was sent is what is saved; anything typed while it was on its way stays unsaved.
		const sending = text;
		try {
			const result = await api.writeConfig(sending);
			saved = sending;
			changes = result.changes ?? [];
			// Precise about what took effect: "restart to apply" as a blanket answer sends an
			// operator to restart a receiver full of listeners because they edited the notice.
			const needsReceiver = new Set(changes.filter((change) => !change.band_restart).map((change) => change.band));
			const byBand = result.bands_changed.filter((band) => !needsReceiver.has(band));
			const byReceiver = result.bands_changed.filter((band) => needsReceiver.has(band));
			const named = (bands: string[]) => `${bands.length === 1 ? 'band' : 'bands'} ${bands.join(', ')}`;
			if (result.bands_changed.length === 0) toast.success('Saved and applied. Nothing needs a restart.');
			else {
				toast.success(
					[
						'Saved. Station settings applied.',
						byBand.length ? `Restart ${named(byBand)} under Bands to apply the rest.` : '',
						byReceiver.length ? `Changes to ${named(byReceiver)} apply when the receiver restarts.` : ''
					]
						.filter(Boolean)
						.join(' '),
					{ duration: 9000 }
				);
			}
		} catch (problem) {
			// The receiver's own reason: the file was built into a throwaway receiver and that failed.
			refusal = (problem as ApiError).message;
		} finally {
			busy = false;
		}
	}
</script>

<PageHeader title="Configuration" description="The file the receiver starts from. A file that would not start it is refused, with the reason." />

{#if loadError}
	<p class="text-destructive" role="alert">{loadError}</p>
{:else if !loaded}
	<div class="shimmer h-[50dvh] max-w-2xl rounded-2xl bg-card md:h-[62dvh]" aria-busy="true"></div>
{:else}
	<Columns asideLabel="About the section at the cursor">
		{#snippet main()}
			<div class="flex flex-col gap-2">
				{#if refusal}
					<p
						class="flex gap-2.5 rounded-2xl bg-destructive/10 px-4 py-3 text-[14px] leading-relaxed text-destructive"
						role="alert"
						transition:fly={{ y: -6, duration: 220 }}
					>
						<CircleAlert size={18} class="mt-0.5 shrink-0" />
						<span>Not saved. {refusal}</span>
					</p>
				{/if}
				<IniEditor label="Configuration file" bind:value={text} bind:section />
				<p class="px-1 text-[13px] break-all text-muted-foreground">{path}, read by FernSDR {version}</p>
			</div>
		{/snippet}

		{#snippet aside()}
			{#if changes.length > 0}
				<SettingsGroup title="What a restart applies" footer="The receiver keeps running the old values until then.">
					{#each changes as change (`${change.band}:${change.key}`)}
						<div class="flex flex-col gap-0.5 px-4 py-3">
							<span class="text-[15px]">{change.band} <span class="text-[13px] text-muted-foreground">{change.key}</span></span>
							<span class="text-[13px] break-words text-muted-foreground">
								{change.running || 'not set'} becomes {change.configured || 'removed'}, by restarting {change.band_restart ? 'the band' : 'the receiver'}
							</span>
						</div>
					{/each}
				</SettingsGroup>
			{/if}

			<SettingsGroup title={section ? `[${section.name}${section.prefixed ? ':...' : ''}]` : 'No section yet'} footer={section?.detail}>
				{#if section}
					{#each section.settings as setting (setting.key)}
						<div class="flex flex-col gap-0.5 px-4 py-2.5">
							<span class="text-[13px]">{setting.key}</span>
							<span class="text-[13px] leading-snug text-muted-foreground">{setting.detail}</span>
						</div>
					{/each}
				{:else}
					<p class="px-4 py-3 text-[14px] leading-relaxed text-muted-foreground">
						Put the cursor in a section to see what it takes. Typing the start of a setting offers the ones that fit.
					</p>
				{/if}
			</SettingsGroup>
		{/snippet}
	</Columns>

	<SaveBar visible={dirty} message="The file has unsaved changes" {busy} onSave={save} onDiscard={() => ((text = saved), (refusal = ''))} />
{/if}
