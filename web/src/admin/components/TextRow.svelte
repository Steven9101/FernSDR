<script lang="ts">
	import type { HTMLInputAttributes } from 'svelte/elements';

	/*
	A row whose value is typed straight into it, for short text such as a name or a callsign. The
	whole row is the field's label, so a tap anywhere on it starts editing, and the row is outlined
	while it has the cursor. What is typed is in the text colour and an empty field says so in the
	quieter one, so a value and its absence never look alike.
	*/
	let {
		label,
		value = $bindable(''),
		placeholder = 'Not set',
		...rest
	}: { label: string; value?: string; placeholder?: string } & Omit<HTMLInputAttributes, 'value' | 'placeholder'> =
		$props();

	const id = `row-${Math.random().toString(36).slice(2, 10)}`;
</script>

<label
	for={id}
	class="flex min-h-13 cursor-text items-center gap-3 px-4 py-2.5 transition-colors focus-within:bg-accent/40 has-[input:focus-visible]:ring-2 has-[input:focus-visible]:ring-ring has-[input:focus-visible]:ring-inset"
>
	<span class="shrink-0 text-[15px]">{label}</span>
	<input
		{id}
		bind:value
		{placeholder}
		autocomplete="off"
		spellcheck="false"
		maxlength="2000"
		class="min-w-0 flex-1 bg-transparent text-right text-[15px] text-foreground outline-none placeholder:text-muted-foreground"
		{...rest}
	/>
</label>
