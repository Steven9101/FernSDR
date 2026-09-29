<script lang="ts">
  import { onMount } from 'svelte';
  import Radio from '@lucide/svelte/icons/radio';
  import Send from '@lucide/svelte/icons/send';
  import { bands, chatLog, chatRefusal, controller, tuning } from '../state/store';
  import { describeTuning, segment, type Spot } from '../util/spots';
  import { signalForCarrier } from '../util/cw';
  import { readStored } from '../util/storage';

  let { height }: { height: number } = $props();

  let draft = $state('');
  let name = $state(readStored('fernsdr.chat.name') ?? '');
  // Open until Done is pressed, not "open while the name is empty" - the field
  // has to survive the first character typed into it.
  let naming = $state(!readStored('fernsdr.chat.name'));
  let list: HTMLDivElement;

  onMount(() => {
    controller.requestChatHistory();
  });

  $effect(() => {
    void chatLog.value;
    // Follow the conversation, but only when already at the bottom: yanking
    // the view down while somebody is reading back is worse than missing a
    // line.
    const atBottom = list.scrollHeight - list.scrollTop - list.clientHeight < 60;
    if (atBottom) list.scrollTop = list.scrollHeight;
  });

  function send(event: SubmitEvent) {
    event.preventDefault();
    const text = draft.trim();
    if (!text) return;
    try { localStorage.setItem('fernsdr.chat.name', name); } catch {
      // The chat still works when private browsing denies persistent storage.
    }
    controller.sendChat(text, name);
    draft = '';
  }

  function shareTuning() {
    const now = tuning.value;
    const signal = signalForCarrier(now.freq, now.mode, now.cwPitch);
    const said = describeTuning(signal, now.mode);
    draft = draft.trim() ? `${draft.trim()} ${said}` : said;
  }
</script>

<!--
  A message, with the frequencies in it made into somewhere you can go.

  Still a text node either way: the pieces come from slicing the message the
  listener was sent, never from building markup out of it, and a piece that is
  not a frequency is rendered exactly as it arrived.
-->
{#snippet said(text: string)}
  {#each segment(text, bands.value) as part, index (index)}{#if part.spot}{@render spotLink(part.spot)}{:else}<span>{part.text}</span>{/if}{/each}
{/snippet}

{#snippet spotLink(spot: Spot)}
  {@const label = spot.mode ? `${spot.mode.toUpperCase()} at ` : ''}
  <button
    type="button"
    class="chat__spot"
    title="Tune to {label}{(spot.hz / 1000).toFixed(3)} kHz"
    onclick={() => controller.goTo(spot.hz, spot.mode)}
  >{spot.text}</button>
{/snippet}

<div class="chat">
  <!-- The height the operator chose is a custom property rather than an
       inline height, so a narrow layout can override it in CSS: on a phone
       the chat has to fit the sheet, and their 260px was chosen looking at
       a desktop. -->
  <div class="chat__log" bind:this={list} style:--chat-height="{height}px">
    {#if chatLog.value.length === 0}<p class="chat__empty">Nothing said yet.</p>{/if}
    {#each chatLog.value as message (message.id)}
      <p class="chat__line"><span class="chat__name">{message.name}</span><span class="chat__text">{@render said(message.text)}</span></p>
    {/each}
  </div>
  {#if chatRefusal.value}<p class="chat__refusal">{chatRefusal.value}</p>{/if}
  <!--
    The call sign is asked for once and then gets out of the way.

    A permanent name field beside the message box costs a third of the
    width, which on a phone is the difference between seeing what you are
    typing and not. Once somebody has entered a call it becomes a chip
    they can tap to change, and the message box gets the room.
  -->
  {#if naming}
    <div class="chat__identify">
      <input
        class="chat__name-input"
        placeholder="Your call"
        maxlength="24"
        bind:value={name}
        autocapitalize="characters"
        autocomplete="off"
        spellcheck={false}
        enterkeyhint="done"
        aria-label="Your name or callsign"
        onkeydown={(event) => { if (event.key === 'Enter') { event.preventDefault(); naming = false; } }}
      />
      <button type="button" class="button button--small" onclick={() => (naming = false)}>
        Done
      </button>
    </div>
  {/if}
  <form class="chat__compose" onsubmit={send}>
    {#if name && !naming}
      <button
        type="button"
        class="chat__as"
        title="Change your call"
        onclick={() => (naming = true)}
      >
        {name}
      </button>
    {/if}
    <!-- What somebody in a receiver's chat most often wants to say is
         where they are listening. Typing it out is a dozen taps on a phone
         and a chance to get a digit wrong. -->
    <button
      type="button"
      class="chat__share"
      title="Put where you are listening into the message"
      aria-label="Share where you are listening"
      onclick={shareTuning}
    >
      <Radio size={15} />
    </button>
    <input
      class="chat__input"
      placeholder="Message"
      maxlength="400"
      bind:value={draft}
      enterkeyhint="send"
      aria-label="Message"
    />
    <button type="submit" class="chat__send" aria-label="Send" disabled={!draft.trim()}>
      <Send size={16} />
    </button>
  </form>
</div>
