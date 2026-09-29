<script lang="ts">
	import { Slider as SliderPrimitive } from 'bits-ui';
	import { cn } from '../../../lib/utils';

	let {
		value = $bindable(0),
		min = 0,
		max = 100,
		step = 1,
		disabled = false,
		class: className,
		label,
		onValueCommit
	}: {
		value?: number;
		min?: number;
		max?: number;
		step?: number | number[];
		disabled?: boolean;
		class?: string;
		label: string;
		onValueCommit?: (value: number) => void;
	} = $props();
</script>

<SliderPrimitive.Root
	type="single"
	bind:value
	{min}
	{max}
	{step}
	{disabled}
	{onValueCommit}
	aria-label={label}
	class={cn(
		'relative flex h-8 w-full touch-none items-center select-none data-disabled:opacity-50',
		className
	)}
>
	{#snippet children({ thumbs })}
		<span class="relative h-1.5 w-full grow overflow-hidden rounded-full bg-input">
			<SliderPrimitive.Range class="absolute h-full rounded-full bg-primary" />
		</span>
		{#each thumbs as index (index)}
			<SliderPrimitive.Thumb
				{index}
				aria-label={label}
				class="block size-5 cursor-grab rounded-full bg-white shadow-[0_1px_4px_oklch(0_0_0/0.3),0_0_0_0.5px_oklch(0_0_0/0.08)] transition-[scale,box-shadow] duration-150 outline-none hover:scale-110 focus-visible:ring-4 focus-visible:ring-ring/40 active:scale-115 active:cursor-grabbing"
			/>
		{/each}
	{/snippet}
</SliderPrimitive.Root>
