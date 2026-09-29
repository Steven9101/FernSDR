/**
 * WebSocket client.
 *
 * Reconnects with backoff and replays the user's settings on the way back in,
 * because a dropped connection should cost a second of audio, not the user's
 * place on the band.
 */
import { parseBinary, type ServerBinary, type ServerMessage } from './protocol';

export type ConnectionState = 'connecting' | 'open' | 'reconnecting' | 'closed';

export interface StreamTraffic {
  audioBps: number;
  waterfallBps: number;
  controlBps: number;
  totalBps: number;
  waterfallFps: number;
}

export interface ClientHandlers {
  onState(state: ConnectionState, detail?: string): void;
  onMessage(message: ServerMessage): void;
  onBinary(packet: ServerBinary): void;
  /** Called after every (re)connection, to restore what the user had set. */
  onReady(): void;
  onTraffic?(traffic: StreamTraffic): void;
  /** The receiver closed the connection for inactivity (CLOSE_INACTIVE). */
  onInactive?(reason: string): void;
}

const BASE_RECONNECT_DELAY_MS = 500;
const MAX_RECONNECT_DELAY_MS = 15_000;
const PING_INTERVAL_MS = 5_000;
/**
 * A stream that has stopped arriving while the socket is still open.
 *
 * `onclose` covers a connection that dropped, and it reconnects. It does not
 * cover the case a listener actually hits: the socket stays open, nothing
 * arrives, and the page sits there looking alive. That is why people reload,
 * which on a phone is the pull-to-refresh gesture that used to lose their
 * frequency. Reconnecting for them removes the reason for the gesture rather
 * than suppressing the gesture.
 *
 * Twelve seconds is well past any hiccup this client already conceals, and
 * short enough that nobody has decided the receiver is broken.
 */
const STALL_TIMEOUT_MS = 12_000;

/**
 * The receiver let this listener's place go after its listener timeout. Not
 * reconnected by itself: that would take the place straight back and make
 * the operator's limit meaningless. The page offers to listen again.
 */
export const CLOSE_INACTIVE = 4001;

export class SdrClient {
  private socket: WebSocket | null = null;
  private reconnectAttempts = 0;
  private reconnectTimer: number | null = null;
  private pingTimer: number | null = null;
  private closedByUser = false;
  private lastPingSent = 0;
  private lastArrivalMs = 0;
  private stallTimer: number | null = null;
  private trafficStartMs = 0;
  private trafficBytes = [0, 0, 0];
  private trafficRows = 0;
  private textEncoder = new TextEncoder();

  /** Round-trip time of the last ping, in milliseconds. */
  latencyMs = 0;

  constructor(private url: string, private readonly handlers: ClientHandlers) {}

  /** Set before connecting; lets the URL be resolved when a DOM exists. */
  setUrl(url: string): void {
    this.url = url;
  }

  connect(): void {
    if (this.socket) return;
    this.closedByUser = false;
    this.open();
  }

  close(): void {
    this.closedByUser = true;
    this.clearTimers();
    const socket = this.socket;
    this.socket = null;
    socket?.close();
    this.handlers.onState('closed');
  }

  get connected(): boolean {
    return this.socket?.readyState === WebSocket.OPEN;
  }

  send(message: Record<string, unknown>): void {
    if (!this.connected) return;
    this.socket!.send(JSON.stringify(message));
  }

  private open(): void {
    this.clearTimers();
    this.handlers.onState(this.reconnectAttempts === 0 ? 'connecting' : 'reconnecting');

    let socket: WebSocket;
    try {
      socket = new WebSocket(this.url);
    } catch (error) {
      this.scheduleReconnect(String(error));
      return;
    }
    socket.binaryType = 'arraybuffer';
    this.socket = socket;

    socket.onopen = () => {
      if (this.socket !== socket) return;
      this.handlers.onState('open');
      this.trafficStartMs = performance.now();
      this.trafficBytes.fill(0);
      this.trafficRows = 0;
      this.handlers.onReady();
      this.pingTimer = window.setInterval(() => this.ping(), PING_INTERVAL_MS);
      this.lastArrivalMs = performance.now();
      this.watchForStall();
    };

    socket.onmessage = (event) => {
      if (this.socket !== socket) return;
      this.lastArrivalMs = performance.now();
      this.measureTraffic(event.data, this.lastArrivalMs);
      if (typeof event.data === 'string') {
        let message: ServerMessage;
        try {
          message = JSON.parse(event.data) as ServerMessage;
        } catch {
          return; // a malformed control message is not worth dropping the stream for
        }
        if (!message || typeof message !== 'object' || typeof message.type !== 'string') return;
        if (message.type === 'welcome') this.reconnectAttempts = 0;
        if (message.type === 'pong') {
          if (typeof message.t === 'number' && message.t === Math.round(this.lastPingSent)) {
            this.latencyMs = Math.max(0, Math.round(performance.now() - this.lastPingSent));
          }
          return;
        }
        this.handlers.onMessage(message);
        return;
      }
      this.lastArrivalMs = performance.now();
      const packet = parseBinary(event.data as ArrayBuffer);
      if (packet) this.handlers.onBinary(packet);
    };

    socket.onclose = (event) => {
      if (this.socket !== socket) return;
      this.socket = null;
      if (this.stallTimer !== null) window.clearInterval(this.stallTimer);
      this.stallTimer = null;
      if (this.closedByUser) return;
      if (event.code === CLOSE_INACTIVE) {
        this.clearTimers();
        this.closedByUser = true;
        this.handlers.onState('closed', event.reason || 'no activity');
        this.handlers.onInactive?.(event.reason || 'no activity');
        return;
      }
      this.scheduleReconnect(event.reason || `closed (${event.code})`);
    };

    socket.onerror = () => {
      // onclose always follows; reconnection is handled there.
    };
  }

  private measureTraffic(data: string | ArrayBuffer, now: number): void {
    const text = typeof data === 'string';
    const bytes = text ? this.textEncoder.encode(data).byteLength : data.byteLength;
    const stream = text || bytes === 0 ? 0 : new Uint8Array(data, 0, 1)[0];
    const slot = stream === 1 ? 0 : stream === 2 ? 1 : 2;
    // The receiver emits one unmasked WebSocket frame per message. Browser
    // events expose its payload, so include that frame's length header here.
    // TCP, TLS and retransmissions are not visible to this API.
    this.trafficBytes[slot] += bytes + (bytes < 126 ? 2 : bytes <= 65535 ? 4 : 10);
    if (slot === 1) this.trafficRows++;
    this.reportTraffic(now);
  }

  private reportTraffic(now: number): void {
    const elapsed = now - this.trafficStartMs;
    if (elapsed < 1000) return;
    const [audioBps, waterfallBps, controlBps] = this.trafficBytes.map(count => count * 8000 / elapsed);
    this.handlers.onTraffic?.({ audioBps, waterfallBps, controlBps,
      totalBps: audioBps + waterfallBps + controlBps, waterfallFps: this.trafficRows * 1000 / elapsed });
    this.trafficStartMs = now;
    this.trafficBytes.fill(0);
    this.trafficRows = 0;
  }

  /**
   * Watches for a stream that has stopped without the socket noticing.
   *
   * The deadline starts at connection, including servers that accept a
   * socket but never send their welcome. Heartbeat replies count as activity
   * when the listener has deliberately disabled both media streams.
   */
  private watchForStall(): void {
    if (this.stallTimer !== null) window.clearInterval(this.stallTimer);
    this.stallTimer = window.setInterval(() => {
      if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
      const now = performance.now();
      this.reportTraffic(now);
      if (now - this.lastArrivalMs < STALL_TIMEOUT_MS) return;
      // A dead link may never complete the close handshake. Retire it before
      // reconnecting, and ignore any callbacks it delivers later.
      const socket = this.socket;
      this.socket = null;
      socket.close(4000, 'stream stalled');
      this.scheduleReconnect('stream stalled');
    }, 2000);
  }

  private ping(): void {
    this.lastPingSent = performance.now();
    this.send({ type: 'ping', t: Math.round(this.lastPingSent) });
  }

  private scheduleReconnect(detail: string): void {
    this.clearTimers();
    // Exponential backoff with jitter, so a receiver coming back up does not
    // get hit by every client it dropped at the same instant.
    const delay = Math.min(
      MAX_RECONNECT_DELAY_MS,
      BASE_RECONNECT_DELAY_MS * 2 ** this.reconnectAttempts,
    );
    const jittered = delay * (0.7 + Math.random() * 0.6);
    this.reconnectAttempts++;
    this.handlers.onState('reconnecting', detail);
    this.reconnectTimer = window.setTimeout(() => this.open(), jittered);
  }

  private clearTimers(): void {
    if (this.reconnectTimer !== null) window.clearTimeout(this.reconnectTimer);
    if (this.pingTimer !== null) window.clearInterval(this.pingTimer);
    if (this.stallTimer !== null) window.clearInterval(this.stallTimer);
    this.reconnectTimer = null;
    this.pingTimer = null;
    this.stallTimer = null;
  }
}

/** Builds the WebSocket URL for the page's own origin. */
export function defaultWebSocketUrl(): string {
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${location.host}/ws`;
}
