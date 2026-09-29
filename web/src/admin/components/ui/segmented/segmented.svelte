<script lang="ts" generics="T extends string">
	import { cn } from '../../../lib/utils';

	let {
		options,
		value,
		onChange,
		label,
		class: className
	}: {
		options: { value: T; label: string }[];
		value: T;
		onChange: (value: T) => void;
		label: string;
		class?: string;
	} = $props();

	const index = $derived(
		Math.max(
			0,
			options.findIndex((option) => option.value === value)
		)
	);

	function onKeydown(event: KeyboardEvent) {
		const step = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : 0;

		if (!step) return;
		event.preventDefault();
		const next = options[(index + step + options.length) % options.length];

		onChange(next.value);
		const group = event.currentTarget as HTMLElement;

		queueMicrotask(() => group.querySelector<HTMLElement>('[aria-checked="true"]')?.focus());
	}
</script>

<!-- One pill that slides between equal segments rather than each segment painting its own
background, so the eye follows the change. A radio group underneath, for keyboards and readers. -->
<div
	role="radiogroup"
	aria-label={label}
	tabindex="-1"
	onkeydown={onKeydown}
	class={cn('relative grid rounded-xl bg-muted p-1', className)}
	style="grid-template-columns: repeat({options.length}, minmax(0, 1fr))"
>
	<span
		aria-hidden="true"
		class="absolute inset-y-1 left-1 rounded-[0.6rem] bg-background shadow-[0_1px_3px_oklch(0_0_0/0.14)] transition-transform duration-[560ms] ease-(--ease-page) dark:bg-accent"
		style="width: calc((100% - 0.5rem) / {options.length}); transform: translateX({index * 100}%)"
	></span>
	{#each options as option (option.value)}
		{@const active = option.value === value}
		<button
			type="button"
			role="radio"
			aria-checked={active}
			tabindex={active ? 0 : -1}
			onclick={() => onChange(option.value)}
			class="pressable relative z-[1] h-9 truncate rounded-[0.6rem] px-2 text-sm font-medium transition-colors duration-300 {active
				? 'text-foreground'
				: 'text-muted-foreground hover:text-foreground'}"
		>
			{option.label}
		</button>
	{/each}
</div>
