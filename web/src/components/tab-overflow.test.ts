import { describe, expect, it } from 'vitest';
import { splitTabs } from './tab-overflow';

const tab = (id: string) => ({ id });
const ids = (list: { id: string }[]) => list.map((t) => t.id);
const all = ['receive', 'display', 'connection', 'history', 'decodes', 'station'].map(tab);

describe('tab overflow', () => {
  it('keeps four or fewer as they are', () => {
    const four = all.slice(0, 4);
    expect(ids(splitTabs(four, 'receive').shown)).toEqual(['receive', 'display', 'connection', 'history']);
    expect(splitTabs(four, 'receive').more).toEqual([]);
  });

  it('shows the decodes in the last place and the rest behind More', () => {
    const { shown, more } = splitTabs(all, 'receive');
    expect(ids(shown)).toEqual(['receive', 'display', 'connection', 'decodes']);
    expect(ids(more)).toEqual(['history', 'station']);
  });

  it('brings the active tab into the bar when it would be hidden', () => {
    const { shown, more } = splitTabs(all, 'station');
    expect(ids(shown)).toEqual(['receive', 'display', 'connection', 'station']);
    expect(ids(more)).toEqual(['history', 'decodes']);
  });

  it('takes the next in order where there are no decodes', () => {
    const { shown, more } = splitTabs(all.filter((t) => t.id !== 'decodes'), 'display');
    expect(ids(shown)).toEqual(['receive', 'display', 'connection', 'history']);
    expect(ids(more)).toEqual(['station']);
  });
});
