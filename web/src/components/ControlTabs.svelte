<script lang="ts">
  interface Props {
    tabs: { id: string; label: string }[];
    active: string;
    onChange(id: string): void;
    variant: 'sidebar' | 'sheet';
  }

  let { tabs, active, onChange, variant }: Props = $props();

  function onKeyDown(event: KeyboardEvent & { currentTarget: HTMLButtonElement }, index: number) {
    const next = event.key === 'ArrowRight' ? (index + 1) % tabs.length
      : event.key === 'ArrowLeft' ? (index + tabs.length - 1) % tabs.length
      : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1;
    if (next < 0) return;
    event.preventDefault();
    onChange(tabs[next].id);
    const buttons = event.currentTarget.parentElement?.querySelectorAll<HTMLButtonElement>('[role="tab"]');
    buttons?.[next].focus();
  }
</script>

<div class="{variant}__tabs" role="tablist" aria-label="Receiver controls">
  {#each tabs as tab, index (tab.id)}
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
