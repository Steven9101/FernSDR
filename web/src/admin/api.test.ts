import { afterEach, expect, it, vi } from 'vitest';
import { createHmac, pbkdf2Sync, webcrypto } from 'node:crypto';

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.resetModules(); });

it('derives the challenge key, binds writes to the login and preserves counters across a backwards clock', async () => {
  const stored = new Map<string, string>();
  vi.stubGlobal('sessionStorage', { getItem: (k: string) => stored.get(k) ?? null,
    setItem: (k: string, v: string) => stored.set(k, v), removeItem: (k: string) => stored.delete(k) });
  vi.stubGlobal('crypto', webcrypto);
  const clock = vi.spyOn(Date, 'now').mockReturnValue(10000);
  const salt = '1'.repeat(32), nonce = '2'.repeat(32), context = '3'.repeat(64);
  const key = pbkdf2Sync('test password', salt, 100, 32, 'sha256');
  const counters: number[] = [];
  vi.stubGlobal('fetch', vi.fn(async (path: string, options: RequestInit) => {
    if (path.endsWith('/challenge')) return Response.json({ salt, nonce, iterations: 100 });
    if (path.endsWith('/login')) {
      expect(JSON.parse(options.body as string).proof).toBe(createHmac('sha256', key).update(nonce).digest('hex'));
      return Response.json({ ok: true, signing_context: context });
    }
    const headers = options.headers as Record<string, string>;
    const count = headers['X-FernSDR-Counter'];
    counters.push(Number(count));
    const message = `fernsdr-admin-v3\n${context}\n${count}\nPOST\n${path}\n${options.body}`;
    const sessionKey = createHmac('sha256', key).update(`fernsdr-admin-session-v3\n${context}`).digest();
    expect(headers['X-FernSDR-Signature']).toBe(createHmac('sha256', sessionKey).update(message).digest('hex'));
    return Response.json({ ok: true });
  }));
  const { api } = await import('./api');
  await api.login('test password');
  await api.writeStation({ name: 'Test' });
  clock.mockReturnValue(100);
  vi.resetModules();
  const reloaded = await import('./api');
  await reloaded.api.writeStation({ name: 'Reloaded' });
  // Counters are the clock in microseconds, or one more than the last.
  expect(counters).toEqual([10_000_001, 10_000_002]);
});

it('serialises mutations so later signatures cannot reach the server first', async () => {
  vi.stubGlobal('sessionStorage', { getItem: () => null });
  let finishFirst!: () => void;
  const fetch = vi.fn().mockImplementationOnce(() => new Promise<Response>(resolve => {
    finishFirst = () => resolve(Response.json({ ok: true }));
  })).mockResolvedValue(Response.json({ ok: true }));
  vi.stubGlobal('fetch', fetch);
  const { api } = await import('./api');
  const first = api.writeStation({ name: 'first' });
  const second = api.writeStation({ name: 'second' });
  await Promise.resolve();
  expect(fetch).toHaveBeenCalledTimes(1);
  finishFirst();
  await Promise.all([first, second]);
  expect(fetch).toHaveBeenCalledTimes(2);
});

it('keeps two tabs of one login in order, and signs again once when the server saw a later counter', async () => {
  // One session, two tabs: the second a copy of the first, as a duplicated
  // tab is, loaded later. The server takes a counter only above the last.
  const stored = new Map<string, string>();
  vi.stubGlobal('sessionStorage', { getItem: (k: string) => stored.get(k) ?? null,
    setItem: (k: string, v: string) => stored.set(k, v), removeItem: (k: string) => stored.delete(k) });
  vi.stubGlobal('crypto', webcrypto);
  const clock = vi.spyOn(Date, 'now').mockReturnValue(1_000);
  let last = 0;
  let replayOnce = false;
  const outcomes: number[] = [];
  vi.stubGlobal('fetch', vi.fn(async (path: string, options: RequestInit) => {
    if (path.endsWith('/challenge')) return Response.json({ salt: '1'.repeat(32), nonce: '2'.repeat(32), iterations: 10 });
    if (path.endsWith('/login')) return Response.json({ ok: true, signing_context: '3'.repeat(64) });
    const count = Number((options.headers as Record<string, string>)['X-FernSDR-Counter']);
    if (replayOnce) { replayOnce = false; last = count; }  // another tab got there with this very counter
    if (count <= last) { outcomes.push(401); return Response.json({ error: 'this request was not signed; sign in again' }, { status: 401 }); }
    last = count;
    outcomes.push(200);
    return Response.json({ ok: true });
  }));
  const tabA = (await import('./api')).api;
  await tabA.login('pw');
  await tabA.writeStation({ name: 'A1' });
  vi.resetModules();
  clock.mockReturnValue(2_000);
  const tabB = (await import('./api')).api;
  await tabB.writeStation({ name: 'B1' });
  await tabB.writeStation({ name: 'B2' });
  clock.mockReturnValue(3_000);
  await tabA.writeStation({ name: 'A2' });
  expect(outcomes).toEqual([200, 200, 200, 200]);
  // A clash all the same, within one millisecond: signed again, once, and through.
  replayOnce = true;
  await tabA.writeStation({ name: 'A3' });
  expect(outcomes.slice(4)).toEqual([401, 200]);
});
