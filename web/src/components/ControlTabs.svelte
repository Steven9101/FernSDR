<script lang="ts">
  import Popover from './Popover.svelte';
  import { splitTabs } from './tab-overflow';

  interface Props {
    tabs: { id: string; label: string }[];
    active: string;
    onChange(id: string): void;
    variant: 'sidebar' | 'sheet';
  }

  let { tabs, active, onChange, variant }: Props = $props();

  // History, Decodes and the operator's tab come on top of the usual three
  // where a receiver has them; past four the rest wait behind "More".
  const split = $derived(splitTabs(tabs, active));
  let moreOpen = $state(false);
  let moreButton = $state<HTMLButtonElement | null>(null);

  function onKeyDown(event: KeyboardEvent & { currentTarget: HTMLButtonElement }, index: number) {
    const shown = split.shown;
    const next = event.key === 'ArrowRight' ? (index + 1) % shown.length
      : event.key === 'ArrowLeft' ? (index + shown.length - 1) % shown.length
      : event.key === 'Home' ? 0 : event.key === 'End' ? shown.length - 1 : -1;
    if (next < 0) return;
    event.preventDefault();
    onChange(shown[next].id);
    const buttons = event.currentTarget.parentElement?.querySelectorAll<HTMLButtonElement>('[role="tab"]');
    buttons?.[next].focus();
  }

  function choose(id: string) {
    moreOpen = false;
    onChange(id);
  }
</script>

<div class="{variant}__tabs">
  <div class="{variant}__tablist" role="tablist" aria-label="Receiver controls">
    {#each split.shown as tab, index (tab.id)}
      <button
        id="control-tab-{tab.id}"
        type="button"
        role="tab"
        aria-selected={active === tab.id}
        aria-controls="receiver-panel"
        tabindex={active === tab.id ? 0 : -1}
        class="{variant}__tab{active === tab.id ? ' is-active' : ''}"
        title={tab.label}
        onclick={() => onChange(tab.id)}
        onkeydown={(event) => onKeyDown(event, index)}
      >{tab.label}</button>
    {/each}
  </div>
  {#if split.more.length > 0}
    <button
      bind:this={moreButton}
      type="button"
      class="{variant}__tab {variant}__more"
      aria-label="More: {split.more.map((tab) => tab.label).join(', ')}"
      aria-haspopup="dialog"
      aria-expanded={moreOpen}
      title="More"
      onclick={() => (moreOpen = !moreOpen)}
    >⋯</button>
  {/if}
</div>

<Popover label="More controls" open={moreOpen} anchor={moreButton} onClose={() => (moreOpen = false)}
  side={variant === 'sheet' ? 'top' : 'bottom'} align="end" class="popover--menu">
  <div class="tabs-more">
    {#each split.more as tab (tab.id)}
      <button type="button" class="tabs-more__item" data-tab={tab.id} onclick={() => choose(tab.id)}>{tab.label}</button>
    {/each}
  </div>
</Popover>
