/**
 * The playback core: NAC decode, jitter policy, resampling and concealment.
 *
 * It lives apart from the worklet because it has to run in two places. An
 * AudioWorklet only exists in a secure context, and a receiver served over
 * plain HTTP - which is the default - has no `audioWorklet` at all. There the
 * same engine is driven by a ScriptProcessorNode instead. One core, two hosts:
 * a second implementation would be a second jitter policy to keep honest.
 *
 * Ordinary packet loss is concealed with a decaying repeat of one frame.
 * Clock drift is corrected by nudging the resampling ratio, without inserting
 * or dropping samples. An explicit sender discontinuity is different: old
 * audio was discarded, so reconstructing its timeline would restore the
 * delay the sender was trying to remove.
 */
import { NacDecoder, FRAME_HOP, MAX_PACKET_FRAMES } from '../dsp/nac';
import { buildKernel, resampleAt } from './resampler';
import { JitterPolicy, DEFAULT_JITTER_CONFIG } from './jitter';

const RING_SECONDS = 2;

export interface PacketMessage {
  type: 'packet';
  compact?: boolean;
  /** NAC3: one to four frames, sequenced by the first. */
  packet?: boolean;
  discontinuity?: boolean;
  sequence: number;
  muted: boolean;
  generation: number;
  data: ArrayBuffer;
}

export interface ConfigMessage {
  type: 'config';
  rate: number;
  generation: number;
}

export interface ControlMessage {
  type: 'gain' | 'target-latency' | 'reset';
  value?: number;
}

export type PlaybackMessage = PacketMessage | ConfigMessage | ControlMessage;

export class NacPlaybackEngine {
  private decoder = new NacDecoder();
  private concealDecoder = new NacDecoder();
  private frame = new Float32Array(FRAME_HOP);
  private packetFrames = new Float32Array(FRAME_HOP * MAX_PACKET_FRAMES);
  /** Frames carried by the last packet: the next sequence follows by that many. */
  private lastFrames = 1;
  private concealRing = new Float32Array(512);
  private concealWritten = 0;
  private concealPosition = 0;
  private recovering = false;
  private fadeRemaining = 0;

  private sourceRate = 12000;
  private generation = -1;
  private kernel = buildKernel(12000, 48000);

  private ring = new Float32Array(Math.ceil(12000 * RING_SECONDS));
  private ringSize = this.ring.length;
  private writeIndex = 0;
  /** Fractional read position, in source samples; absolute, not reduced. */
  private readPosition = 0;
  private written = 0;

  private gain = 1;
  private policy = new JitterPolicy();

  private lastSequence = -1;
  private concealedFrames = 0;
  private discontinuities = 0;
  /**
   * When a packet last arrived, on the audio clock.
   *
   * An empty buffer means two very different things. If packets are arriving
   * and the buffer still ran dry, the buffer is too small and the target
   * should grow. If nothing is arriving at all, the buffer is irrelevant: the
   * connection is gone, and growing the target punishes the listener for the
   * network rather than protecting them from it.
   */
  private lastPacketFrames = 0;
  /**
   * Whether a packet has arrived since the last underrun that was counted.
   *
   * One outage must cost one step, not one step per empty render. Earlier
   * versions refilled the receive queue with concealment and counted every
   * empty render again, walking the target to its ceiling during one stall.
   * Requiring a real packet in between means an
   * underrun only counts as evidence when there is something to have been
   * early or late.
   */
  private packetSinceUnderrun = true;
  private renderedFrames = 0;
  private decodedFrames = 0;
  private primed = false;
  private reportCountdown = 0;

  constructor(
    private readonly outputRate: number,
    private readonly onStats: (stats: Record<string, number>) => void,
  ) {
    this.kernel = buildKernel(this.sourceRate, outputRate);
  }

  handle(message: PlaybackMessage): void {
    switch (message.type) {
      case 'config':
        this.configure(message.rate, message.generation);
        break;
      case 'packet':
        this.acceptPacket(message);
        break;
      case 'gain':
        this.gain = message.value ?? 1;
        break;
      case 'target-latency':
        // A listener asking for a particular latency sets the floor the policy
        // works from; it is still free to buffer more if the link demands it.
        this.policy = new JitterPolicy({
          minTargetSeconds: Math.min(
            DEFAULT_JITTER_CONFIG.maxTargetSeconds,
            Math.max(0.02, message.value ?? DEFAULT_JITTER_CONFIG.minTargetSeconds),
          ),
          initialTargetSeconds: Math.min(
            DEFAULT_JITTER_CONFIG.maxTargetSeconds,
            Math.max(0.02, message.value ?? DEFAULT_JITTER_CONFIG.initialTargetSeconds),
          ),
        });
        // Deliberately not un-primed. Changing what the policy aims to buffer
        // does not invalidate what is already in the ring, and dropping the
        // primed flag costs a full target of digital silence while it refills.
        // Harmless while this only fires at startup; a guaranteed dropout on
        // every movement the moment anyone builds a latency control.
        break;
      case 'reset':
        this.resetStream();
        break;
    }
  }

  private configure(rate: number, generation: number): void {
    if (!Number.isFinite(rate) || rate < 4000 || rate > 192000 ||
        !Number.isInteger(generation) || generation < 0 || generation > 15) return;
    if (rate === this.sourceRate && generation === this.generation) return;
    this.sourceRate = rate;
    this.generation = generation;
    this.kernel = buildKernel(rate, this.outputRate);
    const size = Math.ceil(rate * RING_SECONDS);
    if (size !== this.ringSize) {
      this.ring = new Float32Array(size);
      this.ringSize = size;
    }
    this.resetStream();
  }

  private clearReceiveBuffer(): void {
    this.ring.fill(0);
    this.writeIndex = 0;
    this.written = 0;
    this.readPosition = 0;
    this.decoder.reset();
  }

  private resetStream(): void {
    this.lastPacketFrames = this.renderedFrames;
    this.clearReceiveBuffer();
    this.lastSequence = -1;
    this.lastFrames = 1;
    this.primed = false;
    this.recovering = false;
    this.fadeRemaining = 0;
    this.packetSinceUnderrun = true;
    this.policy.reset();
  }

  private beginRecovery(): void {
    this.primed = false;
    this.recovering = true;
    this.concealDecoder.copyStateFrom(this.decoder);
    this.concealRing.fill(0);
    this.concealWritten = 0;
    this.concealPosition = 0;
  }

  private discardExpiredAudio(): void {
    if (this.primed) this.beginRecovery();
    this.clearReceiveBuffer();
    this.primed = false;
    this.fadeRemaining = 0;
    this.policy.restartTiming();
    this.discontinuities++;
  }

  /**
   * How long the stream may be silent before it counts as stopped rather than
   * late. Cellular delivery gaps can exceed 200 ms while the socket is still
   * healthy. Resetting on each such gap erases what the jitter policy learned
   * and makes the next burst underrun in exactly the same way.
   */
  private static readonly kStallSeconds = 1;

  private acceptPacket(message: PacketMessage): void {
    if (message.generation !== this.generation && this.generation >= 0) {
      // A frame from a configuration we have not been told about yet; the
      // control message is in flight. Dropping it is better than decoding it
      // at the wrong rate.
      return;
    }

    // The stream stopped and has come back. That is a new stream whatever the
    // socket did - a reconnect, a tab that was frozen, a tunnel - and the
    // right thing is to start again rather than carry the outage's target and
    // its stale ring forward. Without this the latency a listener sees after a
    // blip stays where the blip left it, which is the whole complaint.
    const silence = (this.renderedFrames - this.lastPacketFrames) / this.outputRate;
    if (this.lastSequence >= 0 && silence <= NacPlaybackEngine.kStallSeconds) {
      // A duplicate or a packet from before the last one starts inside what
      // was already played.
      const advance = (message.sequence - this.lastSequence) & 0xffff;
      if (advance < this.lastFrames || advance > 0x8000) return;
    }
    if (message.discontinuity) {
      // The server deliberately removed stale, wholly unsent frames. Filling
      // their sequence gap would put that old timeline straight back in the
      // receive ring. Retain the jitter target learned on this connection,
      // rebuffer fresh audio and fade in with a new MDCT overlap instead.
      this.discardExpiredAudio();
    } else if (this.lastSequence >= 0 && silence > NacPlaybackEngine.kStallSeconds) {
      this.resetStream();
    } else if (this.recovering && !this.packetSinceUnderrun) {
      // Packets of several frames arrive that many frames apart, so the
      // spacing to cover is a packet's worth, not one frame.
      this.policy.coverDeliveryGap(silence + (this.lastFrames * FRAME_HOP) / this.sourceRate);
    }

    // Conceal anything the network lost, so the timeline stays intact.
    if (!message.discontinuity && this.lastSequence >= 0) {
      const advance = (message.sequence - this.lastSequence) & 0xffff;
      if (advance < this.lastFrames || advance > 0x8000) return;
      const gap = advance - this.lastFrames;
      if (gap > 0 && gap < 64) {
        for (let i = 0; i < gap; i++) {
          this.decoder.conceal(this.frame);
          this.push(this.frame);
          this.concealedFrames++;
        }
      } else if (gap >= 64) {
        this.resetStream();
      }
    }
    this.lastSequence = message.sequence;
    this.lastPacketFrames = this.renderedFrames;
    this.packetSinceUnderrun = true;

    const payload = new Uint8Array(message.data);
    if (message.packet) {
      const frames = this.decoder.decodePacket(payload, this.packetFrames, MAX_PACKET_FRAMES);
      if (frames === 0) {
        // Not even a frame count: conceal one frame and keep the clock.
        this.lastFrames = 1;
        this.decoder.conceal(this.frame);
        this.concealedFrames++;
        this.push(this.frame);
        return;
      }
      this.lastFrames = frames;
      if (this.decoder.lastPacketOk) this.decodedFrames += frames;
      else this.concealedFrames += frames;
      if (message.muted) this.packetFrames.fill(0, 0, frames * FRAME_HOP);
      this.push(this.packetFrames, frames * FRAME_HOP);
      return;
    }
    this.lastFrames = 1;
    if (this.decoder.decode(payload, this.frame, message.compact)) this.decodedFrames++;
    else this.concealedFrames++;

    if (message.muted) this.frame.fill(0);
    this.push(this.frame);
  }

  private push(samples: Float32Array, count = samples.length): void {
    for (let i = 0; i < count; i++) {
      this.ring[this.writeIndex] = samples[i];
      this.writeIndex = (this.writeIndex + 1) % this.ringSize;
    }
    this.written += count;
  }

  /** Decoded samples not yet played. */
  private get buffered(): number {
    return this.written - this.readPosition;
  }

  /** Fills one mono block. The caller owns the buffer and the output rate. */
  render(channel: Float32Array): void {
    const frames = channel.length;
    const rate = this.sourceRate;
    const needed = (frames * rate) / this.outputRate;
    // A large ScriptProcessor callback can consume more than the jitter
    // target. Keep one packet beyond that quantum: arrivals are quantised in
    // whole packets, one to four frames, and alternate around the callback's
    // fractional need.
    const minimum = needed + this.lastFrames * FRAME_HOP + 16;
    this.renderedFrames += frames;

    if (this.primed && this.buffered < needed + 8) {
      const silence = (this.renderedFrames - this.lastPacketFrames) / this.outputRate;
      if (silence < NacPlaybackEngine.kStallSeconds && this.packetSinceUnderrun) {
        this.policy.noteUnderrun();
        this.packetSinceUnderrun = false;
      }
      this.beginRecovery();
    }

    // Wait until there is enough to play before starting, otherwise the first
    // second of every tune-in stutters.
    if (!this.primed) {
      const target = Math.max(this.policy.targetSeconds * rate, minimum);
      if (this.buffered < target) {
        if (this.recovering) this.renderConcealment(channel);
        else channel.fill(0);
        this.reportStats(frames);
        return;
      }
      this.primed = true;
      this.readPosition = this.written - target;
      this.fadeRemaining = this.recovering ? Math.ceil(this.outputRate * 0.005) : 0;
      this.recovering = false;
    }

    const decision = this.policy.update(this.buffered / rate, frames / this.outputRate, minimum / rate);
    if (decision.resync) {
      // Skip the backlog in one step. Audible, but so is being four seconds
      // behind, and this at least ends.
      this.readPosition = this.written - Math.max(decision.keepSeconds * rate, minimum);
    }

    const step = (rate / this.outputRate) * decision.ratioScale;
    const gain = this.gain;

    for (let i = 0; i < frames; i++) {
      // The ring is circular; the read position is absolute, so reduce it.
      const position = this.readPosition % this.ringSize;
      const fade = this.fadeRemaining > 0 ? 1 - this.fadeRemaining-- / Math.ceil(this.outputRate * 0.005) : 1;
      channel[i] = resampleAt(this.ring, this.ringSize, position, this.kernel) * gain * fade;
      this.readPosition += step;
    }

    this.reportStats(frames);
  }

  private renderConcealment(channel: Float32Array): void {
    // Concealment belongs to the output, not to the receive queue. Filling
    // that queue with a whole jitter target of synthetic audio made delayed
    // real packets wait behind it, adding latency after every short stall.
    // Keep a separate decoder state so returning packets retain their true
    // MDCT overlap, and rebuffer actual audio once before resuming.
    const size = this.concealRing.length;
    const step = this.sourceRate / this.outputRate;
    for (let i = 0; i < channel.length; i++) {
      if (this.concealWritten - this.concealPosition < 32) {
        this.concealDecoder.conceal(this.frame);
        this.concealRing.set(this.frame, this.concealWritten % size);
        this.concealWritten += FRAME_HOP;
        this.concealedFrames++;
      }
      channel[i] = resampleAt(this.concealRing, size, this.concealPosition % size, this.kernel) * this.gain;
      this.concealPosition += step;
    }
  }

  private reportStats(frames: number): void {
    if (--this.reportCountdown <= 0) {
      this.reportCountdown = Math.round(this.outputRate / frames / 4); // ~4 Hz
      const stats = this.policy.stats(Math.max(0, this.buffered) / this.sourceRate);
      this.onStats({
        latencyMs: stats.bufferedSeconds * 1000,
        targetMs: stats.targetSeconds * 1000,
        marginMs: stats.marginSeconds * 1000,
        driftPpm: stats.driftPpm,
        underruns: stats.underruns,
        resyncs: stats.resyncs,
        discontinuities: this.discontinuities,
        concealedFrames: this.concealedFrames,
        decodedFrames: this.decodedFrames,
      });
    }
  }
}
