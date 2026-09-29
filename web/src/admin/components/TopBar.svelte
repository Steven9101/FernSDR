<script lang="ts">
	import { live } from '../lib/live.svelte';
	import Health from './Health.svelte';
</script>

<!-- Phones only: the station, and whether it is well, above whatever page is open. -->
<header
	class="vt-topbar sticky top-0 z-30 bg-background/75 backdrop-blur-xl backdrop-saturate-150 supports-[backdrop-filter]:bg-background/60 md:hidden"
>
	<div class="flex h-14 items-center justify-between gap-3 px-(--gutter)">
		<a href="#/overview" class="truncate text-[1.375rem] font-bold tracking-[-0.04em]">
			{live.state?.site ?? 'FernSDR'}
		</a>
		<Health />
	</div>
</header>
