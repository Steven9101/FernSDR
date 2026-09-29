/**
 * The jitter buffer is the difference between a receiver that is pleasant on a
 * hotel wifi and one that is not, so its policy is tested against the
 * conditions that actually break streams: loss, jitter, stalls, clock drift
 * and a backgrounded tab.
 */
import { describe, expect, it } from 'vitest';
import { JitterPolicy } from './jitter';

/**
 * Simulates a stream. `arrival` returns how many seconds of audio arrived in a
 * given callback; the policy steers playback and we watch the buffer.
 */
function simulate(
  seconds: number,
  arrival: (t: number) => number,
  options: { callback?: number } = {},
) {
  const callback = options.callback ?? 128 / 48000;
  const policy = new JitterPolicy();
  let buffered = policy.targetSeconds;
  let underruns = 0;
  let maxBuffered = 0;
  let minBuffered = Infinity;
  const trace: number[] = [];

  for (let t = 0; t < seconds; t += callback) {
    buffered += arrival(t);

    const decision = policy.update(buffered, callback);
    if (decision.resync) buffered = Math.min(buffered, decision.keepSeconds);

    const consumed = callback * decision.ratioScale;
    if (buffered < consumed) {
      underruns++;
      policy.noteUnderrun();
      buffered = policy.targetSeconds; // concealment refills to the target
    } else {
      buffered -= consumed;
    }

    if (t > seconds * 0.2) {
      maxBuffered = Math.max(maxBuffered, buffered);
      minBuffered = Math.min(minBuffered, buffered);
      trace.push(buffered);
    }
  }

  return { policy, buffered, underruns, maxBuffered, minBuffered, trace };
}

describe('jitter policy', () => {
  it('clears a persistent backlog below the emergency threshold', () => {
    const policy = new JitterPolicy();
    policy.reset();
    expect(policy.update(0.5, 0.01).resync).toBe(true);
    for (let i = 0; i < 150; i++) expect(policy.update(0.5, 0.01).resync).toBe(false);
    let trims = 0;
    for (let i = 0; i < 60; i++) if (policy.update(0.5, 0.01).resync) trims++;
    expect(trims).toBe(1);
  });

  it('does not trim brief delivery bursts repeatedly', () => {
    const policy = new JitterPolicy();
    policy.reset();
    policy.update(0.5, 0.01);
    for (let burst = 0; burst < 10; burst++) {
      for (let i = 0; i < 30; i++) expect(policy.update(0.3, 0.01).resync).toBe(false);
      expect(policy.update(0.09, 0.01).resync).toBe(false);
    }
  });

  it('holds a steady buffer on a perfect connection', () => {
    const callback = 128 / 48000;
    const result = simulate(60, () => callback);
    expect(result.underruns).toBe(0);
    // And it settles near the minimum, because nothing has gone wrong.
    expect(result.policy.targetSeconds).toBeLessThanOrEqual(0.09);
    expect(result.maxBuffered).toBeLessThan(0.15);
  });

  it('does not let latency creep up over a long session', () => {
    // The quiet failure: occasional small underruns push the target up, and
    // without something pushing back the receiver ends up a second behind
    // with nothing visibly wrong.
    const callback = 128 / 48000;
    let time = 0;
    const result = simulate(600, () => {
      time += callback;
      // A brief stall every 30 s, then recovery.
      const stalled = time % 30 < 0.12;
      return stalled ? 0 : callback * 1.02;
    });

    expect(result.policy.targetSeconds).toBeLessThan(0.35);
    expect(result.buffered).toBeLessThan(0.5);
  });

  it('grows the buffer when the connection is genuinely unreliable', () => {
    const callback = 128 / 48000;
    let time = 0;
    const result = simulate(120, () => {
      time += callback;
      // Bursty: nothing for 150 ms, then a lump.
      const phase = time % 0.2;
      return phase < 0.15 ? 0 : callback * (0.2 / 0.05);
    });

    // It should have found a target that covers the burst gap.
    expect(result.policy.targetSeconds).toBeGreaterThan(0.1);
    // And stopped underrunning once it got there.
    const late = result.trace.slice(Math.floor(result.trace.length * 0.7));
    expect(Math.min(...late)).toBeGreaterThan(0);
  });

  it('gives back a buffer the connection stopped needing within half a minute', () => {
    // A connection that breaks once, then delivers evenly. As in the playback engine, each
    // gap that ran the buffer dry is covered once packets resume, which is
    // what pushed the target to its ceiling in the lab. It must come back
    // down in seconds once arrivals are even, not in the minutes a fixed
    // step took.
    const callback = 128 / 48000;
    const policy = new JitterPolicy();
    let buffered = policy.targetSeconds;
    let dryFor = -1;
    let underruns = 0;
    for (let t = 0; t < 45; t += callback) {
      // One gap of a second and a half, as when a phone changes cells.
      const arrived = t >= 3 && t < 4.5 ? 0 : t >= 4.5 && t < 5 ? callback * 4 : callback * 1.001;
      if (arrived > 0 && dryFor >= 0) {
        policy.coverDeliveryGap(dryFor);
        dryFor = -1;
      }
      buffered += arrived;
      const decision = policy.update(buffered, callback);
      if (decision.resync) buffered = Math.min(buffered, decision.keepSeconds);
      const consumed = callback * decision.ratioScale;
      if (buffered < consumed) {
        if (dryFor < 0) {
          underruns++;
          policy.noteUnderrun();
          dryFor = 0;
        }
        dryFor += callback;
        buffered = 0;
      } else {
        buffered -= consumed;
      }
    }
    expect(underruns).toBeGreaterThan(0);
    expect(policy.targetSeconds).toBeLessThan(0.15);
    expect(buffered).toBeLessThan(0.25);
  });

  it('holds its headroom on a link that keeps running dry', () => {
    // Gaps of 300 ms every twelve seconds, as Wi-Fi holds or a narrow link
    // give: long enough apart for the buffer to be calm for a while between
    // them, often enough that giving the headroom back fast would only buy
    // the same underrun again at the next gap.
    const callback = 128 / 48000;
    const policy = new JitterPolicy();
    let buffered = policy.targetSeconds;
    let dryFor = -1;
    let late = 0;
    for (let t = 0; t < 180; t += callback) {
      const arrived = t % 12 < 0.3 ? 0 : callback * (12 / 11.7);
      if (arrived > 0 && dryFor >= 0) {
        policy.coverDeliveryGap(dryFor);
        dryFor = -1;
      }
      buffered += arrived;
      const decision = policy.update(buffered, callback);
      if (decision.resync) buffered = Math.min(buffered, decision.keepSeconds);
      const consumed = callback * decision.ratioScale;
      if (buffered < consumed) {
        if (dryFor < 0) {
          if (t > 90) late++;
          policy.noteUnderrun();
          dryFor = 0;
        }
        dryFor += callback;
        buffered = 0;
      } else {
        buffered -= consumed;
      }
    }
    // Once it has learned the gap, the gaps of the last minute and a half
    // are covered.
    expect(late).toBeLessThanOrEqual(1);
  });

  it('keeps the buffer that a recurring gap needs', () => {
    // Gaps of 400 ms every six seconds: relaxing must not undo the headroom
    // before the next one, or every gap becomes a dropout again.
    const callback = 128 / 48000;
    let time = 0;
    const result = simulate(180, () => {
      time += callback;
      return time % 6 < 0.4 ? 0 : callback * 1.08;
    });
    const late = result.trace.slice(Math.floor(result.trace.length * 0.6));
    expect(Math.min(...late)).toBeGreaterThan(0);
  });

  it('resynchronises rather than playing out a huge backlog', () => {
    // A backgrounded tab keeps receiving while the audio device is stopped.
    // Coming back, there may be many seconds queued; playing that out at
    // +0.4% would take minutes.
    const policy = new JitterPolicy();
    const decision = policy.update(8.0, 0.01);
    expect(decision.resync).toBe(true);
    expect(decision.keepSeconds).toBeLessThan(0.3);
    expect(policy.stats(8.0).resyncs).toBe(1);
  });

  it('corrects clock drift without ever dropping samples', () => {
    // The sender's clock runs 100 ppm fast. Left alone the buffer grows
    // without bound; the fix must be a ratio nudge, never a discontinuity.
    const callback = 128 / 48000;
    const result = simulate(300, () => callback * 1.0001);

    expect(result.underruns).toBe(0);
    expect(result.maxBuffered).toBeLessThan(0.3);
    // The correction stays inaudibly small: well under a tenth of a semitone.
    expect(Math.abs(result.policy.stats(result.buffered).driftPpm)).toBeLessThan(5000);
  });

  it('corrects drift in the other direction too', () => {
    const callback = 128 / 48000;
    const result = simulate(300, () => callback * 0.9999);
    expect(result.maxBuffered).toBeLessThan(0.4);
    expect(result.buffered).toBeGreaterThan(0);
  });

  // 10.7 ms is one NAC3 frame, the server's default packet; 42.7 ms is the
  // most it packs (audio_packet_frames = 4).
  it.each([0.0107, 0.0427])('does not warble when audio arrives in %s s packets', (packet) => {
    // Audio arrives a packet at a time, so the buffer is a sawtooth even on
    // a perfect link with matched clocks. Steering on the instantaneous level
    // turned that sawtooth into a pitch wobble: the lab measured 1.6 Hz of
    // wander on a 1 kHz tone at about one to two cycles a second, 6 dB of
    // SINAD. With nothing to correct, the speed must stay put.
    const callback = 128 / 48000;
    const policy = new JitterPolicy();
    let buffered = policy.targetSeconds;
    let seed = 7;
    const random = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
    // Packet k leaves on the sender's clock and arrives up to 15 ms late.
    let k = 0;
    let next = random() * 0.015;
    const ratios: number[] = [];
    for (let t = 0; t < 60; t += callback) {
      while (next <= t) {
        buffered += packet;
        k++;
        next = k * packet + random() * 0.015;
      }
      const decision = policy.update(buffered, callback);
      if (decision.resync) buffered = Math.min(buffered, decision.keepSeconds);
      buffered = Math.max(0, buffered - callback * decision.ratioScale);
      if (t > 20) ratios.push(decision.ratioScale);
    }
    // Warble is the speed going up and down within a second; a steady ramp
    // while the target relaxes is not. Within each second, fit a line and
    // take the largest deviation from it: 20 ppm is 0.02 Hz on a 1 kHz tone,
    // and the old steering moved it by thousands. The ramp itself is held
    // under 0.5 Hz a second at 1 kHz.
    const perSecond = Math.round(1 / callback);
    let warble = 0;
    let ramp = 0;
    for (let i = 0; i + perSecond <= ratios.length; i += perSecond) {
      const w = ratios.slice(i, i + perSecond);
      const n = w.length;
      const mx = (n - 1) / 2;
      const my = w.reduce((a, b) => a + b, 0) / n;
      let sxy = 0;
      let sxx = 0;
      w.forEach((y, x) => {
        sxy += (x - mx) * (y - my);
        sxx += (x - mx) ** 2;
      });
      const slope = sxy / sxx;
      warble = Math.max(warble, ...w.map((y, x) => Math.abs(y - (my + slope * (x - mx)))));
      ramp = Math.max(ramp, Math.abs(slope * n));
    }
    expect(warble * 1e6).toBeLessThan(20);
    expect(ramp * 1e6).toBeLessThan(500);
    // The slow part, the target relaxing toward less latency, stays under
    // 0.1 % with the default packets: under a Hz at 1 kHz, over seconds.
    if (packet < 0.02) expect(Math.max(...ratios.map((r) => Math.abs(r - 1))) * 1e6).toBeLessThan(1000);
  });

  it('keeps the correction inaudible', () => {
    // Anything beyond a few cents of pitch would be heard on music and on a
    // steady carrier alike.
    const policy = new JitterPolicy();
    for (let i = 0; i < 10000; i++) policy.update(5.0, 0.01);
    const stats = policy.stats(5.0);
    expect(Math.abs(stats.driftPpm)).toBeLessThanOrEqual(4000);
  });

  it('recovers from a total stall', () => {
    const callback = 128 / 48000;
    let time = 0;
    const result = simulate(60, () => {
      time += callback;
      // Ten seconds of nothing in the middle.
      if (time > 20 && time < 30) return 0;
      return callback;
    });
    // It kept going afterwards rather than wedging.
    expect(result.buffered).toBeGreaterThan(0);
    expect(result.policy.targetSeconds).toBeLessThanOrEqual(0.6);
  });
});
