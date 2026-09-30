/**
 * The link between this receiver and the listener's own radio, over the
 * radio's CAT port through Web Serial. It lives outside any component, so
 * closing the panel does not drop it; loaded with the panel, so a listener
 * who never opens it never downloads it.
 *
 * Sync in both directions without the two fighting: the radio is asked five
 * times a second, and a change on either side is copied to the other unless
 * the two already agree to within the radio's dial step. While the listener
 * tunes and briefly after the write to the radio, the radio's answers are
 * not followed: an answer to a poll sent before the write still carries the
 * old frequency and would pull the receiver back.
 */
import { DRIVERS, type DriverId, type ReceiverMode, type RigReport, type RigSession } from './drivers';
import { rigState } from './state';

export { rigState };

export type RigDirection = 'follow' | 'control' | 'both';

export interface RigSettings {
  driver: DriverId;
  baud: number;
  /** CI-V address, Icom only. */
  address: number;
  direction: RigDirection;
}

/** What the link needs from the receiver; the panel hands in the real one. */
export interface ReceiverAccess {
  current(): { freq: number; mode: string };
  /** False when no band of this receiver covers the frequency. */
  tune(freq: number, mode: ReceiverMode | undefined): boolean;
  /** Calls back on every tuning change; returns a stop function. */
  watch(changed: () => void): () => void;
}

/** The parts of Web Serial used here, which TypeScript's DOM types lack. */
interface SerialPortLike {
  open(options: { baudRate: number; stopBits?: number }): Promise<void>;
  setSignals?(signals: { dataTerminalReady?: boolean; requestToSend?: boolean }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
}
interface SerialLike {
  requestPort(): Promise<SerialPortLike>;
}

const POLL_MS = 200;
const WRITE_DEBOUNCE_MS = 100;
const QUIET_AFTER_WRITE_MS = 600;
const SILENT_AFTER_MS = 2500;
const SETTINGS_KEY = 'fernsdr.rig.v1';
const NO_SUCH_MODE = 'The radio cannot be set to';

export const BAUD_RATES = [4800, 9600, 19200, 38400, 57600, 115200] as const;

export function serialAvailable(): boolean {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

export function loadRigSettings(): RigSettings {
  const fallback: RigSettings = { driver: 'kenwood', baud: 38400, address: 0x94, direction: 'follow' };
  try {
    const v = JSON.parse(localStorage.getItem(SETTINGS_KEY) ?? 'null');
    if (!v || typeof v !== 'object') return fallback;
    return {
      driver: DRIVERS.some((d) => d.id === v.driver) ? v.driver : fallback.driver,
      baud: (BAUD_RATES as readonly number[]).includes(v.baud) ? v.baud : fallback.baud,
      address: Number.isInteger(v.address) && v.address > 0 && v.address < 0xe0 ? v.address : fallback.address,
      direction: ['follow', 'control', 'both'].includes(v.direction) ? v.direction : fallback.direction,
    };
  } catch {
    return fallback;
  }
}

export function saveRigSettings(settings: RigSettings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  } catch {
    // Chosen again next time.
  }
}

/** The receiver's mode is kept when it is a variant of what the radio says (SAM for AM). */
function sameMode(receiver: string, rig: ReceiverMode): boolean {
  if (rig === 'am') return receiver === 'am' || receiver === 'sam' || receiver === 'dsb';
  return receiver === rig;
}

export class RigLink {
  private port: SerialPortLike | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private session: RigSession | null = null;
  private settings: RigSettings | null = null;
  private resolution = 1;
  private timers: ReturnType<typeof setInterval>[] = [];
  private debounce: ReturnType<typeof setTimeout> | null = null;
  private stopWatching: (() => void) | null = null;
  private writing = false;
  private commandGapMs = 0;
  private queued: Uint8Array[] | null = null;
  private quietUntil = 0;
  private lastAnswer = 0;
  private closing = false;
  /**
   * Counts connection attempts and disconnects. A connect waiting on the
   * port chooser or on the port opening checks it after each wait: the
   * listener may have pressed Disconnect meanwhile, and the radio must then
   * stay unlinked rather than come alive once the port answers.
   */
  private attempt = 0;

  constructor(
    private readonly receiver: ReceiverAccess,
    private readonly serial: SerialLike | null = serialAvailable() ? ((navigator as unknown as { serial: SerialLike }).serial) : null,
    private readonly now: () => number = () => Date.now(),
  ) {}

  get connected(): boolean {
    return this.port !== null;
  }

  /** Must be called from the click that asks for it: the browser shows its port chooser. */
  async connect(settings: RigSettings): Promise<void> {
    if (this.port) await this.disconnect();
    const attempt = ++this.attempt;
    const stale = () => attempt !== this.attempt;
    if (!this.serial) {
      this.fail('This browser cannot reach a serial port. Chrome, Edge or Opera on a computer can.');
      return;
    }
    rigState.value = { status: 'connecting', message: 'Choose the radio’s port.', freq: null, mode: null };
    let port: SerialPortLike;
    try {
      port = await this.serial.requestPort();
    } catch {
      // The listener closed the chooser: not an error worth a red line.
      if (!stale()) rigState.value = { status: 'off', message: '', freq: null, mode: null };
      return;
    }
    if (stale()) return;
    const driver = DRIVERS.find((d) => d.id === settings.driver) ?? DRIVERS[0];
    try {
      await port.open({ baudRate: settings.baud, stopBits: driver.stopBits });
    } catch {
      if (!stale()) this.fail('The port would not open. Another program may be using it: close its CAT connection and try again.');
      return;
    }
    // Many shacks key the transmitter, or CW, from the port's DTR or RTS
    // line, and opening a port raises both. Down again at once: the browser
    // gives no way to open with them low, so a radio set up that way still
    // sees a pulse of a few milliseconds, which the panel warns about.
    try {
      await port.setSignals?.({ dataTerminalReady: false, requestToSend: false });
    } catch {
      // Some adapters have no modem lines to set.
    }
    if (stale()) {
      try {
        await port.close();
      } catch {
        // Unplugged meanwhile: nothing left to close.
      }
      return;
    }
    this.port = port;
    this.settings = settings;
    this.resolution = driver.resolutionHz;
    this.session = driver.create(settings.address);
    this.writer = port.writable?.getWriter() ?? null;
    this.reader = port.readable?.getReader() ?? null;
    this.closing = false;
    this.lastAnswer = this.now();
    rigState.value = { status: 'on', message: '', freq: null, mode: null };

    void this.readLoop();
    this.commandGapMs = driver.commandGapMs;
    this.timers.push(setInterval(() => this.poll(), Math.max(POLL_MS, driver.commandGapMs + 50)));
    if (settings.direction !== 'follow') {
      this.stopWatching = this.receiver.watch(() => this.scheduleWrite());
      // Controlling only: the radio starts where the receiver is.
      if (settings.direction === 'control') this.scheduleWrite();
    }
  }

  async disconnect(message = ''): Promise<void> {
    this.attempt++;
    this.closing = true;
    for (const timer of this.timers) clearInterval(timer);
    this.timers = [];
    if (this.debounce) clearTimeout(this.debounce);
    this.debounce = null;
    this.stopWatching?.();
    this.stopWatching = null;
    const { port, reader, writer } = this;
    this.port = this.reader = this.writer = null;
    this.session = null;
    this.queued = null;
    try {
      await reader?.cancel();
    } catch {
      // Already gone with the device.
    }
    reader?.releaseLock();
    writer?.releaseLock();
    try {
      await port?.close();
    } catch {
      // Unplugged: nothing left to close.
    }
    rigState.value = { status: message ? 'error' : 'off', message, freq: null, mode: null };
  }

  private fail(message: string): void {
    rigState.value = { status: 'error', message, freq: null, mode: null };
  }

  private async readLoop(): Promise<void> {
    const reader = this.reader;
    if (!reader) return;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && this.session) this.handle(this.session.parser.feed(value));
      }
    } catch {
      // Fall through: the port is gone.
    }
    if (!this.closing) void this.disconnect('The connection to the radio was lost. Is the cable still in?');
  }

  /**
   * One write at a time. A poll finding the port busy is skipped (the next
   * comes 200 ms later); a setting is kept and sent after, the latest one
   * replacing any still waiting.
   */
  private async write(bytes: Uint8Array[], setting = false): Promise<void> {
    if (!this.writer) return;
    if (this.writing) {
      if (setting) this.queued = bytes;
      return;
    }
    this.writing = true;
    try {
      for (const chunk of bytes) {
        await this.writer.write(chunk);
        if (this.commandGapMs > 0) await new Promise((resolve) => setTimeout(resolve, this.commandGapMs));
      }
    } catch {
      // The read loop notices a lost port and says so.
    } finally {
      this.writing = false;
    }
    const next = this.queued;
    this.queued = null;
    if (next) await this.write(next, true);
  }

  private poll(): void {
    if (!this.session) return;
    if (this.now() - this.lastAnswer > SILENT_AFTER_MS) {
      const hint = this.settings?.driver === 'icom'
        ? 'check the baud rate, the CI-V address, and CI-V Transceive in the radio’s menu'
        : 'check the baud rate and that the radio’s CAT port is enabled';
      rigState.value = { ...rigState.value, message: `No answer from the radio: ${hint}.` };
    }
    this.session.beforePoll?.();
    void this.write(this.session.poll());
  }

  private handle(reports: RigReport[]): void {
    if (reports.length === 0) return;
    this.lastAnswer = this.now();
    let { freq, mode } = rigState.value;
    for (const report of reports) {
      freq = report.freq ?? freq;
      mode = report.mode ?? mode;
    }
    const wasSilent = rigState.value.message.startsWith('No answer');
    rigState.value = { ...rigState.value, freq, mode, message: wasSilent ? '' : rigState.value.message };
    if (this.settings?.direction === 'control' || freq === null || this.now() < this.quietUntil) return;

    const here = this.receiver.current();
    const moved = Math.abs(freq - here.freq) >= this.resolution;
    const changedMode = mode !== null && !sameMode(here.mode, mode);
    if (!moved && !changedMode) return;
    const tuned = this.receiver.tune(freq, changedMode && mode ? mode : undefined);
    rigState.value = {
      ...rigState.value,
      message: tuned ? '' : `The radio is on ${(freq / 1e6).toFixed(4)} MHz, which this receiver does not cover.`,
    };
  }

  private scheduleWrite(): void {
    // The receiver moving because the radio did: nothing to send, and no
    // reason to stop listening to a knob that is still turning.
    const here = this.receiver.current();
    const rig = rigState.value;
    // A mode the radio has not reported yet is no difference: its answer is
    // on the way, and quieting now would ignore it.
    if (rig.freq !== null && Math.abs(here.freq - rig.freq) < this.resolution && (rig.mode === null || sameMode(here.mode, rig.mode))) {
      if (rig.message.startsWith(NO_SUCH_MODE)) rigState.value = { ...rig, message: '' };
      return;
    }
    if (this.debounce) clearTimeout(this.debounce);
    // From the listener's first move, not from the write: answers to polls
    // in between still carry the radio's old frequency.
    this.quietUntil = this.now() + WRITE_DEBOUNCE_MS + QUIET_AFTER_WRITE_MS;
    this.debounce = setTimeout(() => {
      this.debounce = null;
      const session = this.session;
      if (!session) return;
      const here = this.receiver.current();
      const rig = rigState.value;
      const chunks: Uint8Array[] = [];
      if (rig.freq === null || Math.abs(here.freq - rig.freq) >= this.resolution) chunks.push(session.setFrequency(here.freq));
      // The receiver has modes a radio's CAT does not (WFM on every driver
      // here): those send no mode at all, since the table's gap would
      // otherwise go out as a malformed command or as LSB.
      const mode = here.mode as ReceiverMode;
      let modeSent = false;
      let message = rig.message.startsWith(NO_SUCH_MODE) ? '' : rig.message;
      if (rig.mode === null || !sameMode(mode, rig.mode)) {
        const command = session.setMode(mode);
        if (command) {
          chunks.push(command);
          modeSent = true;
        } else message = `${NO_SUCH_MODE} ${mode.toUpperCase()} over CAT; it stays in its own mode.`;
      }
      if (message !== rig.message) rigState.value = { ...rigState.value, message };
      if (chunks.length === 0) return;
      this.quietUntil = this.now() + QUIET_AFTER_WRITE_MS;
      rigState.value = { ...rigState.value, freq: here.freq, mode: modeSent ? mode : rig.mode };
      void this.write(chunks, true);
    }, WRITE_DEBOUNCE_MS);
  }
}
