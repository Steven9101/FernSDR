<script lang="ts" module>
  import { lazy } from './components/lazy';

  // Loaded once per page, whatever mounts them.
  const loadWidgetPanel = lazy(() => import('./components/WidgetPanel.svelte'));
  const loadKeyboardHelp = lazy(() => import('./components/KeyboardHelp.svelte'));
  const loadEditLayoutBar = lazy(() => import('./components/EditLayoutBar.svelte'));
</script>

<script lang="ts">
  /**
   * Application shell.
   *
   * One tree for both layouts, with CSS deciding where things go: a phone gets
   * the waterfall plus a draggable sheet of controls, a desktop gets the same
   * controls in a sidebar. Keeping it one tree means the mobile layout cannot
   * quietly rot into a second-class version of the desktop one, which is how
   * most web receivers end up painful on a phone.
   */
  import { onMount } from 'svelte';
  import { MediaQuery } from 'svelte/reactivity';
  import Check from '@lucide/svelte/icons/check';
  import Keyboard from '@lucide/svelte/icons/keyboard';
  import Link2 from '@lucide/svelte/icons/link-2';
  import Tooltip from './components/Tooltip.svelte';
  import SpectrumDisplay from './components/SpectrumDisplay.svelte';
  import FrequencyDisplay from './components/FrequencyDisplay.svelte';
  import BandSelector from './components/BandSelector.svelte';
  import SMeter from './components/SMeter.svelte';
  import VolumeControl from './components/VolumeControl.svelte';
  import MorphPanel from './components/MorphPanel.svelte';
  import ReceivePanel from './components/ReceivePanel.svelte';
  import DisplayPanel from './components/DisplayPanel.svelte';
  import ConnectionPanel from './components/ConnectionPanel.svelte';
  import HistoryPanel from './components/HistoryPanel.svelte';
  import DecodesPanel from './components/DecodesPanel.svelte';
  import BottomSheet from './components/BottomSheet.svelte';
  import ControlTabs from './components/ControlTabs.svelte';
  import AudioGate from './components/AudioGate.svelte';
  import Banner from './components/Banner.svelte';
  import StatusBar from './components/StatusBar.svelte';
  import { audioState, bands, controller, currentBand, decoders, dsp, frequencyEntry, muted, operatorTheme, showNote, site, tuning, viewport, volume } from './state/store';
  import { applySharedTuning, loadPreferences, readUrlTuning, shareUrl, startPersistence } from './state/persist';
  import { copyText } from './util/clipboard';
  import { watchSystemTheme } from './state/store';
  import { loadTone, tone, toneGroup } from './state/tone';
  import { loadBandPlan } from './state/bandplan';
  import { roomOrLater } from './state/link-room';
  import { editingLayout, layout, loadLayout, setLayout } from './state/layout';
  import Editable from './components/Editable.svelte';
  import { signalForCarrier } from './util/cw';

  const MODE_KEYS = ['usb', 'lsb', 'cw', 'cwl', 'am', 'sam', 'nfm', 'dsb', 'wfm'];

  const wide = new MediaQuery('(min-width: 1024px)');
  const isWide = $derived(wide.current);
  let tab = $state('receive');
  let showHelp = $state(false);
  let copied = $state(false);

  // --- startup ---
  onMount(() => {
    loadPreferences();
    loadTone();
    loadLayout();
    const stopWatchingTheme = watchSystemTheme();
    const stopPersisting = startPersistence();
    controller.connect();

    const stopShared = applySharedTuning(readUrlTuning());

    return () => {
      stopPersisting();
      stopShared();
      stopWatchingTheme();
      controller.disconnect();
    };
  });

  // The band plan the station's operator chose, loaded once the link has
  // room for it (at most twenty seconds on a slow one, where it would delay
  // the sound) and again whenever the station details change.
  // Keyed on what decides the plan: the band list is replaced whenever its
  // listener counts change, which must not reload anything.
  const planKey = $derived(site.value
    ? JSON.stringify([site.value.band_plan, site.value.grid, Math.max(0, ...bands.value.map((band) => band.high)) > 30e6])
    : null);
  $effect(() => {
    if (!planKey) return;
    const [choice, grid, wideband] = JSON.parse(planKey) as [string | undefined, string | undefined, boolean];
    void roomOrLater(20000).then(() => loadBandPlan(choice, grid, wideband ? 31e6 : 0));
  });

  // The tone follows the mode: AM's bass boost stays with AM.
  $effect(() => {
    const setting = tone.value[toneGroup(tuning.value.mode)];
    controller.player.setTone(setting.bass, setting.treble);
  });

  // --- keyboard ---
  onMount(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      // Never steal keys from a field the user is typing in.
      if (event.defaultPrevented || (target && (target.closest('input, textarea, select, button, [role="dialog"]') || target.isContentEditable))) {
        return;
      }

      const tune = tuning.value;
      // Broadcast stations sit on a 50 kHz raster (100 or 200 kHz apart), so
      // WFM steps from one raster point to the next.
      const wfm = tune.mode === 'wfm';
      const step = wfm
        ? event.ctrlKey || event.metaKey ? 1_000_000 : event.shiftKey ? 100_000 : 50_000
        : event.ctrlKey || event.metaKey ? 1000 : event.shiftKey ? 100 : 10;
      const stepTo = (direction: number) => {
        const next = signalForCarrier(tune.freq, tune.mode, tune.cwPitch) + direction * step;
        controller.tune(wfm ? Math.round(next / 50_000) * 50_000 : next);
      };

      switch (event.key) {
        case 'ArrowLeft':
          event.preventDefault();
          stepTo(-1);
          return;
        case 'ArrowRight':
          event.preventDefault();
          stepTo(1);
          return;
        case 'ArrowUp':
          event.preventDefault();
          controller.setVolume(Math.min(1, volume.value + 0.05));
          return;
        case 'ArrowDown':
          event.preventDefault();
          controller.setVolume(Math.max(0, volume.value - 0.05));
          return;
        case ' ':
          event.preventDefault();
          if (audioState.value !== 'running') void controller.startAudio();
          else controller.setMuted(!muted.value);
          return;
        case 'z':
        case 'Z': {
          const view = viewport.value;
          const span = (view.highHz - view.lowHz) * 1.6;
          const centre = (view.lowHz + view.highHz) / 2;
          controller.setViewport(centre - span / 2, centre + span / 2);
          return;
        }
        case 'x':
        case 'X': {
          const view = viewport.value;
          const span = (view.highHz - view.lowHz) / 1.6;
          const centre = (view.lowHz + view.highHz) / 2;
          controller.setViewport(centre - span / 2, centre + span / 2);
          return;
        }
        case '[': {
          const width = tune.high - tune.low;
          const shrink = Math.max(50, width * 0.8);
          const centre = (tune.low + tune.high) / 2;
          controller.setPassband(centre - shrink / 2, centre + shrink / 2);
          return;
        }
        case ']': {
          const width = tune.high - tune.low;
          const grow = width * 1.25;
          const centre = (tune.low + tune.high) / 2;
          controller.setPassband(centre - grow / 2, centre + grow / 2);
          return;
        }
        case 'm':
        case 'M':
          controller.setMuted(!muted.value);
          return;
        case 'n':
        case 'N':
          controller.setDsp({ nr: dsp.value.nr > 0 ? 0 : 0.6 });
          return;
        case 'f':
        case 'F':
          // Not Ctrl+F or Cmd+F, which are the browser's find.
          if (event.ctrlKey || event.metaKey || event.altKey) return;
          event.preventDefault();
          frequencyEntry.value += 1;
          return;
        case '?':
          showHelp = !showHelp;
          return;
        case 'l':
        case 'L':
          if (event.ctrlKey || event.metaKey || event.altKey) return;
          editingLayout.value = !editingLayout.value;
          return;
        case 'Escape':
          showHelp = false;
          return;
        default:
          break;
      }

      const digit = Number(event.key);
      if (Number.isInteger(digit) && digit >= 1 && digit <= MODE_KEYS.length) {
        const mode = MODE_KEYS[digit - 1];
        // Broadcast FM only where the band carries it.
        if (mode !== 'wfm' || currentBand.value?.wfm) controller.setMode(mode);
      }
    };

    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  });

  const information = $derived(site.value);

  // A horizontal strip is fine for four bands and unusable for twenty: the
  // ones off the right-hand edge are invisible, and there is nothing to say
  // they exist. Past six, the phone gets the same list the sidebar has, as a
  // tab of its own.
  const bandsNeedTheirOwnTab = $derived(bands.value.length > 6);
  // A tab that always says "nothing here" is worse than no tab, so the
  // archive only appears on receivers that keep one.
  const hasHistory = $derived((currentBand.value?.history ?? 'off') !== 'off');
  // The operator's widgets get a tab of their own, and only exist when they
  // have added some. A receiver with no widgets shows no sign of the feature.
  const hasWidgets = $derived((operatorTheme.value?.widgets?.length ?? 0) > 0);
  // Decodes only where the operator made a decoder public.
  const hasDecodes = $derived(decoders.value.length > 0);

  $effect(() => {
    if ((tab === 'history' && !hasHistory) || (tab === 'station' && !hasWidgets) || (tab === 'decodes' && !hasDecodes) ||
        (tab === 'bands' && (isWide || !bandsNeedTheirOwnTab))) tab = 'receive';
  });

  const sidebarTabs = $derived([
    { id: 'receive', label: 'Receive' },
    { id: 'display', label: 'Display' },
    { id: 'connection', label: 'Stream' },
    ...(hasHistory ? [{ id: 'history', label: 'History' }] : []),
    ...(hasDecodes ? [{ id: 'decodes', label: 'Decodes' }] : []),
    ...(hasWidgets ? [{ id: 'station', label: stationTabLabel(operatorTheme.value) }] : []),
  ]);

  // The same order as the side column's, with the bands first where they
  // need a tab of their own.
  const sheetTabs = $derived([
    ...(bandsNeedTheirOwnTab ? [{ id: 'bands', label: 'Bands' }] : []),
    { id: 'receive', label: 'Receive' },
    { id: 'display', label: 'Display' },
    { id: 'connection', label: 'Stream' },
    ...(hasHistory ? [{ id: 'history', label: 'History' }] : []),
    ...(hasDecodes ? [{ id: 'decodes', label: 'Decodes' }] : []),
    ...(hasWidgets ? [{ id: 'station', label: stationTabLabel(operatorTheme.value) }] : []),
  ]);

  async function share() {
    const url = shareUrl();
    // A phone's share sheet is how a link is passed on there. On a desktop
    // the button copies, as it says: Chrome on Windows has a share sheet too,
    // and opened it instead.
    if (navigator.share && window.matchMedia('(pointer: coarse)').matches) {
      try {
        await navigator.share({ title: information?.name ?? 'FernSDR', url });
        return;
      } catch (problem) {
        if ((problem as DOMException).name === 'AbortError') return;
      }
    }
    if (await copyText(url)) {
      copied = true;
      window.setTimeout(() => (copied = false), 1800);
    } else {
      showNote('This browser does not let the page copy. The address bar holds the same link.');
    }
  }

  /**
   * What to call the widget tab.
   *
   * A receiver with one widget should say what it is - "Chat" reads better than
   * "Station" when the tab holds a chat - and only fall back to a generic name
   * once there is more than one thing behind it.
   */
  function stationTabLabel(theme: { widgets?: { type: string; title?: string }[] } | null): string {
    const widgets = theme?.widgets ?? [];
    if (widgets.length === 1) return widgets[0].title || widgets[0].type;
    return 'Station';
  }

  const setTab = (id: string) => (tab = id);
</script>

{#snippet controls()}
  <div id="receiver-panel" role="tabpanel" aria-labelledby="control-tab-{tab}">
  <MorphPanel viewKey={tab}>
    {#if tab === 'receive'}<ReceivePanel />{/if}
    {#if tab === 'display'}<DisplayPanel />{/if}
    {#if tab === 'connection'}<ConnectionPanel />{/if}
    {#if tab === 'history'}<HistoryPanel />{/if}
    {#if tab === 'decodes'}<DecodesPanel />{/if}
    {#if tab === 'bands'}<BandSelector variant="list" />{/if}
    {#if tab === 'station'}
      {#await loadWidgetPanel()}
        <p role="status">Loading station…</p>
      {:then WidgetPanel}
        <WidgetPanel />
      {:catch}
        <p role="alert">The station widgets did not load. Reload the page to try again.</p>
      {/await}
    {/if}
  </MorphPanel>
  </div>
{/snippet}

<div class="app{isWide ? ' app--wide' : ' app--narrow'}{isWide && layout.value.side === 'left' ? ' app--left' : ''}{editingLayout.value ? ' app--arranging' : ''}">
  <header class="topbar">
    <div class="topbar__identity">
      <h1 class="topbar__name">{information?.name ?? 'FernSDR'}</h1>
      {#if information?.operator}<span class="topbar__operator">{information.operator}</span>{/if}
    </div>

    <BandSelector />

    <div class="topbar__actions">
      <Tooltip
        label={copied ? 'Link copied' : 'Copy a link that opens on this frequency'}
        placement="bottom"
      >
        {#snippet children(anchor)}
          <button
            {@attach anchor}
            type="button"
            class="icon-button"
            onclick={share}
            aria-label={copied ? 'Link copied' : 'Copy a link to this frequency'}
          >
            {#if copied}<Check size={18} />{:else}<Link2 size={18} />{/if}
          </button>
        {/snippet}
      </Tooltip>
      <Tooltip label="Keyboard shortcuts" placement="bottom">
        {#snippet children(anchor)}
          <button
            {@attach anchor}
            type="button"
            class="icon-button"
            onclick={() => (showHelp = true)}
            aria-label="Keyboard shortcuts"
          >
            <Keyboard size={18} />
          </button>
        {/snippet}
      </Tooltip>
    </div>
  </header>

  <Banner />

  <!-- The readout and the meter sit over the display rather than in a row
       of their own: a real receiver puts them on the dial, and the row they
       used to occupy cost 90px of waterfall on every screen. -->
  <main class="stage">
    <SpectrumDisplay />
    <div class="stage-controls">
      <AudioGate />
      <div class="tuner">
        <FrequencyDisplay />
        {#if layout.value.show.meter || layout.value.show.volume || editingLayout.value}
          <div class="tuner__side">
            <Editable label="Meter" shown={layout.value.show.meter} onToggle={(meter) => setLayout({ show: { meter } })}>
              <SMeter />
            </Editable>
            <Editable label="Volume" shown={layout.value.show.volume} onToggle={(volume) => setLayout({ show: { volume } })}>
              <VolumeControl />
            </Editable>
          </div>
        {/if}
      </div>
    </div>
  </main>

  {#if isWide}
    <aside class="sidebar">
      <ControlTabs variant="sidebar" active={tab} onChange={setTab} tabs={sidebarTabs} />
      <div class="sidebar__body">{@render controls()}</div>
      <!-- Bands are the receiver's primary navigation, so on a wide screen
           they get a real list at the foot of the column rather than a
           scrolling strip in the header. -->
      <BandSelector variant="list" />
    </aside>
  {:else}
    <BottomSheet tabs={sheetTabs} activeTab={tab} onTabChange={setTab}>
      {@render controls()}
    </BottomSheet>
  {/if}

  <Editable label="Status line" area="status" inset shown={layout.value.show.status}
    onToggle={(status) => setLayout({ show: { status } })}>
    <StatusBar />
  </Editable>
  {#if editingLayout.value}
    {#await loadEditLayoutBar()}
      <p role="status">Loading…</p>
    {:then EditLayoutBar}
      <EditLayoutBar />
    {:catch}
      <p role="alert">The layout editor did not load. Reload the page to try again.</p>
    {/await}
  {/if}
  {#if showHelp}
    {#await loadKeyboardHelp()}
      <p role="status">Loading shortcuts…</p>
    {:then KeyboardHelp}
      <KeyboardHelp onClose={() => (showHelp = false)} />
    {:catch}
      <p role="alert">The shortcut list did not load. Reload the page to try again.</p>
    {/await}
  {/if}
</div>
