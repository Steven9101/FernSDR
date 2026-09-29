<script lang="ts">
	import { onMount } from 'svelte';
	import { api, canSign } from './api';
	import BottomNav from './components/BottomNav.svelte';
	import SideNav from './components/SideNav.svelte';
	import TopBar from './components/TopBar.svelte';
	import { Toaster } from './components/ui/sonner/index';
	import { live } from './lib/live.svelte';
	import { router } from './lib/router.svelte';
	import Appearance from './pages/Appearance.svelte';
	import Bands from './pages/Bands.svelte';
	import Config from './pages/Config.svelte';
	import Log from './pages/Log.svelte';
	import Modules from './pages/Modules.svelte';
	import Decoders from './pages/Decoders.svelte';
	import Overview from './pages/Overview.svelte';
	import SignIn from './pages/SignIn.svelte';
	import Station from './pages/Station.svelte';
	import Updates from './pages/Updates.svelte';
	import Widgets from './pages/Widgets.svelte';
	import Setup from './pages/Setup.svelte';
	import { looksNew, setupDone, setupInProgress } from './lib/setup';
	import UpdateDialog from './components/UpdateDialog.svelte';
	import { updateToOffer } from './lib/update-offer';
	import type { UpdateView } from './api';

	let session = $state<'checking' | 'signed-out' | 'signed-in'>('checking');
	let exposed = $state(false);

	onMount(() => {
		api
			.session()
			.then((answer) => {
				exposed = answer.exposed === true;
				session = answer.authorised && canSign() ? 'signed-in' : 'signed-out';
			})
			.catch(() => (session = 'signed-out'));
	});

	$effect(() => {
		if (session !== 'signed-in') return;
		live.unauthorised = false;
		live.start();
		return () => live.stop();
	});

	// After signing in: a receiver as the installer left it, or a setup the
	// last restart broke off, opens the setup flow, unless the operator asked
	// for another page by its address.
	$effect(() => {
		if (session !== 'signed-in' || router.route.page !== 'overview' || setupDone()) return;
		if (setupInProgress()) {
			router.go({ page: 'setup' });
			return;
		}
		api
			.readBands()
			.then((answer) => {
				const bands = answer.bands as { fixed: { source: string } }[];
				if (looksNew(bands.map((band) => band.fixed.source)) && router.route.page === 'overview') router.go({ page: 'setup' });
			})
			.catch(() => {});
	});

	// A newer release, offered once after signing in, a few seconds on so that it does not cover
	// the page as it appears, and not while the setup runs, which has its own questions.
	let offered = $state<UpdateView | null>(null);
	let offerOpen = $state(false);
	$effect(() => {
		if (session !== 'signed-in') return;
		let cancelled = false;
		const timer = window.setTimeout(() => {
			updateToOffer()
				.then((view) => {
					if (cancelled || !view || router.route.page === 'setup' || router.route.page === 'updates') return;
					offered = view;
					offerOpen = true;
				})
				.catch(() => {});
		}, 4000);
		return () => {
			cancelled = true;
			window.clearTimeout(timer);
		};
	});

	// A session that ran out while the panel was open lands back on sign-in, not on an error.
	$effect(() => {
		if (live.unauthorised) session = 'signed-out';
	});

	async function signOut() {
		try {
			await api.logout();
		} finally {
			live.stop();
			live.state = null;
			session = 'signed-out';
		}
	}
</script>

{#if exposed}
	<!-- On every page, the sign-in first: this is when a changed page would catch the password. -->
	<p class="sticky top-0 z-40 bg-destructive px-(--gutter) py-2 text-center text-[13px] font-medium text-white" role="alert">
		Unencrypted connection. Anyone between you and the receiver can read this panel and change it to catch your
		password. Use HTTPS, or remove plain_http_anywhere from [admin].
	</p>
{/if}

{#if session === 'checking'}
	<div class="grid min-h-dvh place-items-center" aria-busy="true">
		<span class="shimmer size-10 rounded-full bg-muted"></span>
	</div>
{:else if session === 'signed-out'}
	<SignIn onSignedIn={() => (session = 'signed-in')} />
{:else}
	<div class="flex min-h-dvh">
		<SideNav onSignOut={signOut} />
		<div class="flex min-w-0 flex-1 flex-col">
			<TopBar />
			<main
				class="mx-auto w-full max-w-6xl px-(--gutter) pb-36 md:pb-20"
				data-route={router.route.id ? `${router.route.page}/${router.route.id}` : router.route.page}
			>
				{#if router.route.page === 'overview'}
					<Overview />
				{:else if router.route.page === 'bands'}
					<Bands id={router.route.id} />
				{:else if router.route.page === 'modules'}
					<Modules />
				{:else if router.route.page === 'decoders'}
					<Decoders />
				{:else if router.route.page === 'log'}
					<Log />
				{:else if router.route.page === 'station'}
					<Station />
				{:else if router.route.page === 'appearance'}
					<Appearance />
				{:else if router.route.page === 'widgets'}
					<Widgets />
				{:else if router.route.page === 'updates'}
					<Updates />
				{:else if router.route.page === 'setup'}
					<Setup />
				{:else}
					<Config />
				{/if}
			</main>
		</div>
	</div>
	<BottomNav onSignOut={signOut} />
	{#if offered}<UpdateDialog view={offered} bind:open={offerOpen} />{/if}
{/if}

<Toaster position="top-center" />
