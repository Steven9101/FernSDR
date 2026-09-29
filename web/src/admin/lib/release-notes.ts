/**
 * Release notes, as the release carries them: the changelog's section for
 * the version, in the few Markdown forms the changelog uses. They become
 * blocks of plain text here, never markup, so a note can only ever be shown
 * as words: headings, lists with their wrapped lines joined, paragraphs, and
 * `code`, **strong** and *emphasis* inside them. A link shows its text.
 */

export interface Span {
  text: string;
  code?: boolean;
  strong?: boolean;
  em?: boolean;
}

export type Block =
  | { kind: 'heading'; text: string }
  | { kind: 'list'; items: Span[][] }
  | { kind: 'paragraph'; spans: Span[] };

export function parseNotes(notes: string): Block[] {
  const blocks: Block[] = [];
  let paragraph: string[] = [];
  let items: string[] | null = null;
  const flush = () => {
    if (paragraph.length) blocks.push({ kind: 'paragraph', spans: parseInline(paragraph.join(' ')) });
    paragraph = [];
    if (items?.length) blocks.push({ kind: 'list', items: items.map(parseInline) });
    items = null;
  };
  for (const raw of notes.replace(/\r/g, '').split('\n')) {
    const line = raw.trimEnd();
    const heading = /^#{1,6}\s+(.*)$/.exec(line);
    const item = /^\s{0,3}[-*]\s+(.*)$/.exec(line);
    if (!line.trim()) {
      flush();
    } else if (heading) {
      flush();
      blocks.push({ kind: 'heading', text: plain(heading[1].trim()) });
    } else if (item) {
      if (paragraph.length) flush();
      items ??= [];
      items.push(item[1].trim());
    } else if (items && /^\s/.test(line)) {
      items[items.length - 1] += ` ${line.trim()}`;
    } else {
      if (items) flush();
      paragraph.push(line.trim());
    }
  }
  flush();
  return blocks;
}

/** A heading's words, without the marks inline text may carry. */
function plain(text: string): string {
  return parseInline(text).map((span) => span.text).join('');
}

export function parseInline(text: string): Span[] {
  // Links keep their words; the address is not shown or followed.
  const unlinked = text.replace(/\[([^\]]+)\]\([^)\s]*\)/g, '$1');
  const spans: Span[] = [];
  const pattern = /`([^`]+)`|\*\*([^*]+)\*\*|(?<![\w*])\*([^*\s][^*]*?)\*(?![\w*])|(?<!\w)_([^_\s][^_]*?)_(?!\w)/g;
  let at = 0;
  for (const match of unlinked.matchAll(pattern)) {
    if (match.index! > at) spans.push({ text: unlinked.slice(at, match.index) });
    if (match[1] !== undefined) spans.push({ text: match[1], code: true });
    else if (match[2] !== undefined) spans.push({ text: match[2], strong: true });
    else spans.push({ text: (match[3] ?? match[4])!, em: true });
    at = match.index! + match[0].length;
  }
  if (at < unlinked.length) spans.push({ text: unlinked.slice(at) });
  return spans;
}
