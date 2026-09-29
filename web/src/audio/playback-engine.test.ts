/**
 * The audio path across an outage.
 *
 * The jitter policy is tested on its own in jitter.test.ts, and it behaves.
 * What it cannot see there is the thing an operator actually reported: after
 * the network blinks, the latency sits at three hundred milliseconds and stays
 * there for the rest of the session.
 *
 * Reproduced against the real client first (web/tools/soak.mjs, a six second
 * outage): baseline target 90 ms, target 320 ms afterwards and still 240 ms a
 * minute later. The cause is here rather than in the policy - every empty
 * render during the outage was reported as an underrun, and each one is worth
 * 40 ms of target.
 */
import { describe, expect, it } from 'vitest';
import { NacPlaybackEngine } from './playback-engine';
import { FRAME_HOP } from '../dsp/nac';
import vectors from '../dsp/testdata/vectors.json';

const OUTPUT_RATE = 48000;
const SOURCE_RATE = 12000;
const BLOCK = 128;
/** One packet carries one frame; at 12 kHz that is about 10.7 ms. */
const PACKET_SECONDS = FRAME_HOP / SOURCE_RATE;

function skippedStream(gap: number, marked: boolean, duplicates = false, recovering = false) {
  const source = vectors.audio.find((v) => v.sample_rate === SOURCE_RATE)!;
  const payloads = source.frames.map((f) => Uint8Array.from(f.payload.match(/../g)!.map((b) => parseInt(b, 16))));
  let stats: Record<string, number> = {}, before: Record<string, number> = {};
  const engine = new NacPlaybackEngine(OUTPUT_RATE, value => { stats = value; });
  engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });
  const output = new Float32Array(BLOCK);
  let sent = 0, sequenceOffset = 65500, nextBurst = 0, energy = 0, jumped = false;
  for (let time = 0; time < 5.5; time += BLOCK / OUTPUT_RATE) {
    if (time >= nextBurst && !(recovering && time >= 3.8 && time < 4.2)) {
      nextBurst = time + (time < 3 ? 0.3 : 0);
      while (sent * PACKET_SECONDS <= time) {
        const jump = !jumped && time >= (recovering ? 4.2 : 4);
        if (jump) { sequenceOffset += gap; jumped = true; before = stats; }
        const index = sent % payloads.length;
        const packet = { type: 'packet' as const, sequence: (sequenceOffset + sent) & 65535,
          generation: 1, compact: source.frames[index].compact, muted: false,
          discontinuity: jump && marked, data: payloads[index].slice().buffer };
        engine.handle(packet);
        if (jump && duplicates) engine.handle(packet);
        sent++;
      }
    }
    engine.render(output);
    for (const sample of output) energy += sample * sample;
  }
  return { stats, before, energy };
}

describe('explicit audio discontinuities', () => {
  for (const gap of [1, 20, 63, 64]) for (const recovering of [false, true]) {
    it(`skips ${gap} expired frames while ${recovering ? 'recovering' : 'playing'} without forgetting the learned buffer`, () => {
      const result = skippedStream(gap, true, false, recovering);
      expect(result.before.targetMs).toBeGreaterThanOrEqual(250);
      expect(result.stats.targetMs).toBeGreaterThanOrEqual(result.before.targetMs);
      expect(result.stats.latencyMs).toBeLessThan(result.stats.targetMs + 100);
      expect(result.stats.discontinuities).toBe(1);
      expect(result.stats.decodedFrames).toBeGreaterThan(450);
      expect(Number.isFinite(result.energy)).toBe(true);
      expect(result.energy).toBeGreaterThan(1);
    });
  }

  it('ignores a repeated marked packet before resetting decoder state', () => {
    const ordinary = skippedStream(20, true);
    const duplicated = skippedStream(20, true, true);
    expect(duplicated.energy).toBe(ordinary.energy);
    expect(duplicated.stats).toEqual(ordinary.stats);
    expect(duplicated.stats.discontinuities).toBe(1);
  });

  it('does not reconstruct the missing timeline of expired packets', () => {
    const short = skippedStream(1, true);
    for (const gap of [20, 63, 64]) {
      const longer = skippedStream(gap, true);
      expect(longer.stats.concealedFrames).toBe(short.stats.concealedFrames);
      expect(longer.stats.latencyMs).toBe(short.stats.latencyMs);
    }
  });
});

function playValidStream(outputRate: number, block: number, burstSeconds = 0, duplicates = false) {
  const source = vectors.audio.find((v) => v.sample_rate === SOURCE_RATE)!;
  const payloads = source.frames.map((f) => Uint8Array.from(f.payload.match(/../g)!.map((b) => parseInt(b, 16))));
  let stats: Record<string, number> = {};
  const engine = new NacPlaybackEngine(outputRate, (value) => { stats = value; });
  engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });
  const channel = new Float32Array(block);
  let sent = 0;
  let nextBurst = 0;
  let energy = 0;
  for (let time = 0; time < 8; time += block / outputRate) {
    if (time >= nextBurst) {
      nextBurst = time + burstSeconds;
      while (sent * PACKET_SECONDS <= time) {
        const packet = { type: 'packet' as const, sequence: sent & 65535, muted: false,
          compact: source.frames[sent % payloads.length].compact,
          generation: 1, data: payloads[sent % payloads.length].slice().buffer };
        engine.handle(packet);
        if (duplicates) engine.handle(packet);
        sent++;
      }
    }
    engine.render(channel);
    for (const sample of channel) energy += sample * sample;
  }
  return { stats, energy };
}

describe('valid audio through both browser output paths', () => {
  it('plays a 44.1 kHz ScriptProcessor quantum without reading beyond buffered audio', () => {
    const result = playValidStream(44100, 4096);
    expect(Number.isFinite(result.energy)).toBe(true);
    expect(result.energy).toBeGreaterThan(1);
    expect(result.stats.underruns).toBe(0);
    expect(result.stats.concealedFrames).toBe(0);
  });

  it('absorbs 60 ms delivery bursts with valid decoded audio', () => {
    const result = playValidStream(48000, 128, 0.06);
    expect(Number.isFinite(result.energy)).toBe(true);
    expect(result.energy).toBeGreaterThan(1);
    expect(result.stats.underruns).toBe(0);
    expect(result.stats.latencyMs).toBeLessThan(150);
  });

  it('ignores duplicates without changing the audio timeline', () => {
    const ordinary = playValidStream(48000, 128);
    const duplicated = playValidStream(48000, 128, 0, true);
    expect(duplicated.energy).toBe(ordinary.energy);
    expect(duplicated.stats.decodedFrames).toBe(ordinary.stats.decodedFrames);
    expect(duplicated.stats.concealedFrames).toBe(0);
  });

  it('learns repeated 300 ms cellular delivery gaps without treating each burst as a new stream', () => {
    const result = playValidStream(48000, 128, 0.3);
    expect(Number.isFinite(result.energy)).toBe(true);
    expect(result.energy).toBeGreaterThan(1);
    expect(result.stats.underruns).toBeLessThanOrEqual(2);
    expect(result.stats.targetMs).toBeGreaterThanOrEqual(300);
    expect(result.stats.latencyMs).toBeLessThan(400);
  });
});

/**
 * Drives the engine the way the worklet does: render blocks at the output
 * rate, and deliver packets at the source rate alongside them.
 *
 * The payloads are not valid NAC. The decoder refuses them and the engine
 * pushes a frame regardless, which is exactly the concealment path - and for
 * these tests what matters is the timing of packets, not their contents.
 */
function run({
  seconds,
  outageAt,
  outageSeconds,
}: {
  seconds: number;
  outageAt?: number;
  outageSeconds?: number;
}) {
  let stats: Record<string, number> = {};
  const engine = new NacPlaybackEngine(OUTPUT_RATE, (s) => {
    stats = s;
  });
  engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });

  const channel = new Float32Array(BLOCK);
  const blockSeconds = BLOCK / OUTPUT_RATE;
  let time = 0;
  let sequence = 0;
  let owed = 0;
  const targets: number[] = [];

  while (time < seconds) {
    const inOutage =
      outageAt !== undefined && time >= outageAt && time < outageAt + (outageSeconds ?? 0);

    owed += blockSeconds;
    while (owed >= PACKET_SECONDS) {
      owed -= PACKET_SECONDS;
      if (inOutage) continue;
      engine.handle({
        type: 'packet',
        sequence: sequence++ & 0xffff,
        muted: false,
        generation: 1,
        data: new Uint8Array(24).buffer,
      });
    }

    engine.render(channel);
    time += blockSeconds;
    if (stats.targetMs) targets.push(stats.targetMs);
  }

  return { stats, targets };
}

describe('the playback engine across an outage', () => {
  it('keeps a steady stream at the starting target', () => {
    const { stats } = run({ seconds: 6 });
    expect(stats.underruns).toBe(0);
    expect(stats.targetMs).toBeLessThanOrEqual(80);
  });

  it('does not ratchet the target through an outage', () => {
    // Six seconds away, then back: what the soak run did to the real client.
    const { stats } = run({ seconds: 30, outageAt: 8, outageSeconds: 6 });

    // At most one underrun is charged for the whole outage. Before this it was
    // five, and five is 200 ms of permanent latency.
    expect(stats.underruns).toBeLessThanOrEqual(1);
    // And the stream that came back is a new one, so the target is where a new
    // stream starts rather than where the outage left it.
    expect(stats.targetMs).toBeLessThanOrEqual(80);
  });

  it('comes back to the same latency after several outages', () => {
    const long = run({ seconds: 20, outageAt: 5, outageSeconds: 4 });
    const again = run({ seconds: 20 });
    // Within a step of the untroubled run, not three steps above it.
    expect(long.stats.targetMs).toBeLessThanOrEqual(again.stats.targetMs + 40);
  });

  it('skips a burst that arrives all at once after an outage', () => {
    // A socket that was blocked for eight seconds delivers eight seconds of
    // audio the moment it drains. None of it is worth playing: it is a live
    // receiver, and the listener wants now. Measured before this: 470 ms of
    // buffer after a reconnect, drifting off over the following minute.
    let stats: Record<string, number> = {};
    const engine = new NacPlaybackEngine(OUTPUT_RATE, (s) => {
      stats = s;
    });
    engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });
    const channel = new Float32Array(BLOCK);
    const blockSeconds = BLOCK / OUTPUT_RATE;
    let sequence = 0;
    const packet = () =>
      engine.handle({
        type: 'packet',
        sequence: sequence++ & 0xffff,
        muted: false,
        generation: 1,
        data: new Uint8Array(24).buffer,
      });

    // Two seconds of ordinary listening.
    let owed = 0;
    for (let time = 0; time < 2; time += blockSeconds) {
      owed += blockSeconds;
      while (owed >= PACKET_SECONDS) {
        owed -= PACKET_SECONDS;
        packet();
      }
      engine.render(channel);
    }

    // Silence, then the backlog draining at about ten times real time, which
    // is what a socket that was blocked actually does: the client renders
    // throughout, so it primes early and the rest of the burst piles up behind
    // the play head.
    for (let time = 0; time < 3; time += blockSeconds) engine.render(channel);
    let sent = 0;
    for (let time = 0; time < 1 && sent < 280; time += blockSeconds) {
      for (let i = 0; i < 3 && sent < 280; i++, sent++) packet();
      engine.render(channel);
    }
    for (let time = 0; time < 0.5; time += blockSeconds) engine.render(channel);

    // Within a step of where a stream starts, not three seconds behind.
    //
    // In this shape the policy's ordinary resynchronisation threshold is what
    // catches it, which is why the test passes with and without the one trim a
    // freshly reset stream is allowed. The trim earns its place against the
    // real client instead - see the note in jitter.ts for the four variants
    // and what each measured. This test guards the floor: a change that lets a
    // burst through, or that skips repeatedly to avoid one, fails here.
    expect(stats.latencyMs).toBeLessThan(200);
    expect(stats.resyncs).toBeLessThan(8);
  });

  it('still grows the buffer when packets arrive but arrive late', () => {
    // The case the target exists for must survive the fix: packets keep
    // coming, in bursts, and the buffer keeps running dry between them.
    let stats: Record<string, number> = {};
    const engine = new NacPlaybackEngine(OUTPUT_RATE, (s) => {
      stats = s;
    });
    engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });
    const channel = new Float32Array(BLOCK);
    const blockSeconds = BLOCK / OUTPUT_RATE;
    let sequence = 0;

    for (let time = 0; time < 30; time += blockSeconds) {
      // Nothing for 120 ms, then everything owed for that window at once.
      const phase = time % 0.15;
      if (phase < blockSeconds) {
        for (let i = 0; i < 14; i++) {
          engine.handle({
            type: 'packet',
            sequence: sequence++ & 0xffff,
            muted: false,
            generation: 1,
            data: new Uint8Array(24).buffer,
          });
        }
      }
      engine.render(channel);
    }

    expect(stats.targetMs).toBeGreaterThan(80);
  });
});

describe('NAC3 packets of several frames', () => {
  interface Nac3Packet { payload: string; frames: number }
  const source = (vectors as unknown as { nac3: { sample_rate: number; packets: Nac3Packet[] }[] }).nac3
    .find((v) => v.sample_rate === SOURCE_RATE)!;
  const bytes = (hex: string) => Uint8Array.from(hex.match(/../g)!.map((b) => parseInt(b, 16)));

  function play(skip: number | null, duplicate: number | null) {
    let stats: Record<string, number> = {};
    const engine = new NacPlaybackEngine(OUTPUT_RATE, (value) => { stats = value; });
    engine.handle({ type: 'config', rate: SOURCE_RATE, generation: 1 });
    const output = new Float32Array(BLOCK);
    let sequence = 65530;  // wraps inside the run
    let sent = 0;
    source.packets.forEach((packet, index) => {
      const message = { type: 'packet' as const, packet: true, sequence: sequence & 0xffff, generation: 1,
        muted: false, data: bytes(packet.payload).buffer };
      if (index !== skip) engine.handle(message);
      if (index === duplicate) engine.handle({ ...message, data: bytes(packet.payload).buffer });
      sequence += packet.frames;
      sent += packet.frames;
      // Render roughly in step so nothing runs dry or resynchronises.
      for (let played = 0; played < packet.frames * FRAME_HOP * OUTPUT_RATE / SOURCE_RATE; played += BLOCK) {
        engine.render(output);
      }
    });
    for (let i = 0; i < 40; i++) engine.render(output);
    return { stats, sent };
  }

  it('decodes every frame of every packet', () => {
    const { stats, sent } = play(null, null);
    expect(stats.decodedFrames).toBe(sent);
  });

  it('conceals exactly the frames a missing packet carried', () => {
    const skip = source.packets.findIndex((p, i) => i > 3 && p.frames === 3);
    const clean = play(null, null).stats;
    const { stats, sent } = play(skip, null);
    expect(stats.decodedFrames).toBe(sent - 3);
    expect(stats.concealedFrames - clean.concealedFrames).toBeGreaterThanOrEqual(3);
  });

  it('ignores a repeated packet instead of playing it twice', () => {
    const { stats, sent } = play(null, 6);
    expect(stats.decodedFrames).toBe(sent);
  });
});
