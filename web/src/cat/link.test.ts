import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { RigLink, rigState, type ReceiverAccess, type RigSettings } from './link';

/** A radio on a serial port: answers FA/MD like a Kenwood, remembers what it is told. */
class FakeRadio {
  freq = 14_074_000;
  mode = '2';
  written: string[] = [];
  answering = true;
  private push: ((bytes: Uint8Array) => void) | null = null;
  private end: (() => void) | null = null;
  readable = new ReadableStream<Uint8Array>({
    start: (controller) => {
      this.push = (bytes) => controller.enqueue(bytes);
      this.end = () => controller.close();
    },
  });
  writable = new WritableStream<Uint8Array>({
    write: (chunk) => {
      const command = new TextDecoder().decode(chunk);
      this.written.push(command);
      if (command.startsWith('FA') && command.length > 3) this.freq = Number(command.slice(2, -1));
      else if (command.startsWith('MD') && command.length > 3) this.mode = command.slice(2, -1);
      else if (this.answering) {
        const answer = command === 'FA;' ? `FA${String(this.freq).padStart(11, '0')};` : `MD${this.mode};`;
        queueMicrotask(() => this.push?.(new TextEncoder().encode(answer)));
      }
    },
  });
  opened: { baudRate: number; stopBits?: number } | null = null;
  signals: Record<string, boolean>[] = [];
  async open(options: { baudRate: number; stopBits?: number }) {
    this.opened = options;
  }
  async setSignals(signals: Record<string, boolean>) {
    this.signals.push(signals);
  }
  async close() {}
  unplug() {
    this.end?.();
  }
}

function receiver(start = { freq: 7_074_000, mode: 'usb' }) {
  const state = { ...start, covers: (f: number) => f < 30e6 };
  const listeners = new Set<() => void>();
  const access: ReceiverAccess = {
    current: () => ({ freq: state.freq, mode: state.mode }),
    tune: (freq, mode) => {
      if (!state.covers(freq)) return false;
      state.freq = freq;
      if (mode) state.mode = mode;
      listeners.forEach((l) => l());
      return true;
    },
    watch: (changed) => {
      listeners.add(changed);
      return () => listeners.delete(changed);
    },
  };
  const move = (freq: number, mode?: string) => {
    state.freq = freq;
    if (mode) state.mode = mode;
    listeners.forEach((l) => l());
  };
  return { state, access, move };
}

const settings = (direction: RigSettings['direction']): RigSettings => ({ driver: 'kenwood', baud: 38400, address: 0x94, direction });

// Fake timers move Date.now() with the timers they run, so the link's clock
// and its intervals agree inside every step.
const now = () => Date.now();
beforeEach(() => {
  vi.useFakeTimers();
});
afterEach(() => vi.useRealTimers());
const advance = (ms: number) => vi.advanceTimersByTimeAsync(ms);

describe('rig link', () => {
  it('follows the radio', async () => {
    const radio = new FakeRadio();
    const rx = receiver();
    const link = new RigLink(rx.access, { requestPort: async () => radio }, now);
    await link.connect(settings('follow'));
    await advance(450);
    expect(rx.state).toMatchObject({ freq: 14_074_000, mode: 'usb' });
    radio.freq = 7_030_000;
    radio.mode = '3';
    await advance(450);
    expect(rx.state).toMatchObject({ freq: 7_030_000, mode: 'cw' });
    // Following only: tuning the receiver leaves the radio alone.
    rx.move(7_040_000);
    await advance(150);
    expect(radio.written.filter((c) => c.length > 4)).toEqual([]);
    await link.disconnect();
    expect(rigState.value.status).toBe('off');
  });

  it('takes the radio\'s mode at connect in both directions, though it answers after the frequency', async () => {
    const radio = new FakeRadio();
    radio.freq = 7_070_000;
    radio.mode = '1';
    const rx = receiver({ freq: 7_040_000, mode: 'am' });
    const link = new RigLink(rx.access, { requestPort: async () => radio }, now);
    await link.connect(settings('both'));
    await advance(450);
    expect(rx.state).toMatchObject({ freq: 7_070_000, mode: 'lsb' });
    expect(radio.written.filter((c) => c.length > 4)).toEqual([]);
    await link.disconnect();
  });

  it('keeps SAM when the radio says AM, and says when the radio is outside the receiver', async () => {
    const radio = new FakeRadio();
    radio.freq = 7_200_000;
    radio.mode = '5';
    const rx = receiver({ freq: 7_200_000, mode: 'sam' });
    const link = new RigLink(rx.access, { requestPort: async () => radio }, now);
    await link.connect(settings('follow'));
    await advance(450);
    expect(rx.state.mode).toBe('sam');
    radio.freq = 50_313_000;
    await advance(450);
    expect(rx.state.freq).toBe(7_200_000);
    expect(rigState.value.message).toContain('50.3130 MHz');
    await link.disconnect();
  });

  it('controls the radio without being pulled back by stale answers, in both directions', async () => {
    const radio = new FakeRadio();
    const rx = receiver();
    const link = new RigLink(rx.access, { requestPort: async () => radio }, now);
    await link.connect(settings('both'));
    await advance(450);
    expect(rx.state.freq).toBe(14_074_000);
    // The listener drags across the waterfall: several changes, one write each settle.
    for (const f of [14_075_000, 14_076_000, 14_077_000]) {
      rx.move(f);
      await advance(30);
    }
    await advance(1000);
    expect(radio.freq).toBe(14_077_000);
    expect(rx.state.freq).toBe(14_077_000);
    expect(radio.written.filter((c) => /^FA\d/.test(c))).toEqual(['FA00014077000;']);
    // And the dial on the radio still moves the receiver.
    radio.freq = 14_080_000;
    await advance(450);
    expect(rx.state.freq).toBe(14_080_000);
    await link.disconnect();
  });

  it('follows a radio knob turned steadily, in both directions, without lagging', async () => {
    const radio = new FakeRadio();
    const rx = receiver({ freq: 14_074_000, mode: 'usb' });
    const link = new RigLink(rx.access, { requestPort: async () => radio }, now);
    await link.connect(settings('both'));
    await advance(450);
    for (let step = 1; step <= 5; step++) {
      radio.freq = 14_074_000 + step * 100;
      await advance(250);
      expect(rx.state.freq).toBe(radio.freq);
    }
    expect(radio.written.filter((c) => /^FA\d/.test(c))).toEqual([]);
    await link.disconnect();
  });

  it('says when the radio does not answer, and when the port goes away', async () => {
    const radio = new FakeRadio();
    radio.answering = false;
    const link = new RigLink(receiver().access, { requestPort: async () => radio }, now);
    await link.connect(settings('follow'));
    await advance(3000);
    expect(rigState.value.message).toContain('No answer from the radio');
    radio.unplug();
    await advance(10);
    expect(rigState.value).toMatchObject({ status: 'error' });
    expect(rigState.value.message).toContain('lost');
    expect(link.connected).toBe(false);
  });

  it('lowers DTR and RTS as soon as the port is open', async () => {
    const radio = new FakeRadio();
    const link = new RigLink(receiver().access, { requestPort: async () => radio }, now);
    await link.connect(settings('follow'));
    expect(radio.signals[0]).toEqual({ dataTerminalReady: false, requestToSend: false });
    expect(radio.opened).toEqual({ baudRate: 38400, stopBits: 1 });
    await link.disconnect();
  });

  it('keeps FT-817 commands 250 ms apart, a frequency set between polls included', async () => {
    const times: number[] = [];
    const port = {
      readable: new ReadableStream<Uint8Array>(),
      writable: new WritableStream<Uint8Array>({ write: () => void times.push(Date.now()) }),
      opened: null as unknown,
      async open(options: unknown) { this.opened = options; },
      async setSignals() {},
      async close() {},
    };
    const rx = receiver();
    const link = new RigLink(rx.access, { requestPort: async () => port }, now);
    await link.connect({ driver: 'yaesu-classic', baud: 4800, address: 0x94, direction: 'control' });
    expect(port.opened).toEqual({ baudRate: 4800, stopBits: 2 });
    for (let i = 0; i < 20; i++) {
      if (i % 5 === 0) rx.move(7_000_000 + i * 1000, i % 10 === 0 ? 'usb' : 'lsb');
      await advance(100);
    }
    expect(times.length).toBeGreaterThan(5);
    for (let i = 1; i < times.length; i++) expect(times[i] - times[i - 1]).toBeGreaterThanOrEqual(250);
    await link.disconnect();
  });

  it('explains a browser without Web Serial, and treats a closed chooser as nothing', async () => {
    await new RigLink(receiver().access, null, now).connect(settings('follow'));
    expect(rigState.value.message).toContain('Chrome, Edge or Opera');
    await new RigLink(receiver().access, { requestPort: async () => { throw new Error('cancelled'); } }, now).connect(settings('follow'));
    expect(rigState.value).toMatchObject({ status: 'off', message: '' });
  });
});
