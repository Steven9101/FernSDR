import { afterEach, expect, it, vi } from 'vitest';
import { readStored } from './storage';

afterEach(() => vi.unstubAllGlobals());
it('reads saved preferences and distinguishes a missing value', () => {
  const storage = new Map([['saved', 'value']]);
  vi.stubGlobal('localStorage', { getItem: (key: string) => storage.get(key) ?? null });
  expect(readStored('saved')).toBe('value');
  expect(readStored('missing')).toBeNull();
});
it('allows listening when browser storage is unavailable', () => {
  vi.stubGlobal('localStorage', { getItem: () => { throw new Error('disabled'); } });
  expect(readStored('missing')).toBeNull();
});
