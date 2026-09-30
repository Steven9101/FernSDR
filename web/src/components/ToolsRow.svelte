<script lang="ts" module>
  import { lazy } from './lazy';
  import { box } from '../state/reactive.svelte';
  import { Recording } from '../audio/recorder';

  // Each tool's panel loads the first time it is opened: none of them makes
  // the page heavier for a listener who never uses it.
  const loadBookmarks = lazy(() => import('./tools/BookmarksPanel.svelte'));
  const loadLogbook = lazy(() => import('./tools/LogbookPanel.svelte'));
  const loadRig = lazy(() => import('./tools/RigPanel.svelte'));
  // A recording belongs to the page, not to this row: hiding the tools, or
  // arranging the page, must not leave one running with no way to stop or
  // save it.
  export const recordingNow = box<Recording | null>(null);
</script>

<script lang="ts">
  /**
   * The tools under the dial: small buttons that open a panel of their own
   * rather than taking a tab. They are there for everyone: each is one
   * button until it is used, and costs nothing until then.
   */
  import Popover from './Popover.svelte';
  import Editable from './Editable.svelte';
  import { layout, setLayout } from '../state/layout';
  import { controller, tuning, viewport } from '../state/store';
  import { loadBookmarks as readBookmarks } from '../state/bookmarks';
  import { rigState } from '../cat/state';
  import { equalizeVfo, loadVfo, swapVfo, vfo, type VfoSetting } from '../state/vfo';
  import { extensionFor, recordingName } from '../audio/recorder';

  let {
    recordingOnly = false,
  }: {
    /** Only the record button: the tools are hidden, but a recording runs and must stay in reach. */
    recordingOnly?: boolean;
  } = $props();
  import { edgesAtPitch, signalForCarrier } from '../util/cw';
  import { saveFile } from '../util/save-file';

  let open = $state<'bookmarks' | 'log' | 'rig' | null>(null);
  let bookmarkButton = $state<HTMLButtonElement | null>(null);
  let logButton = $state<HTMLButtonElement | null>(null);
  let rigButton = $state<HTMLButtonElement | null>(null);
  const recording = $derived(recordingNow.value);
  let elapsed = $state(0);
  let problem = $state('');

  readBookmarks();
  loadVfo();

  const linked = $derived(rigState.value.status === 'on');
  const other = $derived(vfo.value[vfo.value.active === 'a' ? 'b' : 'a']);

  function here(): VfoSetting {
    const tune = tuning.value;
    const view = viewport.value;
    return {
      freq: signalForCarrier(tune.freq, tune.mode, tune.cwPitch),
      mode: tune.mode,
      low: tune.low,
      high: tune.high,
      pitch: tune.cwPitch,
      band: tune.band,
      view: [view.lowHz, view.highHz],
    };
  }

  function swap() {
    swapVfo(here(), (setting) => {
      // The band first, where bands overlap and the frequency alone would
      // land on the wrong one; the view last, since tuning moves it.
      if (setting.band) controller.selectBand(setting.band);
      if (!controller.goTo(setting.freq, setting.mode)) return;
      const edges = edgesAtPitch(setting.mode, setting.low, setting.high, setting.pitch, tuning.value.cwPitch);
      controller.setPassband(edges.low, edges.high);
      if (setting.view) controller.setViewport(setting.view[0], setting.view[1]);
    });
  }

  const describe = (setting: VfoSetting | null) =>
    setting ? `${(setting.freq / 1e6).toFixed(4)} MHz ${setting.mode.toUpperCase()}` : 'where you are now';

  $effect(() => {
    if (!recording) return;
    const timer = window.setInterval(() => (elapsed = Date.now() - (recording?.started ?? Date.now())), 1000);
    return () => window.clearInterval(timer);
  });

  async function toggleRecording() {
    problem = '';
    if (recording) {
      const done = recording;
      recordingNow.value = null;
      const blob = await done.stop();
      const tune = tuning.value;
      saveFile(blob, recordingName(signalForCarrier(tune.freq, tune.mode, tune.cwPitch), tune.mode, done.started, extensionFor(done.type)));
      return;
    }
    const started = Recording.start(controller.player);
    if (typeof started === 'string') problem = started;
    else {
      elapsed = 0;
      recordingNow.value = started;
    }
  }

  const clock = (ms: number) => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, '0')}`;
</script>

<div class="tools" role="toolbar" aria-label="Receiver tools">
  {#if !recordingOnly}
  <Editable label="VFO A and B" inline shown={layout.value.tools.vfo} onToggle={(vfo) => setLayout({ tools: { vfo } })}>
  <button
    type="button"
    class="tools__button"
    aria-label="VFO {vfo.value.active.toUpperCase()} in use; switch to the other, {describe(other)}"
    title="Switch to VFO {vfo.value.active === 'a' ? 'B' : 'A'}: {describe(other)}"
    onclick={swap}
  >{vfo.value.active.toUpperCase()}</button>
  <button
    type="button"
    class="tools__button"
    title="Copy this VFO into the other one"
    onclick={() => equalizeVfo(here())}
  >A=B</button>
  </Editable>
  {/if}
  {#if !recordingOnly}
  <Editable label="Bookmarks" inline shown={layout.value.tools.bookmarks} onToggle={(bookmarks) => setLayout({ tools: { bookmarks } })}>
  <button
    bind:this={bookmarkButton}
    type="button"
    class="tools__button{open === 'bookmarks' ? ' is-open' : ''}"
    aria-expanded={open === 'bookmarks'}
    aria-label="Bookmarks"
    title="Bookmarks"
    onclick={() => (open = open === 'bookmarks' ? null : 'bookmarks')}
  >
    <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 1.8l1.8 3.9 4.2.5-3.1 2.9.8 4.2L8 11.2l-3.7 2.1.8-4.2L2 6.2l4.2-.5z" /></svg>
  </button>
  </Editable>
  {/if}
  <Editable label="Recording" inline shown={layout.value.tools.record || recording !== null} onToggle={(record) => setLayout({ tools: { record } })}>
  <button
    type="button"
    class="tools__button{recording ? ' is-recording' : ''}"
    aria-pressed={recording !== null}
    aria-label={recording ? `Stop recording, ${clock(elapsed)}` : 'Record what you hear'}
    title={problem || (recording ? 'Stop and save the recording' : 'Record what you hear')}
    onclick={toggleRecording}
  >
    <span class="tools__dot" aria-hidden="true"></span>
    {#if recording}<span class="tools__time">{clock(elapsed)}</span>{/if}
  </button>
  </Editable>
  {#if !recordingOnly}
  <Editable label="Logbook" inline shown={layout.value.tools.logbook} onToggle={(logbook) => setLayout({ tools: { logbook } })}>
  <button
    bind:this={logButton}
    type="button"
    class="tools__button{open === 'log' ? ' is-open' : ''}"
    aria-expanded={open === 'log'}
    aria-label="Log"
    title="Log what you hear"
    onclick={() => (open = open === 'log' ? null : 'log')}
    >
    <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3.5 1.5h8a1 1 0 0 1 1 1v11a1 1 0 0 1-1 1h-8a1 1 0 0 1-1-1v-11a1 1 0 0 1 1-1zm1.5 3v1h5v-1zm0 3v1h5v-1zm0 3v1h3v-1z" fill-rule="evenodd" /></svg>
  </button>
  </Editable>
  {/if}
  {#if !recordingOnly}
  <Editable label="Radio link" inline shown={layout.value.tools.rig} onToggle={(rig) => setLayout({ tools: { rig } })}>
  <button
    bind:this={rigButton}
    type="button"
    class="tools__button{open === 'rig' ? ' is-open' : ''}{linked ? ' is-linked' : ''}"
    aria-expanded={open === 'rig'}
    aria-label={linked ? 'Your radio, linked' : 'Link your radio'}
    title={linked ? 'Your radio is linked' : 'Link your own radio over its CAT cable'}
    onclick={() => (open = open === 'rig' ? null : 'rig')}
    >
    <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M2 5.5A1.5 1.5 0 0 1 3.5 4h9A1.5 1.5 0 0 1 14 5.5v5a1.5 1.5 0 0 1-1.5 1.5h-9A1.5 1.5 0 0 1 2 10.5zm3 2.5a1.5 1.5 0 1 0 3 0 1.5 1.5 0 0 0-3 0zm5-1h2v1h-2zm0 2h2v1h-2zM4 2.5h5V4H4z" fill-rule="evenodd" /></svg>
  </button>
  </Editable>
  {/if}
  {#if problem}<span class="tools__problem" role="status">{problem}</span>{/if}
</div>

<!-- A page open across an update of the receiver asks for a panel by its old
     name; say so rather than open an empty box. -->
{#snippet notLoaded()}
  <div class="tool">
    <p class="tool__note" role="alert">This panel did not load; the receiver may have been updated since this page opened.</p>
    <div class="tool__footer">
      <button type="button" class="button button--small button--primary" onclick={() => location.reload()}>Reload the page</button>
    </div>
  </div>
{/snippet}

<Popover label="Bookmarks" open={open === 'bookmarks'} anchor={bookmarkButton} onClose={() => (open = null)}>
  {#await loadBookmarks()}
    <p class="tool__loading" role="status">Loading…</p>
  {:then Panel}
    <Panel />
  {:catch}
    {@render notLoaded()}
  {/await}
</Popover>

<Popover label="Log" open={open === 'log'} anchor={logButton} onClose={() => (open = null)}>
  {#await loadLogbook()}
    <p class="tool__loading" role="status">Loading…</p>
  {:then Panel}
    <Panel />
  {:catch}
    {@render notLoaded()}
  {/await}
</Popover>

<Popover label="Your radio" open={open === 'rig'} anchor={rigButton} onClose={() => (open = null)}>
  {#await loadRig()}
    <p class="tool__loading" role="status">Loading…</p>
  {:then Panel}
    <Panel />
  {:catch}
    {@render notLoaded()}
  {/await}
</Popover>
