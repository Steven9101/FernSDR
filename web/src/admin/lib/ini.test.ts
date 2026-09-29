import { describe, expect, it } from 'vitest';
import { readKey, setKey, tokenise } from './ini';

const kinds = (line: string) => tokenise(line).map((token) => `${token.kind}:${token.text}`);

describe('tokenise', () => {
  it('keeps every character, so the painted layer lines up with the text', () => {
    for (const line of ['  [band:20m]  ', 'center = 14.1M  # the middle', 'x', '', '   ', 'a=b', '; note']) {
      expect(tokenise(line).map((token) => token.text).join('')).toBe(line);
    }
  });

  it('tells sections, keys, values and comments apart', () => {
    expect(kinds('[band:20m]')).toEqual(['section:[band:20m]']);
    expect(kinds('# a comment')).toEqual(['comment:# a comment']);
    expect(kinds('center = 14.1M # middle')).toEqual([
      'key:center',
      'space: ',
      'equals:=',
      'space: ',
      'number:14.1M',
      'space: ',
      'comment:# middle',
    ]);
    expect(kinds('iq_swap = yes')).toContain('boolean:yes');
    expect(kinds('website = https://example.org')).toContain('url:https://example.org');
  });

  it('colours a value by its whole shape, so a name with a number in it stays a name', () => {
    expect(kinds('name = RTL-SDR 20 m')).toEqual(['key:name', 'space: ', 'equals:=', 'space: ', 'value:RTL-SDR 20 m']);
    expect(kinds('bands = 7.0M, 14.1M')).toEqual([
      'key:bands',
      'space: ',
      'equals:=',
      'space: ',
      'number:7.0M',
      'space:,',
      'space: ',
      'number:14.1M',
    ]);
  });

  it('marks a password as a secret so it is not painted', () => {
    expect(kinds('password_hash = pbkdf2$200000$abc')).toContain('secret:pbkdf2$200000$abc');
  });

  it('does not take a hash inside quotes for a comment', () => {
    expect(kinds('notice = "Net #5 tonight"').some((token) => token.startsWith('comment'))).toBe(false);
  });

  it('marks a line that is none of these, which the server would refuse', () => {
    expect(kinds('center 14.1M')).toEqual(['unknown:center 14.1M']);
  });
});

describe('setKey', () => {
  const file = ['[site]', 'name = Test', '', '[band:20m]', 'center = 14.1M  # the middle', 'ppm = 3', '', '[band:40m]', 'center = 7.1M', ''].join('\n');

  it('replaces the value in the right section and keeps its comment', () => {
    const next = setKey(file, 'band:20m', 'center', '14.2M');
    expect(next).toContain('center = 14.2M # the middle');
    expect(next).toContain('center = 7.1M');
  });

  it('replaces a key without touching the same key in another section', () => {
    const next = setKey(file, 'band:20m', 'ppm', '-5.23');
    expect(next).toContain('ppm = -5.23');
    expect(next?.split('\n').filter((line) => line.startsWith('ppm')).length).toBe(1);
  });

  it("adds a key after the section's last setting", () => {
    const next = setKey(file, 'band:40m', 'ppm', '1.5')?.split('\n');
    expect(next?.indexOf('ppm = 1.5')).toBe((next?.indexOf('center = 7.1M') ?? 0) + 1);
  });

  it('refuses a section the file does not have', () => {
    expect(setKey(file, 'band:80m', 'ppm', '1')).toBeNull();
  });
});

describe('setKey on files as operators write them', () => {
  it('finds the key in a file with Windows line endings and keeps them', () => {
    const file = '[band:20m]\r\ncenter = 14.1M\r\nppm = 3\r\n\r\n[band:40m]\r\nppm = 1\r\n';
    const next = setKey(file, 'band:20m', 'ppm', '-5.23');
    expect(next).toBe('[band:20m]\r\ncenter = 14.1M\r\nppm = -5.23\r\n\r\n[band:40m]\r\nppm = 1\r\n');
  });

  it('changes the last of a key given twice, which is the one the server reads', () => {
    const file = '[band:20m]\nppm = 1\ncenter = 14.1M\nppm = 2\n';
    const next = setKey(file, 'band:20m', 'ppm', '7');
    expect(next).toBe('[band:20m]\nppm = 1\ncenter = 14.1M\nppm = 7\n');
    expect(readKey(next!, 'band:20m', 'ppm')).toBe('7');
  });

  it('adds a key to a section that is last in the file', () => {
    expect(setKey('[band:20m]\ncenter = 14.1M', 'band:20m', 'ppm', '2')).toBe('[band:20m]\ncenter = 14.1M\nppm = 2');
  });
});

describe('readKey', () => {
  it('reads the value the server would, without its comment', () => {
    const file = '[band:20m]\nppm = 1 # old\n[band:40m]\nppm = 9\n';
    expect(readKey(file, 'band:20m', 'ppm')).toBe('1');
    expect(readKey(file, 'band:40m', 'ppm')).toBe('9');
    expect(readKey(file, 'band:80m', 'ppm')).toBeNull();
  });

  it('takes the last of a key given twice, and only from the first section of that name', () => {
    const file = '[band:20m]\nppm = 1\nppm = 2 ; later\n[band:20m]\nppm = 9\n';
    expect(readKey(file, 'band:20m', 'ppm')).toBe('2');
  });

  it('leaves a # inside quotes alone and takes the quotes off', () => {
    const file = '[site]\nname = "Fern #1" # the station\n';
    expect(readKey(file, 'site', 'name')).toBe('Fern #1');
  });
});
