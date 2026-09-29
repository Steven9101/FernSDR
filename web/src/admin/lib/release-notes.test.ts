import { describe, expect, it } from 'vitest';
import { parseInline, parseNotes } from './release-notes';

describe('release notes', () => {
  it('reads a changelog section into headings, lists and paragraphs', () => {
    const notes = [
      'The first release.',
      '',
      '### Receiver',
      '',
      '- Demodulates USB, LSB and CW, with a passband that can be dragged',
      '  to any width.',
      '- Hours on the air for each band.',
      '',
      '### Operator',
      'Run `install.sh` again.',
    ].join('\n');
    expect(parseNotes(notes)).toEqual([
      { kind: 'paragraph', spans: [{ text: 'The first release.' }] },
      { kind: 'heading', text: 'Receiver' },
      {
        kind: 'list',
        items: [
          [{ text: 'Demodulates USB, LSB and CW, with a passband that can be dragged to any width.' }],
          [{ text: 'Hours on the air for each band.' }],
        ],
      },
      { kind: 'heading', text: 'Operator' },
      { kind: 'paragraph', spans: [{ text: 'Run ' }, { text: 'install.sh', code: true }, { text: ' again.' }] },
    ]);
  });

  it('marks code, strong and emphasis, and keeps only the words of a link', () => {
    expect(parseInline('**Full** controls, *Essential* and `ppm`; see [the guide](docs/GUIDE.md).')).toEqual([
      { text: 'Full', strong: true },
      { text: ' controls, ' },
      { text: 'Essential', em: true },
      { text: ' and ' },
      { text: 'ppm', code: true },
      { text: '; see the guide.' },
    ]);
    // Not emphasis: a lone star, snake_case and arithmetic.
    expect(parseInline('2 * 3 and max_user_bitrate')).toEqual([{ text: '2 * 3 and max_user_bitrate' }]);
  });

  it('leaves markup as text', () => {
    const blocks = parseNotes('<img src=x onerror=alert(1)>');
    expect(blocks).toEqual([{ kind: 'paragraph', spans: [{ text: '<img src=x onerror=alert(1)>' }] }]);
  });
});
