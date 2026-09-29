<script lang="ts">
	import { tick } from 'svelte';
	import { completionsFor, sectionAt, type Section, type Setting } from '../config-schema';
	import { tokenise, type TokenKind } from '../lib/ini';

	/*
	The configuration file, painted as configuration and completed as you type.

	Written rather than installed: an editor component is where a project like this picks up its
	first heavy dependency, and what is needed is one file format with about sixty keys. The
	technique is the old one, a coloured layer behind a transparent textarea in the same font and
	metrics, scrolled together. The textarea keeps everything a browser already does for editing
	(selection, undo, mobile keyboards, screen readers) and the layer behind only paints.
	*/
	let {
		value = $bindable(''),
		section = $bindable<Section | null>(null),
		label
	}: { value?: string; section?: Section | null; label: string } = $props();

	let area: HTMLTextAreaElement | undefined = $state();
	let paint: HTMLPreElement | undefined = $state();
	let mark: HTMLSpanElement | undefined = $state();
	let list: HTMLUListElement | undefined = $state();
	let caret = $state(0);
	let open = $state(false);
	let chosen = $state(0);
	let at = $state<{ left: number; top: number } | null>(null);

	const typing = $derived.by(() => {
		const start = value.lastIndexOf('\n', caret - 1) + 1;
		const line = value.slice(start, caret);
		if (/^\s*[#;[]/.test(line) || line.includes('=')) return null;
		return { line, start };
	});
	const suggestions = $derived(typing ? completionsFor(sectionAt(value, caret), typing.line) : []);
	const lines = $derived(value.split('\n'));
	const caretLine = $derived(value.slice(0, caret).split('\n').length - 1);
	const caretColumn = $derived(caret - (value.lastIndexOf('\n', caret - 1) + 1));

	$effect(() => {
		section = sectionAt(value, caret);
	});

	// A new word starts the list from the top.
	$effect(() => {
		void typing?.line;
		chosen = 0;
	});

	// Held inside the editor: hung off the caret alone, a list opened deep in an indented line runs
	// off the right of a phone and takes the whole page sideways with it. Near the bottom of the
	// editor it opens above the line instead, where the save bar and the keyboard cannot cover it.
	$effect(() => {
		void caret;
		void value;
		if (!open || !mark || !paint) return;
		const m = mark.getBoundingClientRect();
		const box = paint.getBoundingClientRect();
		const width = list?.offsetWidth ?? 0;
		const height = list?.offsetHeight ?? 0;
		const room = Math.max(0, box.width - width - 8);
		const below = m.bottom - box.top + 6;
		const above = m.top - box.top - height - 6;
		at = {
			left: Math.max(4, Math.min(m.left - box.left, room)),
			top: below + height > box.height && above >= 0 ? above : below
		};
	});

	async function accept(setting: Setting) {
		if (!area || !typing) return;
		const indent = /^\s*/.exec(typing.line)?.[0] ?? '';
		const insert = `${indent}${setting.key} = ${setting.example ?? setting.values?.[0] ?? ''}`;
		value = value.slice(0, typing.start) + insert + value.slice(caret);
		open = false;
		const end = typing.start + insert.length;
		await tick();
		// The caret goes where the value is, the only place anyone wants it after picking a key.
		area.focus();
		area.setSelectionRange(end, end);
		caret = end;
	}

	function onkeydown(event: KeyboardEvent) {
		if (!open || suggestions.length === 0) return;
		if (event.key === 'ArrowDown') {
			event.preventDefault();
			chosen = (chosen + 1) % suggestions.length;
		} else if (event.key === 'ArrowUp') {
			event.preventDefault();
			chosen = (chosen - 1 + suggestions.length) % suggestions.length;
		} else if (event.key === 'Enter' || event.key === 'Tab') {
			event.preventDefault();
			void accept(suggestions[chosen]);
		} else if (event.key === 'Escape') {
			// Only the list closes; Escape in an editor should not also close whatever it sits in.
			event.preventDefault();
			event.stopPropagation();
			open = false;
		}
	}

	function sync() {
		if (!area || !paint) return;
		paint.scrollTop = area.scrollTop;
		paint.scrollLeft = area.scrollLeft;
	}

	const tone: Record<TokenKind, string> = {
		space: '',
		comment: 'text-muted-foreground/75 italic',
		section: 'font-semibold text-foreground',
		key: 'text-foreground',
		equals: 'text-muted-foreground/60',
		value: 'text-muted-foreground',
		number: 'text-success',
		boolean: 'text-warning',
		url: 'text-muted-foreground underline underline-offset-2',
		// Drawn with its own letters in no colour over a bar, so it is hidden and still exactly as
		// wide as the text in the field underneath, whatever the face.
		secret: 'rounded-sm bg-muted-foreground/25 text-transparent',
		// A line that is none of the above is refused by the receiver; better seen while typing.
		unknown: 'text-destructive underline decoration-wavy underline-offset-3'
	};

	// Both layers take exactly these, or the colours drift off the letters as the file scrolls.
	const metrics =
		'm-0 rounded-2xl border px-4 py-3 text-[13px] leading-[1.6] [tab-size:4] break-words whitespace-pre-wrap';
</script>

<div class="relative">
	<pre
		bind:this={paint}
		aria-hidden="true"
		class="{metrics} pointer-events-none absolute inset-0 overflow-hidden border-border bg-card text-muted-foreground">{#each lines as line, index (index)}<div class="min-h-[1lh]">{#if index === caretLine}{@const tokens = tokenise(line)}{#each tokens as token, position (position)}{@const from = tokens.slice(0, position).reduce((sum, item) => sum + item.text.length, 0)}{#if caretColumn > from && caretColumn < from + token.text.length}<span class={tone[token.kind]}>{token.text.slice(0, caretColumn - from)}</span><span bind:this={mark} class="inline-block h-[1em] w-0 align-baseline"></span><span class={tone[token.kind]}>{token.text.slice(caretColumn - from)}</span>{:else}{#if caretColumn === from}<span bind:this={mark} class="inline-block h-[1em] w-0 align-baseline"></span>{/if}<span class={tone[token.kind]}>{token.text}</span>{/if}{/each}{#if caretColumn >= line.length}<span bind:this={mark} class="inline-block h-[1em] w-0 align-baseline"></span>{/if}{:else}{#each tokenise(line) as token, position (position)}<span class={tone[token.kind]}>{token.text}</span>{/each}{/if}{'\n'}</div>{/each}</pre>

	<textarea
		bind:this={area}
		bind:value
		aria-label={label}
		spellcheck="false"
		autocomplete="off"
		autocapitalize="off"
		class="{metrics} relative block h-[50dvh] min-h-72 w-full resize-y md:h-[62dvh] border-transparent bg-transparent text-transparent caret-foreground outline-none selection:bg-primary/25 focus-visible:border-ring focus-visible:ring-3 focus-visible:ring-ring/30"
		onscroll={sync}
		{onkeydown}
		onblur={() => (open = false)}
		oninput={(event) => {
			caret = event.currentTarget.selectionStart ?? 0;
			// Only once a setting is being typed: on an empty line Enter must make a new line, not
			// write in a key nobody asked for.
			open = (typing?.line.trim() ?? '') !== '';
		}}
		onclick={(event) => {
			caret = event.currentTarget.selectionStart ?? 0;
			open = false;
		}}
		onkeyup={(event) => {
			if (!['ArrowDown', 'ArrowUp', 'Enter', 'Tab', 'Escape'].includes(event.key) || !open) caret = event.currentTarget.selectionStart ?? 0;
		}}
	></textarea>

	{#if open && suggestions.length > 0}
		<ul
			bind:this={list}
			role="listbox"
			aria-label="Settings that fit here"
			class="absolute z-10 max-h-64 w-[min(26rem,calc(100%-1rem))] overflow-y-auto rounded-2xl bg-popover p-1 shadow-[0_12px_40px_-8px_oklch(0_0_0/0.35),0_0_0_1px_var(--border)]"
			style={at ? `left: ${at.left}px; top: ${at.top}px` : 'left: 1rem; top: 2.5rem'}
		>
			{#each suggestions as setting, index (setting.key)}
				<li>
					<button
						type="button"
						role="option"
						aria-selected={index === chosen}
						class="flex w-full items-baseline gap-3 rounded-xl px-3 py-2 text-left {index === chosen ? 'bg-accent' : 'hover:bg-accent/60'}"
						onmousedown={(event) => {
							// The list closes on blur, and blur comes before click.
							event.preventDefault();
							void accept(setting);
						}}
					>
						<span class="text-[13px] whitespace-nowrap">{setting.key}</span>
						<span class="min-w-0 flex-1 truncate text-[12px] text-muted-foreground">{setting.detail}</span>
					</button>
				</li>
			{/each}
		</ul>
	{/if}
</div>
