<script lang="ts" module>
  import { controller, tuning } from '../../state/store';
  import { watch } from '../../state/reactive.svelte';
  import { signalForCarrier } from '../../util/cw';
  import { RigLink, type ReceiverAccess } from '../../cat/link';

  const receiver: ReceiverAccess = {
    current() {
      const t = tuning.value;
      return { freq: signalForCarrier(t.freq, t.mode, t.cwPitch), mode: t.mode };
    },
    tune: (freq, mode) => controller.goTo(freq, mode),
    watch(changed) {
      // A watcher runs once as it starts; that run is where the receiver
      // already is, not a change the radio has to hear about.
      let first = true;
      return watch(() => {
        void tuning.value;
        if (first) first = false;
        else changed();
      });
    },
  };

  // One link for the page, not one per opening of the panel: closing the
  // panel leaves the radio linked.
  const link = new RigLink(receiver);
</script>

<script lang="ts">
  /**
   * The listener's own radio, linked over its CAT cable: the receiver can
   * follow its dial, set it, or both. Nothing here goes through the receiver;
   * the browser talks to the radio directly.
   */
  import Segmented from '../Segmented.svelte';
  import { DRIVERS, ICOM_ADDRESSES } from '../../cat/drivers';
  import { BAUD_RATES, loadRigSettings, rigState, saveRigSettings, serialAvailable, type RigSettings } from '../../cat/link';

  const DIRECTIONS = [
    { value: 'follow', label: 'Follow', title: 'The receiver goes where the radio is tuned' },
    { value: 'control', label: 'Control', title: 'The radio goes where the receiver is tuned' },
    { value: 'both', label: 'Both', title: 'Either one moves the other' },
  ] as const;

  let settings = $state<RigSettings>(loadRigSettings());
  const available = serialAvailable();
  const secure = typeof window !== 'undefined' && window.isSecureContext;
  const status = $derived(rigState.value);
  const busy = $derived(status.status === 'on' || status.status === 'connecting');
  const driver = $derived(DRIVERS.find((d) => d.id === settings.driver) ?? DRIVERS[0]);
  const hex = (n: number) => n.toString(16).toUpperCase().padStart(2, '0');

  function change(next: Partial<RigSettings>) {
    settings = { ...settings, ...next };
    saveRigSettings(settings);
  }

  function chooseDriver(id: string) {
    const next = DRIVERS.find((d) => d.id === id);
    if (next) change({ driver: next.id, baud: next.defaultBaud });
  }

  function setAddress(text: string) {
    const value = Number.parseInt(text, 16);
    if (Number.isInteger(value) && value > 0 && value < 0xe0) change({ address: value });
  }
</script>

<div class="tool">
  {#if !available && !secure}
    <!-- Browsers keep serial ports from pages that arrive unencrypted, whatever
         the browser: nothing in the page can change that. -->
    <p class="tool__note">
      Your browser lets a page reach your radio's port only when the page came over https. This receiver was
      opened over http, so the port stays out of reach. If it also has an https address, open it there.
    </p>
  {:else if !available}
    <p class="tool__note">
      Linking your own radio needs a browser that can reach a serial port, such as Chrome, Edge or Opera on a
      computer. This one cannot.
    </p>
  {:else}
    <label class="tool__field">
      <span class="tool__label">Radio</span>
      <select class="tool__input" value={settings.driver} disabled={busy} onchange={(event) => chooseDriver(event.currentTarget.value)}>
        {#each DRIVERS as option (option.id)}
          <option value={option.id}>{option.label}</option>
        {/each}
      </select>
    </label>

    <div class="tool__pair">
      <label class="tool__field">
        <span class="tool__label">Baud</span>
        <select class="tool__input" value={settings.baud} disabled={busy} onchange={(event) => change({ baud: Number(event.currentTarget.value) })}>
          {#each BAUD_RATES as rate (rate)}
            <option value={rate}>{rate}</option>
          {/each}
        </select>
      </label>
      {#if driver.needsAddress}
        <label class="tool__field">
          <span class="tool__label">CI-V address</span>
          <select class="tool__input" value={settings.address} disabled={busy} onchange={(event) => change({ address: Number(event.currentTarget.value) })}>
            {#each ICOM_ADDRESSES as model (model.address)}
              <option value={model.address}>{model.label}, {hex(model.address)}h</option>
            {/each}
            {#if !ICOM_ADDRESSES.some((model) => model.address === settings.address)}
              <option value={settings.address}>{hex(settings.address)}h</option>
            {/if}
          </select>
        </label>
        <label class="tool__field tool__field--narrow">
          <span class="tool__label">Hex</span>
          <input class="tool__input" value={hex(settings.address)} maxlength="2" disabled={busy}
            aria-label="CI-V address in hexadecimal" onchange={(event) => setAddress(event.currentTarget.value)} />
        </label>
      {/if}
    </div>

    <div class="tool__field">
      <span class="tool__label">Sync</span>
      <Segmented label="Sync" options={DIRECTIONS} value={settings.direction} disabled={busy} onChange={(direction) => change({ direction })} />
    </div>

    <div class="tool__footer">
      {#if busy}
        <button type="button" class="button button--small" onclick={() => link.disconnect()}>Disconnect</button>
      {:else}
        <button type="button" class="button button--small button--primary" onclick={() => link.connect(settings)}>Connect</button>
      {/if}
      {#if status.status === 'on' && status.freq !== null}
        <span class="tool__note" role="status">Radio on {(status.freq / 1e6).toFixed(4)} MHz{status.mode ? ` ${status.mode.toUpperCase()}` : ''}</span>
      {/if}
    </div>
    {#if status.message}
      <p class="tool__note{status.status === 'error' || status.message.startsWith('No answer') ? ' tool__note--problem' : ''}" role="status">{status.message}</p>
    {/if}
    <p class="tool__note">
      If your radio can key the transmitter from the cable's DTR or RTS line (a menu setting on many radios),
      switch that off first: opening the port raises both for a moment. The link itself only reads and sets the
      frequency and mode, and never transmits. Your browser talks to the radio directly; nothing goes through this
      receiver.
    </p>
  {/if}
</div>
