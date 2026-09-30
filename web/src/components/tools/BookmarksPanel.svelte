<script lang="ts">
  /**
   * The listener's bookmarks, opened from the star under the dial: keep this
   * frequency, go back to one, rename, remove, and carry them to another
   * browser as a file.
   */
  import { controller, tuning } from '../../state/store';
  import { describeFrequency } from '../../state/bandplan';
  import {
    addBookmark,
    bookmarks,
    exportBookmarks,
    importBookmarks,
    removeBookmark,
    renameBookmark,
    type Bookmark,
  } from '../../state/bookmarks';
  import { edgesAtPitch, signalForCarrier } from '../../util/cw';
  import { saveFile } from '../../util/save-file';

  let name = $state('');
  let editing = $state<string | null>(null);
  let draft = $state('');
  let message = $state('');
  let fileInput = $state<HTMLInputElement | null>(null);

  const tune = $derived(tuning.value);
  const signal = $derived(signalForCarrier(tune.freq, tune.mode, tune.cwPitch));
  const kept = $derived(bookmarks.value.find((b) => b.freq === Math.round(signal) && b.mode === tune.mode));

  function keep() {
    const added = addBookmark({
      name: name.trim() || describeFrequency(signal) || '',
      freq: signal,
      mode: tune.mode,
      low: tune.low,
      high: tune.high,
      pitch: tune.cwPitch,
    });
    message = added ? '' : 'The list is full; remove some first.';
    name = '';
  }

  function go(bookmark: Bookmark) {
    if (!controller.goTo(bookmark.freq, bookmark.mode)) return;
    const edges = edgesAtPitch(bookmark.mode, bookmark.low, bookmark.high, bookmark.pitch, tuning.value.cwPitch);
    controller.setPassband(edges.low, edges.high);
  }


  async function upload(file: File | undefined) {
    if (!file) return;
    if (file.size > 2 * 1024 * 1024) {
      message = 'That file is too large for a bookmarks export.';
      return;
    }
    const result = importBookmarks(await file.text());
    message = 'error' in result ? result.error : `${result.added} added.`;
  }
</script>

<div class="tool">
  <form class="tool__row" onsubmit={(event) => { event.preventDefault(); keep(); }}>
    <input class="tool__input" bind:value={name} maxlength="80"
      placeholder={kept ? `Kept as ${kept.name}` : `Name for ${(signal / 1e6).toFixed(4)} MHz`} aria-label="Bookmark name" />
    <button type="submit" class="button button--small button--primary" disabled={!!kept}>Keep</button>
  </form>

  {#if bookmarks.value.length === 0}
    <p class="tool__note">Nothing kept yet. Bookmarks stay in this browser.</p>
  {:else}
    <ul class="tool__list">
      {#each bookmarks.value as bookmark (bookmark.id)}
        <li class="tool__item">
          {#if editing === bookmark.id}
            <form class="tool__row" onsubmit={(event) => { event.preventDefault(); renameBookmark(bookmark.id, draft); editing = null; }}>
              <input class="tool__input" bind:value={draft} maxlength="80" aria-label="New name" />
              <button type="submit" class="button button--small">Save</button>
            </form>
          {:else}
            <button type="button" class="tool__go" onclick={() => go(bookmark)} title="Tune to {bookmark.name}">
              <span class="tool__name">{bookmark.name}</span>
              <span class="tool__detail">{(bookmark.freq / 1e6).toFixed(4)} {bookmark.mode.toUpperCase()}</span>
            </button>
            <button type="button" class="tool__icon" aria-label="Rename {bookmark.name}"
              onclick={() => { editing = bookmark.id; draft = bookmark.name; }}>✎</button>
            <button type="button" class="tool__icon" aria-label="Remove {bookmark.name}" onclick={() => removeBookmark(bookmark.id)}>✕</button>
          {/if}
        </li>
      {/each}
    </ul>
  {/if}

  <div class="tool__footer">
    <button type="button" class="button button--small" onclick={() => saveFile(exportBookmarks(), 'fernsdr-bookmarks.json', 'application/json')} disabled={bookmarks.value.length === 0}>Export</button>
    <button type="button" class="button button--small" onclick={() => fileInput?.click()}>Import</button>
    <input bind:this={fileInput} type="file" accept="application/json,.json" hidden
      onchange={(event) => { upload(event.currentTarget.files?.[0]); event.currentTarget.value = ''; }} />
    {#if message}<span class="tool__note" role="status">{message}</span>{/if}
  </div>
</div>
