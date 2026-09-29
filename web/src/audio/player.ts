/**
 * Owns the AudioContext and the worklet that plays the stream.
 *
 * Browsers will not start audio without a user gesture, so the context is
 * created on demand and the UI has to show an explicit control. That is a
 * platform rule, not a preference - the alternative is a page that looks like
 * it is working and is silent.
 */
import workletUrl from './nac-worklet.ts?worker&url';
import type { NacPlaybackEngine, PlaybackMessage } from './playback-engine';
import type { AudioPacket } from '../net/protocol';

/**
 * `AudioWorklet` exists only in a secure context. A receiver served over plain
 * HTTP - the default - has no `audioWorklet` at all, and reaching for it threw
 * a TypeError that surfaced as "Audio could not start". A ScriptProcessorNode
 * is deprecated and runs on the main thread, but it exists everywhere, and it
 * drives the same `NacPlaybackEngine` the worklet does. HTTP listeners get
 * audio; HTTPS listeners still get it off the audio thread.
 */
/**
 * 2048 frames is ~43 ms at 48 kHz. The worklet can afford a short block because
 * it owns the audio thread; this path shares the main thread with layout and
 * the waterfall, and a block short enough to be overrun by one repaint just
 * turns every repaint into a dropout. The block is also latency, once in the
 * engine's buffer and once in the node's output: 4096 put about 170 ms of it
 * on every listener of a plain-HTTP receiver, which is the default install.
 * In the benchmark lab 2048 took the marker latency from 320 to 275 ms with no
 * dropout and the waterfall at its full 12 rows a second.
 */
const FALLBACK_BLOCK_FRAMES = 2048;

/**
 * Chrome and WebKit do not reject `AudioContext.resume()` when the gesture is
 * not accepted: the promise simply never settles. Awaiting it unbounded left
 * `start()` parked forever in 'starting', and the first line of `start()` bails
 * out while it is 'starting' - so every later press of the button returned
 * immediately and the gate sat there doing nothing.
 */
const RESUME_TIMEOUT_MS = 1_200;

function withTimeout<T>(work: Promise<T>, ms: number): Promise<T | 'timeout'> {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      resolve('timeout');
    }, ms);
    work.then(
      (value) => { if (!settled) { settled = true; clearTimeout(timer); resolve(value); } },
      (error) => { if (!settled) { settled = true; clearTimeout(timer); reject(error); } },
    );
  });
}

export interface PlaybackStats {
  /** Audio buffered ahead of the play head, in milliseconds. */
  latencyMs: number;
  /** What the jitter policy is currently aiming to buffer. */
  targetMs: number;
  /** Closest the buffer came to empty recently: the real headroom figure. */
  marginMs: number;
  /** Clock correction currently applied, in parts per million. */
  driftPpm: number;
  underruns: number;
  /** Times the backlog was past playing out and was skipped in one step. */
  resyncs: number;
  discontinuities: number;
  concealedFrames: number;
  decodedFrames: number;
}

export type PlayerState = 'idle' | 'starting' | 'running' | 'suspended' | 'failed';

export class AudioPlayer {
  private context: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  /** Set instead of `node` when the context has no worklet. */
  private fallback: { processor: ScriptProcessorNode; engine: NacPlaybackEngine } | null = null;
  private gainNode: GainNode | null = null;
  /** The listener's bass and treble, between the decoder and the volume. */
  private bassNode: BiquadFilterNode | null = null;
  private trebleNode: BiquadFilterNode | null = null;
  private initialising: Promise<void> | null = null;
  private pendingConfig: { rate: number; generation: number } | null = null;

  state: PlayerState = 'idle';
  /**
   * Why audio is not playing, in the listener's own browser. "Nothing happens
   * when I press start" is the single least actionable bug report a receiver
   * can generate, and the facts that settle it - is this a secure context, did
   * the context reach running, which output path was taken - are all knowable
   * on the spot.
   */
  lastError: string | null = null;
  stats: PlaybackStats = {
    latencyMs: 0,
    targetMs: 0,
    marginMs: 0,
    driftPpm: 0,
    underruns: 0,
    resyncs: 0,
    discontinuities: 0,
    concealedFrames: 0,
    decodedFrames: 0,
  };

  onStateChange: ((state: PlayerState) => void) | null = null;
  onStats: ((stats: PlaybackStats) => void) | null = null;

  private volume = 0.8;
  private muted = false;
  private bassDb = 0;
  private trebleDb = 0;
  private targetLatency = 0.09;

  get contextRate(): number {
    return this.context?.sampleRate ?? 0;
  }

  /** A one-line summary of the output path, for the gate to show when stuck. */
  get diagnostics(): string {
    const secure = typeof window !== 'undefined' && window.isSecureContext ? 'https' : 'http';
    const path = this.node ? 'worklet' : this.fallback ? 'fallback' : 'no output';
    const ctx = this.context ? this.context.state : 'none';
    const rate = this.context ? `${Math.round(this.context.sampleRate / 100) / 10}k` : '-';
    return `${secure} · ${path} · context ${ctx} · ${rate}${this.lastError ? ` · ${this.lastError}` : ''}`;
  }

  /** Must be called from a user gesture handler. */
  async start(): Promise<void> {
    if (this.state === 'running') return;
    this.setState('starting');
    let context = this.context;

    try {
      if (!this.context) {
        this.context = new AudioContext({ latencyHint: 'interactive' });
        this.context.onstatechange = () => {
          if (this.context?.state === 'running' && (this.node || this.fallback)) {
            this.lastError = null;
            this.setState('running');
          } else if (this.state === 'running') {
            this.setState('suspended');
          }
        };
      }
      context = this.context;
      // resume() must run in the gesture, before loading a worklet or a module
      // yields to the event loop. Repeated taps share setup but each supplies
      // a fresh gesture to the browser.
      const resumed = withTimeout(context.resume(), RESUME_TIMEOUT_MS);
      this.initialising ??= this.initialiseOutput(context);
      const [, outcome] = await Promise.all([this.initialising, resumed]);
      if (context !== this.context) return;
      if (context.state === 'running') {
        this.lastError = null;
        this.setState('running');
      } else {
        this.lastError = outcome === 'timeout'
          ? 'the browser did not accept the tap; press again'
          : `the browser left the audio device ${context.state}`;
        this.setState('suspended');
      }
    } catch (error) {
      if (context !== this.context) return;
      this.node?.disconnect();
      this.fallback?.processor.disconnect();
      this.gainNode?.disconnect();
      this.bassNode?.disconnect();
      this.trebleNode?.disconnect();
      this.bassNode = null;
      this.trebleNode = null;
      this.node = null;
      this.fallback = null;
      this.gainNode = null;
      this.context = null;
      this.initialising = null;
      if (context) {
        context.onstatechange = null;
        void context.close().catch(() => {});
      }
      this.lastError = error instanceof Error ? error.message : String(error);
      this.setState('failed');
    }
  }

  private async initialiseOutput(context: AudioContext): Promise<void> {
    this.gainNode = context.createGain();
    // Shelves rather than peaks: "more bass" on AM means everything under the
    // voice, not one bump. At 0 dB a shelf passes the signal unchanged.
    this.bassNode = context.createBiquadFilter();
    this.bassNode.type = 'lowshelf';
    this.bassNode.frequency.value = 300;
    this.trebleNode = context.createBiquadFilter();
    this.trebleNode.type = 'highshelf';
    this.trebleNode.frequency.value = 1800;
    this.bassNode.connect(this.trebleNode);
    this.trebleNode.connect(this.gainNode);
    if (context.audioWorklet) {
      await context.audioWorklet.addModule(workletUrl);
      if (context !== this.context) return;
      this.node = new AudioWorkletNode(context, 'nac-playback', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      this.node.port.onmessage = (event) => {
        if (event.data?.type === 'stats') {
          this.stats = event.data as PlaybackStats;
          this.onStats?.(this.stats);
        }
      };
      this.node.connect(this.bassNode);
    } else {
      // The decoder is already in the worklet bundle. Only HTTP browsers need
      // a second copy on the main thread, so load it when that path is used.
      const { NacPlaybackEngine } = await import('./playback-engine');
      if (context !== this.context) return;
      const engine = new NacPlaybackEngine(context.sampleRate, (stats) => {
        this.stats = stats as unknown as PlaybackStats;
        this.onStats?.(this.stats);
      });
      // WebKit does not reliably schedule a ScriptProcessor with zero inputs.
      // The unconnected input makes it participate in the audio graph.
      const processor = context.createScriptProcessor(FALLBACK_BLOCK_FRAMES, 1, 1);
      processor.onaudioprocess = (event) => engine.render(event.outputBuffer.getChannelData(0));
      this.fallback = { processor, engine };
      processor.connect(this.bassNode);
    }
    this.gainNode.connect(context.destination);
    this.applyTone();
    this.applyGain();
    this.post({ type: 'target-latency', value: this.targetLatency });
    if (this.pendingConfig) this.post({ type: 'config', ...this.pendingConfig });
  }

  async suspend(): Promise<void> {
    if (!this.context) return;
    await this.context.suspend();
    this.setState('suspended');
  }

  /** Tells the worklet what rate the incoming frames are at. */
  configure(rate: number, generation: number): void {
    this.pendingConfig = { rate, generation };
    this.post({ type: 'config', rate, generation });
  }

  /** Hands a frame to the audio thread. Cheap; safe to call at frame rate. */
  feed(packet: AudioPacket): void {
    if (this.fallback) {
      this.fallback.engine.handle({
        type: 'packet',
        compact: packet.compact,
        packet: packet.packet,
        discontinuity: packet.discontinuity,
        sequence: packet.sequence,
        muted: packet.muted,
        generation: packet.generation,
        data: packet.payload.slice().buffer,
      });
      return;
    }
    if (!this.node) return;
    // Copy out of the receive buffer so the transfer does not detach a view
    // the caller still holds.
    const data = packet.payload.slice().buffer;
    this.node.port.postMessage(
      {
        type: 'packet',
        compact: packet.compact,
        packet: packet.packet,
        discontinuity: packet.discontinuity,
        sequence: packet.sequence,
        muted: packet.muted,
        generation: packet.generation,
        data,
      },
      [data],
    );
  }

  /**
   * The received audio as a MediaStream, with the listener's tone but taken
   * before the volume control, so that a recording is whole even while the
   * speaker is muted. Null before audio has started. The returned function
   * takes the tap away again.
   */
  recordingStream(): { stream: MediaStream; release: () => void } | null {
    const source = this.trebleNode;
    if (!this.context || !source || typeof this.context.createMediaStreamDestination !== 'function') return null;
    const destination = this.context.createMediaStreamDestination();
    source.connect(destination);
    return {
      stream: destination.stream,
      release: () => {
        try {
          source.disconnect(destination);
        } catch {
          // Already gone with the context.
        }
      },
    };
  }

  reset(): void {
    this.post({ type: 'reset' });
  }

  setVolume(volume: number): void {
    this.volume = Math.max(0, Math.min(1, volume));
    this.applyGain();
  }

  setMuted(muted: boolean): void {
    this.muted = muted;
    this.applyGain();
  }

  /** Bass and treble shelf gains in dB. */
  setTone(bassDb: number, trebleDb: number): void {
    this.bassDb = bassDb;
    this.trebleDb = trebleDb;
    this.applyTone();
    this.applyGain();
  }

  private applyTone(): void {
    if (!this.context || !this.bassNode || !this.trebleNode) return;
    this.bassNode.gain.setTargetAtTime(this.bassDb, this.context.currentTime, 0.02);
    this.trebleNode.gain.setTargetAtTime(this.trebleDb, this.context.currentTime, 0.02);
  }

  /** Trades latency against robustness on a poor connection. */
  setTargetLatency(seconds: number): void {
    this.targetLatency = seconds;
    this.post({ type: 'target-latency', value: seconds });
  }

  private applyGain(): void {
    if (!this.gainNode || !this.context) return;
    // A boost takes the same amount off the volume: the decoder's output is
    // already levelled by the AGC to use the range, so 12 dB more of anything
    // would clip. Relative to the rest, the bass is still 12 dB up.
    const headroom = 10 ** (-Math.max(0, this.bassDb, this.trebleDb) / 20);
    const value = this.muted ? 0 : this.volume ** 2 * headroom; // perceptual taper
    this.gainNode.gain.setTargetAtTime(value, this.context.currentTime, 0.02);
  }

  private post(message: Record<string, unknown>): void {
    if (this.node) {
      this.node.port.postMessage(message);
      return;
    }
    this.fallback?.engine.handle(message as unknown as PlaybackMessage);
  }

  /** True when playback is running on the main thread instead of the audio thread. */
  get usingFallback(): boolean {
    return this.fallback !== null;
  }

  private setState(state: PlayerState): void {
    if (this.state === state) return;
    this.state = state;
    this.onStateChange?.(state);
  }
}
