/**
 * A listener's bookmarks: frequencies worth coming back to, with the mode and
 * filter they were heard in. Kept in this browser only, never sent anywhere,
 * and moved between browsers as a JSON file the listener exports and imports.
 */
import { box } from './reactive.svelte';

export interface Bookmark {
  id: string;
  name: string;
  /** The signal's frequency in Hz, as the dial shows it. */
  freq: number;
  mode: string;
  low: number;
  high: number;
  /** The CW pitch the edges were kept at, Hz: they move with it on the way back. */
  pitch?: number;
  /** When it was kept, ms since 1970. */
  created: number;
}

const STORAGE_KEY = 'fernsdr.bookmarks.v1';
/** More than anyone keeps by hand, few enough that a bad import cannot fill storage. */
export const MAX_BOOKMARKS = 1000;
const MODES = new Set(['usb', 'lsb', 'cw', 'cwl', 'am', 'sam', 'nfm', 'dsb', 'wfm']);

export const bookmarks = box<Bookmark[]>([]);

/** A stored or imported entry, or null when it is not one. */
export function validBookmark(value: unknown): Bookmark | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const freq = Number(v.freq);
  const low = Number(v.low);
  const high = Number(v.high);
  const mode = String(v.mode ?? '').toLowerCase();
  if (!Number.isFinite(freq) || freq <= 0 || freq > 1e11) return null;
  if (!MODES.has(mode) || !Number.isFinite(low) || !Number.isFinite(high) || low >= high) return null;
  const name = String(v.name ?? '').trim().slice(0, 80);
  const created = Number(v.created);
  const pitch = Number(v.pitch);
  return {
    id: typeof v.id === 'string' && /^[a-z0-9-]{1,40}$/i.test(v.id) ? v.id : newId(),
    name: name || `${(freq / 1e6).toFixed(4)} MHz`,
    freq: Math.round(freq),
    mode,
    low: Math.round(low),
    high: Math.round(high),
    ...(v.pitch !== undefined && pitch >= 200 && pitch <= 1500 ? { pitch: Math.round(pitch) } : {}),
    created: Number.isFinite(created) && created > 0 ? created : Date.now(),
  };
}

function newId(): string {
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

function save(list: Bookmark[]): void {
  bookmarks.value = list;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
  } catch {
    // Private browsing or storage off: they last as long as the page.
  }
}

export function loadBookmarks(): void {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '[]');
    if (Array.isArray(parsed)) bookmarks.value = parsed.map(validBookmark).filter((b): b is Bookmark => b !== null).slice(0, MAX_BOOKMARKS);
  } catch {
    bookmarks.value = [];
  }
}

export function addBookmark(entry: Omit<Bookmark, 'id' | 'created'>): Bookmark | null {
  const bookmark = validBookmark({ ...entry, id: newId(), created: Date.now() });
  if (!bookmark || bookmarks.value.length >= MAX_BOOKMARKS) return null;
  save([bookmark, ...bookmarks.value]);
  return bookmark;
}

export function renameBookmark(id: string, name: string): void {
  const trimmed = name.trim().slice(0, 80);
  if (!trimmed) return;
  save(bookmarks.value.map((b) => (b.id === id ? { ...b, name: trimmed } : b)));
}

export function removeBookmark(id: string): void {
  save(bookmarks.value.filter((b) => b.id !== id));
}

export function exportBookmarks(): string {
  return JSON.stringify({ fernsdr: 'bookmarks', version: 1, bookmarks: bookmarks.value }, null, 1);
}

/**
 * Adds the bookmarks in an exported file, skipping ones already kept at the
 * same frequency and mode. Returns how many were added, or an error.
 */
export function importBookmarks(text: string): { added: number } | { error: string } {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    return { error: 'That file is not a bookmarks export.' };
  }
  const list = Array.isArray(parsed) ? parsed : (parsed as { bookmarks?: unknown })?.bookmarks;
  if (!Array.isArray(list)) return { error: 'That file is not a bookmarks export.' };
  const have = new Set(bookmarks.value.map((b) => `${b.freq}:${b.mode}`));
  const incoming: Bookmark[] = [];
  for (const item of list) {
    const bookmark = validBookmark(item);
    if (!bookmark || have.has(`${bookmark.freq}:${bookmark.mode}`)) continue;
    have.add(`${bookmark.freq}:${bookmark.mode}`);
    incoming.push({ ...bookmark, id: newId() });
  }
  const room = Math.max(0, MAX_BOOKMARKS - bookmarks.value.length);
  save([...bookmarks.value, ...incoming.slice(0, room)]);
  return { added: Math.min(incoming.length, room) };
}
