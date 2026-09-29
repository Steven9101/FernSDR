<script lang="ts">
  import Panel from './Panel.svelte';
  import Rack from './Rack.svelte';
  import Segmented from './Segmented.svelte';
  import {
    BANDWIDTH_PROFILES,
    audioLatencyMs,
    audioTargetMs,
    audioUnderruns,
    bandwidthProfile,
    controller,
    currentBand,
    meter,
    site,
    streamTraffic,
  } from '../state/store';
  import { formatBitrate, formatSigned } from '../util/frequency';
  import { Sustained } from './sustained';

  const PROFILE_OPTIONS = BANDWIDTH_PROFILES.map((p) => ({ value: p.id, label: p.label }));

  const profile = $derived(bandwidthProfile.value);
  const stats = $derived(meter.value);
  const traffic = $derived(streamTraffic.value);
  const noiseFloor = $derived(currentBand.value?.noise_floor);

  // The server lowers the waterfall rate to fit its bandwidth budget, and
  // the rate it reports wanders around that as it does. Said only when it
  // has been lower for five seconds, and taken back after fifteen at the
  // full rate; the number shown is the lowest of the last five seconds.
  const reduced = new Sustained(5000, 15000);
  let reducedShown = $state(false);
  let reducedRate = $state(0);
  let recentRates: { at: number; fps: number }[] = [];
  $effect(() => {
    const fps = stats?.waterfall_fps ?? 0;
    const target = profile.waterfallFps;
    const now = performance.now();
    recentRates = recentRates.filter((r) => now - r.at < 5000);
    if (fps > 0) recentRates.push({ at: now, fps });
    reducedShown = reduced.update(now, fps > 0 && fps < target - 0.5);
    if (reducedShown && recentRates.length > 0) reducedRate = Math.min(...recentRates.map((r) => r.fps));
  });

  /**
   * The AGPL section 13 source offer.
   *
   * FernSDR is used over a network and never distributed, which is precisely the
   * case the AGPL exists for: everyone listening to this receiver is entitled to
   * the source it is running. That entitlement is worth nothing if there is no
   * way to act on it from the page itself, so the link is part of the client
   * rather than a line in a README somewhere.
   *
   * An operator who has modified FernSDR sets `source_url` in their config to
   * their own repository. One who has not gets the upstream link, which is the
   * right answer for them.
   */
  const sourceUrl = $derived(site.value?.source_url);
</script>

<Panel title="Stream">
  <Rack title="Data usage">
    <Segmented
      label="Data usage"
      options={PROFILE_OPTIONS}
      value={profile.id}
      onChange={(id) => {
        const next = BANDWIDTH_PROFILES.find((p) => p.id === id);
        if (next) controller.setBandwidthProfile(next);
      }}
    />
    <!-- Room for the longest of the three, so that choosing one does not
         move everything below. -->
    <p class="rack__caption">{profile.description}</p>
  </Rack>

  <Rack title="Now">
  <dl class="stats">
    <div class="stats__row">
      <dt>Audio</dt>
      <dd>{traffic ? formatBitrate(traffic.audioBps) : '--'}</dd>
    </div>
    <!-- The level between the signals, which is what every other level on
         this band is judged against: how strong a station is only means
         something relative to it, and whether the antenna is doing its job
         is exactly the question of whether band noise sits above the
         receiver's own. -->
    <div class="stats__row">
      <dt>Noise floor</dt>
      <dd>
        {noiseFloor !== undefined && noiseFloor > -159
          ? `${formatSigned(noiseFloor, 1)} dBFS`
          : '--'}
      </dd>
    </div>
    <div class="stats__row">
      <dt>Waterfall</dt>
      <dd>{traffic ? formatBitrate(traffic.waterfallBps) : '--'}{#if traffic && traffic.waterfallFps > 0}<span class="stats__note">{' '}&middot; {traffic.waterfallFps.toFixed(0)}/s</span>{/if}</dd>
    </div>
    <div class="stats__row">
      <dt>Status and control</dt>
      <dd>{traffic ? formatBitrate(traffic.controlBps) : '--'}</dd>
    </div>
    <div class="stats__row">
      <dt>Buffer</dt>
      <dd>{audioLatencyMs.value} ms{#if audioTargetMs.value > 0}<span class="stats__note">{' '}&middot; target {audioTargetMs.value} ms</span>{/if}{#if audioUnderruns.value > 0}<span class="stats__note stats__note--warn">{' '}&middot; {audioUnderruns.value} dropouts</span>{/if}</dd>
    </div>
    <div class="stats__row">
      <dt>Listeners</dt>
      <dd>{stats?.listeners ?? '--'}</dd>
    </div>
  </dl>
  <p class="stats__caption">Received stream including WebSocket framing. Network and TLS overhead are additional.</p>
  </Rack>

  {#if sourceUrl}
    <p class="source-offer">
      FernSDR is free software under the <a href="https://www.gnu.org/licenses/agpl-3.0.html" target="_blank" rel="noreferrer noopener">AGPL v3</a>. <a href={sourceUrl} target="_blank" rel="noreferrer noopener">Source for this receiver</a>. <a href="/licenses.txt" target="_blank" rel="noreferrer noopener">Third-party licences</a>.
    </p>
  {/if}

  {#if reducedShown}
    <p class="notice notice--info">
      The receiver is sending about {reducedRate.toFixed(0)} lines a second rather than {profile.waterfallFps}, to stay within its bandwidth limit. Sustained congestion can also reduce audio quality.
    </p>
  {/if}
</Panel>
