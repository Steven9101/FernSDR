<script lang="ts">
  /**
   * One part of the page the listener may hide, as the edit mode shows it:
   * outlined, with a button to hide it, or where it is hidden, a dashed
   * placeholder in its place that brings it back.
   *
   * The part is never rebuilt when the edit mode opens or closes: the wrapper
   * stays and only changes its class, and outside the edit mode it takes no
   * part in the layout (`display: contents`). Rebuilding would drop what the
   * part holds, a recording under way or an open popover among it.
   *
   * While editing, the part itself does not respond (`inert`): a tap meant for
   * the outline must not open the logbook or start a recording.
   */
  import Eye from '@lucide/svelte/icons/eye';
  import EyeOff from '@lucide/svelte/icons/eye-off';
  import type { Snippet } from 'svelte';
  import { editingLayout } from '../state/layout';

  let {
    label,
    shown,
    onToggle,
    inline = false,
    container = false,
    inset = false,
    area,
    children,
  }: {
    /** What it is, as a listener calls it: "Volume", "Band name". */
    label: string;
    shown: boolean;
    onToggle: (shown: boolean) => void;
    /** Sits in a row of buttons rather than on its own. */
    inline?: boolean;
    /** Holds parts of its own to arrange, such as the tools: not made inert. */
    container?: boolean;
    /** At the edge of the screen: the outline and its button go inside. */
    inset?: boolean;
    /** The page grid's area the part sits in, for its outline to take the same place. */
    area?: string;
    children: Snippet;
  } = $props();

  const editing = $derived(editingLayout.value);

  // Set here rather than in the markup: a part that becomes inert while one
  // of its buttons has focus loses that focus at once, and the blur runs
  // that button's handlers, a tooltip closing say, which may not change
  // state in the middle of the page being drawn, only after.
  let part = $state<HTMLDivElement | null>(null);
  $effect(() => {
    if (part) part.inert = editing && !container;
  });
</script>

{#if shown}
  <div
    class="editable{editing ? ' is-editing' : ''}{inline ? ' editable--inline' : ''}{inset ? ' editable--inset' : ''}{container
      ? ' editable--container'
      : ''}"
    style:grid-area={editing ? area : undefined}
  >
    <div class="editable__part" bind:this={part}>{@render children()}</div>
    {#if editing}
      <button
        type="button"
        class="editable__eye"
        aria-label="Hide the {label.toLowerCase()}"
        title="Hide the {label.toLowerCase()}"
        onclick={() => onToggle(false)}
      >
        <EyeOff size={14} aria-hidden="true" />
      </button>
    {/if}
  </div>
{:else if editing}
  <button
    type="button"
    class="editable__ghost"
    style:grid-area={area}
    aria-label="Show the {label.toLowerCase()}"
    onclick={() => onToggle(true)}
  >
    <Eye size={14} aria-hidden="true" />
    <span>{label}</span>
  </button>
{/if}

<style>
  /* Outside the edit mode the wrapper is not there, as far as layout goes. */
  .editable:not(.is-editing),
  .editable:not(.is-editing) > .editable__part {
    display: contents;
  }

  .editable.is-editing {
    position: relative;
    border-radius: var(--radius-sm);
    outline: 1.5px dashed color-mix(in srgb, var(--primary) 60%, transparent);
    outline-offset: 4px;
  }

  .editable--inline.is-editing {
    display: inline-flex;
  }

  .is-editing > .editable__part {
    /* The part as it is, a little quieter: it is being arranged, not used. */
    opacity: 0.85;
  }

  .editable--container.is-editing > .editable__part {
    opacity: 1;
  }

  .editable__eye {
    pointer-events: auto;
    position: absolute;
    top: -14px;
    right: -14px;
    display: grid;
    place-items: center;
    width: 26px;
    height: 26px;
    border-radius: var(--radius-pill);
    background: var(--card);
    color: var(--foreground);
    box-shadow: var(--shadow-chip);
    border: 1px solid var(--border-strong);
    z-index: 2;
  }

  .editable__eye:hover {
    background: var(--well);
  }

  .editable__eye:focus-visible,
  .editable__ghost:focus-visible {
    outline: 2px solid var(--ring);
    outline-offset: 2px;
  }

  /* A container's own button goes to the corner below, clear of its parts' and
     its neighbour's, which sit along the top. */
  .editable--container > .editable__eye {
    top: auto;
    right: auto;
    bottom: -14px;
    left: -14px;
  }

  .editable--inset.is-editing {
    outline-offset: -3px;
  }

  .editable--inset > .editable__eye {
    top: 50%;
    right: var(--space-2);
    transform: translateY(-50%);
  }

  .editable__ghost {
    pointer-events: auto;
    white-space: nowrap;
    display: inline-flex;
    align-items: center;
    gap: var(--space-1-5);
    min-height: 32px;
    padding: 0 var(--space-3);
    border: 1.5px dashed var(--border-strong);
    border-radius: var(--radius-sm);
    color: var(--muted-foreground);
    font-size: var(--text-xs);
    background: transparent;
  }

  .editable__ghost:hover {
    color: var(--foreground);
    border-color: color-mix(in srgb, var(--primary) 60%, transparent);
  }

  /* Fingers get room enough to hit the small buttons. */
  @media (pointer: coarse) {
    .editable__eye {
      width: 36px;
      height: 36px;
      top: -18px;
      right: -18px;
    }

    .editable--container > .editable__eye {
      top: auto;
      right: auto;
      bottom: -18px;
      left: -18px;
    }

    .editable__ghost {
      min-height: 40px;
    }
  }
</style>
