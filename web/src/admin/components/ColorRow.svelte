<script lang="ts">
	import Undo2 from '@lucide/svelte/icons/undo-2';

	/*
	One colour of the theme: a swatch that opens the system's colour picker, and what it is set to.
	Unset means the receiver's own colour, shown in the swatch as what listeners get, and a set one
	can go back to that with one tap.
	*/
	let {
		label,
		hint,
		value,
		fallback,
		onChange
	}: {
		label: string;
		hint: string;
		value?: string;
		fallback: string;
		onChange: (value: string | null) => void;
	} = $props();

	const id = `color-${Math.random().toString(36).slice(2, 10)}`;

	/** The picker only takes #rrggbb; a shorter or translucent value is shown as its nearest. */
	function pickerValue(color: string): string {
		if (/^#[0-9a-fA-F]{3}$/.test(color)) return `#${[...color.slice(1)].map((c) => c + c).join('')}`.toLowerCase();
		if (/^#[0-9a-fA-F]{4}$/.test(color)) return `#${[...color.slice(1, 4)].map((c) => c + c).join('')}`.toLowerCase();
		return color.slice(0, 7).toLowerCase();
	}
</script>

<!-- The picker itself is invisible over the swatch, so the whole row shows where the keyboard is. -->
<div
	class="flex min-h-13 items-center gap-1 py-1.5 pr-2 pl-4 transition-colors focus-within:bg-accent/40 has-[input:focus-visible]:ring-2 has-[input:focus-visible]:ring-ring has-[input:focus-visible]:ring-inset"
>
	<label for={id} class="flex min-w-0 flex-1 cursor-pointer items-center gap-3 py-1">
		<span
			class="relative size-8 shrink-0 rounded-full shadow-[inset_0_0_0_1px_oklch(0_0_0/0.12)] ring-1 ring-foreground/25"
			style="background: {value ?? fallback}"
		>
			<input
				{id}
				type="color"
				class="absolute inset-0 size-full cursor-pointer rounded-full opacity-0 outline-none"
				value={pickerValue(value ?? fallback)}
				oninput={(event) => onChange(event.currentTarget.value)}
			/>
		</span>
		<span class="flex min-w-0 flex-1 flex-col">
			<span class="text-[15px]">{label}</span>
			<span class="truncate text-[13px] text-muted-foreground">{hint}</span>
		</span>
		<span class="shrink-0 text-[13px] text-muted-foreground tabular">{value ? value.toUpperCase() : 'Built in'}</span>
	</label>
	{#if value}
		<button
			type="button"
			class="pressable grid size-11 md:size-9 shrink-0 place-items-center rounded-full text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
			aria-label="Back to the receiver's own {label.toLowerCase()} colour"
			title="Back to the receiver's own colour"
			onclick={() => onChange(null)}
		>
			<Undo2 size={16} />
		</button>
	{:else}
		<span class="size-11 shrink-0 md:size-9" aria-hidden="true"></span>
	{/if}
</div>
