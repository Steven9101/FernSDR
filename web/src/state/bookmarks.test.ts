import { beforeEach, describe, expect, it, vi } from 'vitest';
import { addBookmark, bookmarks, exportBookmarks, importBookmarks, loadBookmarks, MAX_BOOKMARKS, removeBookmark, renameBookmark, validBookmark } from './bookmarks';

const stored = new Map<string, string>();

beforeEach(() => {
  stored.clear();
  vi.stubGlobal('localStorage', { getItem: (k: string) => stored.get(k) ?? null, setItem: (k: string, v: string) => stored.set(k, v) });
  bookmarks.value = [];
});

describe('bookmarks', () => {
  it('keeps, renames and removes, newest first, and survives a reload', () => {
    addBookmark({ name: 'DWD', freq: 7_646_000, mode: 'usb', low: 300, high: 2700 });
    const second = addBookmark({ name: '', freq: 9_420_000, mode: 'am', low: -4500, high: 4500 })!;
    expect(bookmarks.value.map((b) => b.name)).toEqual(['9.4200 MHz', 'DWD']);
    renameBookmark(second.id, '  Greece  ');
    expect(bookmarks.value[0].name).toBe('Greece');
    bookmarks.value = [];
    loadBookmarks();
    expect(bookmarks.value.map((b) => b.name)).toEqual(['Greece', 'DWD']);
    removeBookmark(second.id);
    expect(bookmarks.value.map((b) => b.name)).toEqual(['DWD']);
  });

  it('refuses what is not a bookmark', () => {
    for (const bad of [null, 5, {}, { freq: -1, mode: 'usb', low: 0, high: 1 }, { freq: 7e6, mode: 'jt65', low: 0, high: 1 },
      { freq: 7e6, mode: 'usb', low: 3000, high: 300 }, { freq: 'x', mode: 'usb', low: 0, high: 1 }]) {
      expect(validBookmark(bad)).toBeNull();
    }
    expect(validBookmark({ freq: 7e6, mode: 'USB', low: 300, high: 2700, name: 'x'.repeat(200) })!.name).toHaveLength(80);
  });

  it('moves between browsers by export and import, without duplicates or overflow', () => {
    addBookmark({ name: 'A', freq: 7_074_000, mode: 'usb', low: 0, high: 3000 });
    const file = exportBookmarks();
    bookmarks.value = [];
    expect(importBookmarks(file)).toEqual({ added: 1 });
    expect(importBookmarks(file)).toEqual({ added: 0 });
    expect(importBookmarks('not json')).toEqual({ error: 'That file is not a bookmarks export.' });
    expect(importBookmarks('{"bookmarks": 3}')).toEqual({ error: 'That file is not a bookmarks export.' });
    const many = Array.from({ length: MAX_BOOKMARKS + 50 }, (_, i) => ({ freq: 1e6 + i * 1000, mode: 'am', low: -4500, high: 4500 }));
    const result = importBookmarks(JSON.stringify(many));
    expect('added' in result && result.added).toBe(MAX_BOOKMARKS - 1);
    expect(bookmarks.value).toHaveLength(MAX_BOOKMARKS);
  });
});
