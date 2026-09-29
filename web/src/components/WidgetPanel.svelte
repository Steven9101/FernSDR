<script lang="ts">
  /**
   * The operator's widgets, on the listener's page.
   *
   * Everything here renders operator-supplied strings as text nodes and
   * operator-supplied URLs as attributes, never as markup. The server has
   * already refused anything that is not a hex colour or an acceptable URL
   * scheme; this does not undo that by building HTML out of it.
   *
   * `embed` and the lightning map are the exception and honest about it: an
   * iframe pointing at whatever the operator chose, sandboxed to scripts and
   * its own origin - no forms, no popups, no top-level navigation - so a page
   * that turns hostile cannot steer the listener away. Its own origin is what
   * a map needs for its settings and tiles (Blitzortung's stays blank without
   * it), and gives it nothing of ours: a page from another origin cannot
   * reach into this one either way. Only a page from this very server could
   * use it to lift the sandbox, so that one keeps scripts alone.
   */
  import Chat from './Chat.svelte';
  import Clock from './Clock.svelte';
  import Greyline from './Greyline.svelte';
  import StationCard from './StationCard.svelte';
  import SpaceWeather from './SpaceWeather.svelte';
  import { controller, operatorTheme, site } from '../state/store';
  import { locatorCentre } from '../util/locator';

  const widgets = $derived(operatorTheme.value?.widgets ?? []);
  // Frames the listener has asked to see, for this visit.
  let opened = $state(new Set<string>());
  function hostOf(url: string): string {
    try {
      return new URL(url, location.href).host;
    } catch {
      return 'another site';
    }
  }

  function frameSandbox(url: string): string {
    try {
      return new URL(url, location.href).origin === location.origin ? 'allow-scripts' : 'allow-scripts allow-same-origin';
    } catch {
      return 'allow-scripts';
    }
  }

  // Blitzortung's live map, centred on this station when the operator gave
  // no address of their own (or the bare one the panel used to suggest).
  function lightningUrl(url: string | undefined): string {
    if (url && url.replace(/\/+$/, '') !== 'https://map.blitzortung.org') return url;
    const here = locatorCentre(site.value?.grid);
    return here
      ? `https://map.blitzortung.org/#6/${here.lat.toFixed(2)}/${here.lon.toFixed(2)}`
      : 'https://map.blitzortung.org/#3/30/10';
  }
</script>

<!-- What this receiver is hearing, by band, from the listener counts it already has. -->
{#snippet conditions()}
  {@const list = controller.bandList()}
  {#if list.length === 0}
    <p class="widget__text">No bands.</p>
  {:else}
    {@const busiest = Math.max(1, ...list.map((band) => band.listeners))}
    <ul class="widget__bars">
      {#each list as band}
        <li class="widget__bar">
          <span class="widget__bar-label">{band.name}</span>
          <span class="widget__bar-track">
            <span class="widget__bar-fill" style:width="{(band.listeners / busiest) * 100}%"></span>
          </span>
          <span class="widget__bar-value">{band.listeners}</span>
        </li>
      {/each}
    </ul>
  {/if}
{/snippet}

{#if widgets.length > 0}
  <div class="widgets">
    {#each widgets as widget}
      <section class="widget">
        <h3 class="widget__head">{widget.title ?? widget.type}</h3>
        <div class="widget__body">
        {#if widget.type === 'chat'}
          {#if site.value?.chat === false}
            <p class="widget__text">The chat is off on this receiver.</p>
          {:else}
            <Chat height={widget.height ?? 260} />
          {/if}
        {:else if widget.type === 'clock'}
          <Clock />
        {:else if widget.type === 'space'}
          <SpaceWeather />
        {:else if widget.type === 'greyline'}
          <Greyline />
        {:else if widget.type === 'station'}
          <StationCard />
        {:else if widget.type === 'spots'}
          {@render conditions()}
        {:else if widget.type === 'notice'}
          <p class="widget__text">{widget.text}</p>
        {:else if widget.type === 'links'}
          <ul class="widget__links">
            {#each widget.items ?? [] as item}
              <li>
                <a href={item.url} target="_blank" rel="noreferrer noopener">{item.label || item.url}</a>
              </li>
            {/each}
          </ul>
        {:else if widget.type === 'image'}
          {#if widget.url}<img class="widget__image" src={widget.url} alt={widget.title ?? ''} />{/if}
        {:else if widget.type === 'lightning' || widget.type === 'embed'}
          {@const url = widget.type === 'lightning' ? lightningUrl(widget.url) : widget.url}
          {#if url && opened.has(url)}
            <iframe
              class="widget__frame"
              src={url}
              style:height="{widget.height ?? 260}px"
              loading="lazy"
              referrerpolicy="no-referrer"
              sandbox={frameSandbox(url)}
              title={widget.title ?? widget.type}
            ></iframe>
          {:else if url}
            <!-- Another site's page loads only when asked for: until then it
                 cannot set cookies, show its consent banner or learn who is
                 listening here. -->
            <button type="button" class="widget__reveal" style:height="{widget.height ?? 260}px"
              onclick={() => (opened = new Set([...opened, url]))}>
              <span class="widget__reveal-title">Show {widget.type === 'lightning' ? 'the lightning map' : widget.title ?? 'this page'}</span>
              <span class="widget__reveal-host">Loads {hostOf(url)}</span>
            </button>
          {/if}
        {/if}
        </div>
      </section>
    {/each}
  </div>
{/if}
