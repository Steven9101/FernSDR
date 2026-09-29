<script lang="ts">
	// Anchored rather than centered: phones get a sheet pinned to the bottom, larger screens a
	// dialog pinned near the top. A centered dialog re-centers whenever its content grows or
	// shrinks (a warning appearing, a form opening), which makes the whole thing jump.
	import { Dialog as DialogPrimitive } from 'bits-ui';
	import XIcon from '@lucide/svelte/icons/x';
	import { Button } from '../button/index';
	import { cn, type WithoutChildrenOrChild } from '../../../lib/utils';
	import * as Dialog from './index.js';
	import DialogPortal from './dialog-portal.svelte';
	import type { Snippet } from 'svelte';
	import type { ComponentProps } from 'svelte';

	let {
		ref = $bindable(null),
		class: className,
		portalProps,
		children,
		showCloseButton = true,
		...restProps
	}: WithoutChildrenOrChild<DialogPrimitive.ContentProps> & {
		portalProps?: WithoutChildrenOrChild<ComponentProps<typeof DialogPortal>>;
		children: Snippet;
		showCloseButton?: boolean;
	} = $props();
</script>

<DialogPortal {...portalProps}>
	<Dialog.Overlay />
	<DialogPrimitive.Content
		bind:ref
		data-slot="dialog-content"
		class={cn(
			'fixed bottom-[max(0.75rem,env(safe-area-inset-bottom))] left-1/2 z-50 grid w-full max-w-[calc(100%_-_2rem)] -translate-x-1/2 grid-cols-[minmax(0,1fr)] gap-4 rounded-xl bg-popover p-4 text-sm text-popover-foreground ring-1 ring-foreground/10 duration-100 outline-none sm:top-[12dvh] sm:bottom-auto sm:max-w-sm data-open:animate-in data-open:fade-in-0 data-open:slide-in-from-bottom-4 sm:data-open:slide-in-from-bottom-0 sm:data-open:zoom-in-95 data-closed:animate-out data-closed:fade-out-0 data-closed:slide-out-to-bottom-4 sm:data-closed:slide-out-to-bottom-0 sm:data-closed:zoom-out-95',
			className
		)}
		{...restProps}
	>
		{@render children?.()}
		{#if showCloseButton}
			<DialogPrimitive.Close data-slot="dialog-close">
				{#snippet child({ props })}
					<!-- A thumb-sized target: 44 px on a phone, where these sheets are closed most. -->
					<Button variant="ghost" class="absolute top-2 right-2 size-11 rounded-full sm:size-9" size="icon" {...props}>
						<XIcon />
						<span class="sr-only">Close</span>
					</Button>
				{/snippet}
			</DialogPrimitive.Close>
		{/if}
	</DialogPrimitive.Content>
</DialogPortal>
