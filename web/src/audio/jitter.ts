/**
 * Jitter buffer policy.
 *
 * Kept as a pure object, separate from the AudioWorklet, so it can be tested
 * against loss, jitter, stalls and clock drift without a browser. The worklet
 * does the sample shuffling; every decision about how much to buffer and how
 * fast to play lives here.
 *
 * Three failures it exists to prevent, in the order people notice them:
 *
 * 1. UNDERRUN - the buffer empties and the audio breaks up. Handled by an
 *    adaptive target that grows when the connection proves unreliable.
 * 2. OVERRUN - the buffer grows without bound, usually after a stall or a
 *    backgrounded tab, until the receiver is minutes behind. Handled by
 *    resampling slightly fast, and by a single hard resynchronisation when
 *    the backlog is past saving.
 * 3. CREEPING LATENCY - the quiet one. Each small underrun nudges the buffer
 *    up, nothing ever nudges it back, and after an hour the receiver is a
 *    second behind with nothing obviously wrong. Handled by letting the target
 *    fall again once the connection has behaved for a while.
 */

export interface JitterConfig {
  /** Never buffer less than this, however good the connection looks. */
  minTargetSeconds: number;
  /** Never buffer more than this, however bad it looks. */
  maxTargetSeconds: number;
  /** Where to start before anything is known about the connection. */
  initialTargetSeconds: number;
  /**
   * Largest speed-up or slow-down used to steer the buffer, as a fraction.
   * 0.004 is about seven cents of pitch: inaudible on speech and music alike,
   * and the reason this never resorts to dropping samples.
   */
  maxDriftCorrection: number;
  /**
   * Beyond target + this, the backlog is not worth playing out and the stream
   * resynchronises to the live edge in one step.
   */
  resyncThresholdSeconds: number;
  /** How long the connection must behave before the target is allowed down. */
  relaxAfterSeconds: number;
}

/** How far past the target a freshly reset stream may sit before it is trimmed. */
const FRESH_TRIM_SECONDS = 0.15;

/**
 * Time constant of the buffer level the speed is steered on.
 *
 * Audio arrives a packet at a time, so even a perfect link with matched clocks
 * gives a sawtooth buffer, 40 ms deep at the usual packet size, plus whatever
 * jitter the network adds. Steered on the instantaneous level, that sawtooth
 * became the playback speed: the benchmark lab measured a 1 kHz tone wandering
 * by 1.6 Hz at one to two cycles a second, heard as warble and costing 6 dB of
 * SINAD and FT8 and CW decodes. What the steering is for, clock drift and the
 * slow drain after a burst, moves over tens of seconds, so a 2 s average loses
 * nothing there and takes the sawtooth down by two orders of magnitude.
 */
const STEER_SMOOTH_SECONDS = 2;

/**
 * How far the averaged level may sit from the target before the speed moves
 * at all. Even averaged, the level carries half a packet of sawtooth above the
 * target and moves whenever the target is relaxed by 10 ms; steering on that
 * kept the speed changing by hundreds of ppm for no reason a listener has.
 * Inside the band the audio plays at exactly the sender's rate.
 *
 * The band is lopsided because the level is the latency: above the target it
 * only has to hold the sawtooth of the usual 10.7 ms packets and some jitter,
 * so latency ends at most 15 ms over the target; below, running a little low
 * costs nothing until the target itself grows. A 100 ppm clock difference
 * takes minutes to cross either edge and is then held there by a correction of
 * the same 100 ppm.
 */
const STEER_BAND_ABOVE_SECONDS = 0.015;
const STEER_BAND_BELOW_SECONDS = 0.03;

/**
 * Speed change per second of level beyond the band. The buffer integrates the
 * speed error, so this is the loop gain: 0.05/s crosses over near 0.008 Hz,
 * ten times below the averaging, so the loop cannot ring, while a burst 120 ms
 * past the band still drains at the full 0.4 %.
 */
const STEER_GAIN_PER_SECOND = 0.05;

/** Headroom a relaxing target never gives back, below the lowest level seen. */
const RELAX_RESERVE_SECONDS = 0.02;

/** How long a close call counts against giving the buffer back quickly. */
const CLOSE_CALL_MEMORY_SECONDS = 60;


export const DEFAULT_JITTER_CONFIG: JitterConfig = {
  minTargetSeconds: 0.04,
  maxTargetSeconds: 0.6,
  initialTargetSeconds: 0.07,
  maxDriftCorrection: 0.004,
  resyncThresholdSeconds: 0.75,
  relaxAfterSeconds: 8,
};

export interface JitterDecision {
  /** Multiply the nominal resampling ratio by this. */
  ratioScale: number;
  /** Discard everything but the newest `keepSeconds` and carry on. */
  resync: boolean;
  keepSeconds: number;
}

export interface JitterStats {
  targetSeconds: number;
  bufferedSeconds: number;
  driftPpm: number;
  underruns: number;
  resyncs: number;
  /** Lowest buffer level seen recently: the real measure of headroom. */
  marginSeconds: number;
}

export class JitterPolicy {
  private config: JitterConfig;
  private target: number;
  private drift = 0;
  /** The buffer level averaged over STEER_SMOOTH_SECONDS; NaN until the first update. */
  private level = NaN;
  private observedMinimum = Infinity;
  private windowSeconds = 0;
  private calmSeconds = 0;
  private underruns = 0;
  /**
   * Stream time of the close calls in the last CLOSE_CALL_MEMORY_SECONDS:
   * underruns, and windows whose lowest level was under a quarter of the
   * target.
   */
  private closeCalls: number[] = [];
  /** Seconds of stream seen by update(), the clock closeCalls are on. */
  private clock = 0;
  private resyncs = 0;
  private lastMargin = 0;
  private excessSeconds = 0;
  /**
   * One cheap trim, available to a stream that has just been reset.
   *
   * A socket that was blocked delivers its backlog the moment it drains, and
   * several hundred milliseconds of it sits under the ordinary
   * resynchronisation threshold - so it plays out at 0.4 percent and the
   * listener is a third of a second behind for the next minute and a half.
   * None of that audio is worth keeping: it is a live receiver and the burst
   * is by definition old.
   *
   * Four shapes were tried against the soak harness, an outage every two
   * minutes. Measured as the worst lasting difference from the first
   * segment's settled latency, and as how many samples sat above 200 ms:
   *
   *   nothing                      +153 ms, and a minute of drain each time
   *   lower threshold for 3 s      six times the resynchronisations, each a skip
   *   three trims, taken eagerly   worse than one: each resets the drift
   *                                correction and the early ones are spent on
   *                                the front of a burst still landing
   *   one trim, held 0.5 s         +157 ms, two bad segments of five
   *   one trim, taken at once      +12 ms, one bad segment of four
   *
   * A recovering stream therefore gets one immediate trim. Later bursts must
   * remain above the target for two seconds before another trim is allowed,
   * so draining a transient burst does not repeatedly move the play head.
   */
  private trimAvailable = false;

  constructor(config: Partial<JitterConfig> = {}) {
    this.config = { ...DEFAULT_JITTER_CONFIG, ...config };
    this.target = this.config.initialTargetSeconds;
  }

  get targetSeconds(): number {
    return this.target;
  }

  reset(): void {
    this.target = this.config.initialTargetSeconds;
    this.restartTiming();
  }

  /** A sender-side skip invalidates buffer history, but not the learned target. */
  restartTiming(): void {
    this.drift = 0;
    this.level = NaN;
    this.observedMinimum = Infinity;
    this.windowSeconds = 0;
    this.calmSeconds = 0;
    this.trimAvailable = true;
    this.excessSeconds = 0;
  }

  /** The buffer ran dry. Raise the target so it is less likely to recur. */
  noteUnderrun(): void {
    this.underruns++;
    this.closeCalls.push(this.clock);
    // A step rather than a nudge: an underrun already cost the listener a
    // glitch, and creeping up 5 ms at a time would cost several more.
    this.target = Math.min(this.config.maxTargetSeconds, this.target + 0.04);
    this.calmSeconds = 0;
    this.observedMinimum = Infinity;
    this.windowSeconds = 0;
  }

  coverDeliveryGap(seconds: number): void {
    if (!Number.isFinite(seconds)) return;
    this.target = Math.max(this.target, Math.min(this.config.maxTargetSeconds, seconds));
    this.calmSeconds = 0;
  }

  /**
   * Called once per audio callback.
   *
   * `bufferedSeconds` is how much decoded audio is waiting, and
   * `elapsedSeconds` how much wall time this callback covers.
   */
  update(bufferedSeconds: number, elapsedSeconds: number, minimumTargetSeconds = 0): JitterDecision {
    this.target = Math.max(this.target, minimumTargetSeconds);
    this.clock += elapsedSeconds;
    this.observedMinimum = Math.min(this.observedMinimum, bufferedSeconds);
    this.windowSeconds += elapsedSeconds;
    this.calmSeconds += elapsedSeconds;
    // A shaped 64 kbit/s link left 700 ms queued against a 210 ms target,
    // below the emergency threshold. At 0.4% drift correction that takes
    // minutes to drain. Trim only if the excess stays for two seconds; an
    // ordinary burst falls back below this threshold as it plays out.
    this.excessSeconds = bufferedSeconds > this.target + FRESH_TRIM_SECONDS
      ? this.excessSeconds + elapsedSeconds : 0;

    // Far too much buffered to play out: usually a backgrounded tab that kept
    // receiving, or a stall that ended. Trim in one step rather than spending
    // a minute at +0.4% getting back.
    // A fresh stream sitting well above its target is holding a burst that
    // arrived while the socket was blocked, and none of that audio is worth
    // playing: this is a live receiver and the listener wants now.
    if (
      bufferedSeconds > this.target + this.config.resyncThresholdSeconds ||
      (this.trimAvailable && bufferedSeconds > this.target + FRESH_TRIM_SECONDS) ||
      this.excessSeconds >= 2
    ) {
      this.trimAvailable = false;
      this.resyncs++;
      this.drift = 0;
      this.level = NaN;
      this.observedMinimum = Infinity;
      this.windowSeconds = 0;
      this.excessSeconds = 0;
      return { ratioScale: 1, resync: true, keepSeconds: this.target };
    }

    // Every few seconds, look at how close the buffer actually came to empty
    // and move the target accordingly.
    if (this.windowSeconds >= 4) {
      this.lastMargin = this.observedMinimum;
      if (this.observedMinimum < this.target * 0.25) this.closeCalls.push(this.clock);
      if (this.observedMinimum < 0.015) {
        // Nearly dry: the connection needs more headroom than it is getting.
        this.target = Math.min(this.config.maxTargetSeconds, this.target + 0.03);
        this.calmSeconds = 0;
      } else if (
        this.calmSeconds > this.config.relaxAfterSeconds &&
        this.observedMinimum > this.target * 0.5
      ) {
        // It has behaved for a while with room to spare. Give some back -
        // this is the step that stops latency creeping up over an hour.
        //
        // How much is set by the room it never used: the lowest the buffer
        // came in these seconds, less RELAX_RESERVE_SECONDS it keeps. Half of
        // that goes each time. A fixed 10 ms step took four minutes to come
        // back from 0.6 s, so a listener whose link dropped to 24 kbit/s or
        // changed cells once stayed a second behind long after the audio was
        // arriving evenly again (the lab: 950 ms at 24 kbit/s, 745 ms after a
        // cell change, 232 ms on a clean link). Halving the spare still never
        // takes the buffer below what the last seconds actually used.
        //
        // Only after a one-off, though. A link that keeps running the buffer
        // low (24 kbit/s, Wi-Fi holds, loss) needs the headroom again within
        // seconds, and giving it back fast there cost the lab 8 % of the audio
        // at 24 kbit/s and 40 ms of latency on Wi-Fi. So when the last minute
        // held more than one close call, an underrun or a window that took
        // the buffer under a quarter of the target, the step stays at 10 ms.
        // Counting only underruns was not enough: once the gaps were covered
        // there were none, and a minute later the headroom went again.
        this.forgetOldCloseCalls();
        const spare = Math.max(0, this.observedMinimum - RELAX_RESERVE_SECONDS);
        const step = this.closeCalls.length <= 1 ? Math.max(0.01, spare * 0.5) : 0.01;
        this.target = Math.max(this.config.minTargetSeconds, minimumTargetSeconds, this.target - step);
      }
      this.observedMinimum = Infinity;
      this.windowSeconds = 0;
    }

    // Steer toward the target by playing fractionally fast or slow. The
    // correction is proportional to the averaged level, clamped, and slewed,
    // so it is a slow pitch change of a few cents rather than anything audible.
    this.level = Number.isNaN(this.level)
      ? bufferedSeconds
      : this.level + (bufferedSeconds - this.level) * Math.min(1, elapsedSeconds / STEER_SMOOTH_SECONDS);
    const offset = this.level - this.target;
    const beyond = offset > 0
      ? Math.max(0, offset - STEER_BAND_ABOVE_SECONDS)
      : Math.min(0, offset + STEER_BAND_BELOW_SECONDS);
    const wanted = Math.max(
      -this.config.maxDriftCorrection,
      Math.min(this.config.maxDriftCorrection, beyond * STEER_GAIN_PER_SECOND),
    );
    // Slewed over about 2 s: a new target or the edge of the band then bends
    // the speed gently instead of kinking it.
    this.drift += (wanted - this.drift) * Math.min(1, elapsedSeconds / 2);

    return { ratioScale: 1 + this.drift, resync: false, keepSeconds: this.target };
  }

  private forgetOldCloseCalls(): void {
    while (this.closeCalls.length && this.clock - this.closeCalls[0] > CLOSE_CALL_MEMORY_SECONDS) {
      this.closeCalls.shift();
    }
  }

  stats(bufferedSeconds: number): JitterStats {
    return {
      targetSeconds: this.target,
      bufferedSeconds,
      driftPpm: Math.round(this.drift * 1e6),
      underruns: this.underruns,
      resyncs: this.resyncs,
      marginSeconds: Number.isFinite(this.lastMargin) ? this.lastMargin : 0,
    };
  }
}
