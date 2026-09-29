<script lang="ts">
	import LoaderCircle from '@lucide/svelte/icons/loader-circle';
	import { api, ApiError } from '../api';
	import { Button } from '../components/ui/button/index';
	import { Input } from '../components/ui/input/index';

	let { onSignedIn }: { onSignedIn: () => void } = $props();

	let password = $state('');
	let phase = $state<'idle' | 'asking' | 'deriving'>('idle');
	let error = $state('');
	let field: HTMLInputElement | null = $state(null);

	$effect(() => {
		field?.focus();
	});

	async function submit(event: SubmitEvent) {
		event.preventDefault();
		if (phase !== 'idle' || !password) return;
		phase = 'asking';
		error = '';
		try {
			await api.login(password, async () => {
				// Deriving the key takes a second or more on the main thread. Say so, and give the
				// browser a frame to draw it before starting, or the button seems to do nothing.
				phase = 'deriving';
				await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
			});
			onSignedIn();
		} catch (problem) {
			const failure = problem as ApiError;
			// A receiver with no password set has no panel; "not accepted" there would send the
			// operator looking for a password that does not exist.
			error =
				failure.status === 404
					? failure.message
					: failure.retryAfter > 0
						? `Too many attempts. Try again in ${failure.retryAfter} seconds.`
						: 'That password was not accepted.';
			password = '';
			field?.focus();
		} finally {
			phase = 'idle';
		}
	}
</script>

<div class="grid min-h-dvh place-items-center px-5">
	<form class="flex w-full max-w-sm flex-col items-center gap-6 text-center" onsubmit={submit}>
		<div class="flex flex-col gap-2">
			<p class="text-[2rem] font-bold tracking-[-0.04em]">FernSDR</p>
			<h1 class="text-2xl">Sign in to the admin panel</h1>
			<p class="text-[15px] text-muted-foreground">
				This receiver's controls. The session ends by itself, and the password never crosses the
				network.
			</p>
		</div>
		<div class="flex w-full flex-col gap-3">
			<Input
				bind:ref={field}
				bind:value={password}
				type="password"
				autocomplete="current-password"
				placeholder="Password"
				aria-label="Admin password"
				aria-invalid={error ? true : undefined}
				class="h-12 rounded-2xl px-4 text-base"
			/>
			{#if error}
				<p class="text-sm text-destructive" role="alert">{error}</p>
			{/if}
			<Button type="submit" size="lg" class="h-12 rounded-full text-base" disabled={phase !== 'idle' || !password}>
				{#if phase !== 'idle'}<LoaderCircle class="animate-spin" />{/if}
				{phase === 'deriving' ? 'Checking' : 'Sign in'}
			</Button>
		</div>
	</form>
</div>
