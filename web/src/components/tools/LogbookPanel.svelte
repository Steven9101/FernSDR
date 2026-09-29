<script lang="ts">
  /**
   * The listener's log, opened from the book under the dial: the time, the
   * frequency and the mode are taken from the receiver when the panel opens,
   * so logging what was just heard is a callsign and Enter.
   */
  import { tuning } from '../../state/store';
  import { addLogEntry, loadLogbook, logbook, removeLogEntry, toAdif, toCsv } from '../../state/logbook';
  import { signalForCarrier } from '../../util/cw';
  import { saveFile } from '../../util/save-file';

  /** The list shows the latest; the rest are in the exports. */
  const SHOWN = 30;

  // Read each time the panel opens, not with the page: the log can be large,
  // most listeners never open it, and another tab may have added to it.
  loadLogbook();

  const tune = tuning.value;
  const heard = Date.now();
  const freq = signalForCarrier(tune.freq, tune.mode, tune.cwPitch);

  let station = $state('');
  let report = $state('');
  let note = $state('');
  let message = $state('');

  const utc = (ms: number) => new Date(ms).toISOString().slice(11, 16);
  const day = (ms: number) => new Date(ms).toISOString().slice(0, 10);

  function log() {
    const added = addLogEntry({ time: heard, freq, mode: tune.mode, station, report, note });
    message = added ? '' : 'The log is full; export it and remove old entries.';
    if (added) station = report = note = '';
  }

  function exportAs(kind: 'adif' | 'csv') {
    // Oldest first, as logging programs and people read a log.
    const entries = logbook.value.slice().reverse();
    const stamp = new Date().toISOString().slice(0, 10);
    if (kind === 'adif') saveFile(toAdif(entries), `fernsdr-log-${stamp}.adi`);
    else saveFile(toCsv(entries), `fernsdr-log-${stamp}.csv`, 'text/csv');
  }
</script>

<div class="tool">
  <p class="tool__note">{day(heard)} {utc(heard)} UTC, {(freq / 1e6).toFixed(4)} MHz {tune.mode.toUpperCase()}</p>
  <form class="tool__form" onsubmit={(event) => { event.preventDefault(); log(); }}>
    <input class="tool__input" bind:value={station} maxlength="40" placeholder="Station or callsign" aria-label="Station or callsign" />
    <input class="tool__input" bind:value={report} maxlength="12" placeholder="Report" aria-label="Report, such as RST or SINPO" />
    <input class="tool__input" bind:value={note} maxlength="200" placeholder="Note" aria-label="Note" />
    <button type="submit" class="button button--small button--primary">Log</button>
  </form>

  {#if logbook.value.length === 0}
    <p class="tool__note">Nothing logged yet. The log stays in this browser; export it as ADIF for a logging program.</p>
  {:else}
    <ul class="tool__list">
      {#each logbook.value.slice(0, SHOWN) as entry (entry.id)}
        <li class="tool__item">
          <span class="tool__go tool__go--static">
            <span class="tool__name">{entry.station || 'Unidentified'}{entry.report ? `, ${entry.report}` : ''}</span>
            <span class="tool__detail">{day(entry.time)} {utc(entry.time)} {(entry.freq / 1e6).toFixed(4)} {entry.mode.toUpperCase()}{entry.note ? `, ${entry.note}` : ''}</span>
          </span>
          <button type="button" class="tool__icon" aria-label="Remove the entry for {entry.station || 'an unidentified station'}" onclick={() => removeLogEntry(entry.id)}>✕</button>
        </li>
      {/each}
    </ul>
    {#if logbook.value.length > SHOWN}
      <p class="tool__note">And {logbook.value.length - SHOWN} earlier, in the exports.</p>
    {/if}
  {/if}

  <div class="tool__footer">
    <button type="button" class="button button--small" onclick={() => exportAs('adif')} disabled={logbook.value.length === 0}>Export ADIF</button>
    <button type="button" class="button button--small" onclick={() => exportAs('csv')} disabled={logbook.value.length === 0}>CSV</button>
    {#if message}<span class="tool__note" role="status">{message}</span>{/if}
  </div>
</div>
