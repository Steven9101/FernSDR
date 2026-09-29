<script lang="ts">
	import { AlertDialog } from 'bits-ui';
	import { buttonVariants } from '../button/index';
	import { cn } from '../../../lib/utils';

	/*
	A question before something that cannot be taken back, worded as what will happen rather than
	"are you sure". Focus starts on the side that does nothing, so an Enter already on its way does
	no harm; the button that acts carries the warning colour when it destroys something.
	*/
	let {
		open = $bindable(false),
		title,
		description,
		action,
		destructive = false,
		onConfirm
	}: {
		open?: boolean;
		title: string;
		description: string;
		action: string;
		destructive?: boolean;
		onConfirm: () => void;
	} = $props();
</script>

<AlertDialog.Root bind:open>
	<AlertDialog.Portal>
		<AlertDialog.Overlay
			class="fixed inset-0 z-50 bg-black/30 supports-backdrop-filter:backdrop-blur-xs data-open:animate-in data-open:fade-in-0 data-closed:animate-out data-closed:fade-out-0"
		/>
		<AlertDialog.Content
			class="fixed bottom-[max(0.75rem,env(safe-area-inset-bottom))] left-1/2 z-50 grid w-full max-w-[calc(100%-2rem)] -translate-x-1/2 gap-4 rounded-3xl bg-popover p-6 text-popover-foreground ring-1 ring-foreground/10 outline-none sm:top-[14dvh] sm:bottom-auto sm:max-w-md data-open:animate-in data-open:fade-in-0 data-open:slide-in-from-bottom-4 sm:data-open:slide-in-from-bottom-0 sm:data-open:zoom-in-95 data-closed:animate-out data-closed:fade-out-0 data-closed:zoom-out-95"
		>
			<div class="flex flex-col gap-2">
				<AlertDialog.Title class="text-lg font-semibold">{title}</AlertDialog.Title>
				<AlertDialog.Description class="text-sm leading-relaxed text-muted-foreground">
					{description}
				</AlertDialog.Description>
			</div>
			<div class="flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
				<AlertDialog.Cancel class={cn(buttonVariants({ variant: 'secondary', size: 'lg' }), 'rounded-full px-5')}>
					Cancel
				</AlertDialog.Cancel>
				<AlertDialog.Action
					onclick={() => {
						open = false;
						onConfirm();
					}}
					class={cn(
						buttonVariants({ variant: destructive ? 'destructive' : 'default', size: 'lg' }),
						'rounded-full px-5'
					)}
				>
					{action}
				</AlertDialog.Action>
			</div>
		</AlertDialog.Content>
	</AlertDialog.Portal>
</AlertDialog.Root>
