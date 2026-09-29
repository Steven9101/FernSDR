import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { CLOSE_INACTIVE, SdrClient } from './client';

class Socket {
  static OPEN = 1;
  static instances: Socket[] = [];
  readyState = 0;
  binaryType = '';
  onopen: (() => void) | null = null;
  onclose: ((event: { code: number; reason: string }) => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onerror = null;
  send = vi.fn();
  close = vi.fn(() => { this.readyState = 3; });
  constructor() { Socket.instances.push(this); }
  open() { this.readyState = 1; this.onopen?.(); }
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.spyOn(performance, 'now').mockImplementation(() => Date.now());
  vi.stubGlobal('window', globalThis);
  vi.stubGlobal('WebSocket', Socket);
  Socket.instances = [];
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); vi.restoreAllMocks(); });

function client() {
  const handlers = { onState: vi.fn(), onMessage: vi.fn(), onBinary: vi.fn(), onReady: vi.fn(), onTraffic: vi.fn() };
  return { receiver: new SdrClient('ws://test', handlers), handlers };
}

describe('connection recovery', () => {
  it('counts audio, waterfall, UTF-8 control and WebSocket framing from received messages', () => {
    const { receiver, handlers } = client();
    receiver.connect(); Socket.instances[0].open();
    const socket = Socket.instances[0];
    socket.onmessage?.({ data: new Uint8Array([1, 0, 0, 0, 0]).buffer });
    const waterfall = new Uint8Array(126); waterfall[0] = 2;
    socket.onmessage?.({ data: waterfall.buffer });
    vi.advanceTimersByTime(1000);
    const control = '{"type":"test","text":"ä"}';
    socket.onmessage?.({ data: control });
    const controlBps = (new TextEncoder().encode(control).byteLength + 2) * 8;
    expect(handlers.onTraffic).toHaveBeenLastCalledWith({ audioBps: 56, waterfallBps: 1040,
      controlBps, totalBps: 1096 + controlBps, waterfallFps: 1 });
    receiver.close(); receiver.connect(); Socket.instances[1].open();
    vi.advanceTimersByTime(1000);
    Socket.instances[1].onmessage?.({ data: control });
    expect(handlers.onTraffic).toHaveBeenLastCalledWith({ audioBps: 0, waterfallBps: 0,
      controlBps, totalBps: controlBps, waterfallFps: 0 });
    receiver.close();
  });
  it('does not take back a place the receiver let go for inactivity', () => {
    const { receiver, handlers } = client();
    const inactive = vi.fn();
    (handlers as Record<string, unknown>).onInactive = inactive;
    receiver.connect(); Socket.instances[0].open();
    Socket.instances[0].onclose?.({ code: CLOSE_INACTIVE, reason: 'no activity for 60 minutes' });
    vi.advanceTimersByTime(60_000);
    expect(Socket.instances).toHaveLength(1);
    expect(inactive).toHaveBeenCalledWith('no activity for 60 minutes');
    expect(handlers.onState).toHaveBeenLastCalledWith('closed', 'no activity for 60 minutes');
    // Listening again is the listener's choice, and it works.
    receiver.connect();
    expect(Socket.instances).toHaveLength(2);
  });

  it('still reconnects after any other close', () => {
    const { receiver } = client();
    receiver.connect(); Socket.instances[0].open();
    Socket.instances[0].onclose?.({ code: 1006, reason: '' });
    vi.advanceTimersByTime(60_000);
    expect(Socket.instances.length).toBeGreaterThan(1);
  });

  it('opens one socket even when connect is called twice', () => {
    const { receiver } = client();
    receiver.connect(); receiver.connect();
    expect(Socket.instances).toHaveLength(1);
    receiver.close();
  });

  it('reports zero throughput during a stall instead of keeping the last rate', () => {
    const { receiver, handlers } = client();
    receiver.connect(); Socket.instances[0].open();
    Socket.instances[0].onmessage?.({ data: new Uint8Array([1, 0, 0, 0, 0]).buffer });
    vi.advanceTimersByTime(2000);
    expect(handlers.onTraffic).toHaveBeenLastCalledWith(expect.objectContaining({ audioBps: 28, totalBps: 28 }));
    vi.advanceTimersByTime(2000);
    expect(handlers.onTraffic).toHaveBeenLastCalledWith({ audioBps: 0, waterfallBps: 0,
      controlBps: 0, totalBps: 0, waterfallFps: 0 });
    receiver.close();
  });

  it('ignores a late close or message from the previous connection', () => {
    const { receiver, handlers } = client();
    receiver.connect();
    const old = Socket.instances[0];
    old.open();
    receiver.close(); receiver.connect();
    Socket.instances[1].open();
    old.onclose?.({ code: 1000, reason: '' });
    old.onmessage?.({ data: '{"type":"state"}' });
    expect(receiver.connected).toBe(true);
    expect(handlers.onMessage).not.toHaveBeenCalled();
    receiver.close();
  });

  it('reconnects a silent socket without waiting for its close handshake', () => {
    const { receiver } = client();
    receiver.connect();
    Socket.instances[0].open();
    vi.advanceTimersByTime(16000);
    expect(Socket.instances.length).toBeGreaterThan(1);
    receiver.close();
  });

  it('accepts an idle control connection and rejects malformed JSON shapes', () => {
    const { receiver, handlers } = client();
    receiver.connect(); Socket.instances[0].open();
    for (let i = 0; i < 4; i++) {
      vi.advanceTimersByTime(8000);
      Socket.instances[0].onmessage?.({ data: '{"type":"meter"}' });
      Socket.instances[0].onmessage?.({ data: 'null' });
    }
    expect(Socket.instances).toHaveLength(1);
    expect(handlers.onMessage).toHaveBeenCalledTimes(4);
    receiver.close();
  });
});
