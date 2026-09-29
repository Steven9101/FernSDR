<script lang="ts">
	import { Slider } from './ui/slider/index';

	/*
	A value found by looking rather than by knowing the number: dragged while the preview beside it
	follows. Kept inside its row, so it is as wide as the settings and no wider.
	*/
	let {
		label,
		value = $bindable(0),
		min = 0,
		max = 100,
		step = 1,
		format = (value: number) => String(value)
	}: {
		label: string;
		value?: number;
		min?: number;
		max?: number;
		step?: number;
		format?: (value: number) => string;
	} = $props();
</script>

<div class="flex flex-col gap-1 px-4 pt-3 pb-2">
	<div class="flex items-baseline justify-between gap-3">
		<span class="text-[15px]">{label}</span>
		<span class="text-[15px] text-muted-foreground tabular">{format(value)}</span>
	</div>
	<Slider {label} {min} {max} {step} bind:value />
</div>
