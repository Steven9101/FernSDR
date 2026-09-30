/**
 * Cross-checks the client's decoders against the server's.
 *
 * The vectors are produced by server/tools/gen_vectors.cpp (`make -C server
 * vectors`): they contain encoded frames alongside the samples the C++ decoder
 * produced from them. Two independent implementations of a bitstream drift
 * apart silently otherwise, and the symptom - audio that is subtly wrong
 * rather than obviously broken - is miserable to track down from a bug report.
 */
import { describe, expect, it } from 'vitest';
import { NacDecoder, FRAME_HOP, BAND_STARTS, BAND_WIDTHS, MAX_PACKET_FRAMES, riceKForQuality } from './nac';
import { WaterfallDecoder } from './waterfall';
import { BitReader, BitWriter } from './bitio';
import vectors from './testdata/vectors.json';

interface AudioCase {
  sample_rate: number;
  bitrate: number;
  frames: { payload: string; legacy_payload: string; compact: boolean; samples: number[] }[];
}

interface WaterfallCase {
  width: number;
  lines: { payload: string; intra: boolean; zero_runs?: boolean; adaptive?: boolean; step_db?: number; levels: number[] }[];
}

function fromHex(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

describe('NAC decoder', () => {
  const cases = vectors.audio as AudioCase[];

  it('has vectors to check against', () => {
    expect(cases.length).toBeGreaterThan(0);
  });

  it('rejects malformed compact metadata and conceals without nonfinite output', () => {
    const out = new Float32Array(FRAME_HOP);
    const decoder = new NacDecoder();
    for (const [mask, first, count, exponent, scale] of [
      [2, 0, 18, 160, 0], [3, 17, 1, 160, 0], [3, 16, 2, 160, 0],
      [3, 0, 0, 160, 0], [2, 0, 1, 401, 0], [2, 0, 1, 160, 6], [2, 0, 1, 160, 7],
    ]) {
      const writer = new BitWriter();
      writer.putBits(20, 6);
      writer.putBits(mask, 2);
      if (mask === 3) writer.putBits(first, 5);
      writer.putBits(count, 5);
      writer.putBits(exponent, 9);
      writer.putBits(scale, 3);
      expect(decoder.decode(writer.finish(), out, true)).toBe(false);
      expect(out.every(Number.isFinite)).toBe(true);
    }
    expect(decoder.decode(new Uint8Array([2]), out, true)).toBe(false);
  });

  for (const testCase of cases) {
    it(`matches the server at ${testCase.sample_rate} Hz, ${testCase.bitrate} bit/s`, () => {
      const decoder = new NacDecoder();
      const legacy = new NacDecoder();
      const out = new Float32Array(FRAME_HOP);
      const original = new Float32Array(FRAME_HOP);

      testCase.frames.forEach((frame, index) => {
        const ok = decoder.decode(fromHex(frame.payload), out, frame.compact);
        expect(ok, `frame ${index} should decode`).toBe(true);
        expect(legacy.decode(fromHex(frame.legacy_payload), original)).toBe(true);
        expect(out).toEqual(original);
        expect(frame.payload.length).toBeLessThanOrEqual(frame.legacy_payload.length);
        for (let i = 0; i < FRAME_HOP; i++) {
          // Float32 arithmetic in a different order; agreement to a few parts
          // in 10^6 of full scale is exact for practical purposes.
          expect(out[i]).toBeCloseTo(frame.samples[i], 5);
        }
      });
    });
  }

  it('conceals lost frames exactly as the server does', () => {
    const data = vectors.concealment as { sample_rate: number; payloads: string[]; after: number[][] };
    const decoder = new NacDecoder();
    const out = new Float32Array(FRAME_HOP);
    for (const payload of data.payloads) decoder.decode(fromHex(payload), out);

    for (const expected of data.after) {
      decoder.conceal(out);
      for (let i = 0; i < FRAME_HOP; i++) expect(out[i]).toBeCloseTo(expected[i], 5);
    }
  });

  it('rejects a truncated frame instead of playing garbage', () => {
    const frame = (vectors.audio as AudioCase[])[1].frames[10];
    const payload = fromHex(frame.payload);
    const decoder = new NacDecoder();
    const out = new Float32Array(FRAME_HOP);
    expect(decoder.decode(payload.slice(0, Math.floor(payload.length / 3)), out, frame.compact)).toBe(false);
    for (const sample of out) expect(Number.isFinite(sample)).toBe(true);
  });

  it('refuses a coefficient far outside audio and plays silence after it', () => {
    // Quality 0, exponent 200 and an escaped 2^31 - 1: whole frames whose one
    // coefficient is some 10^22. The same payloads as server/tests/test_nac.cpp.
    const silence = new BitWriter();
    silence.putBits(0, 6);
    for (let b = 0; b < BAND_WIDTHS.length; b++) silence.putBit(0);
    const quiet = silence.finish();
    for (const [hex, compact] of [['02000001e1ffffff7fffffff00', false], ['020e407fffffbfffffff80', true]] as const) {
      const decoder = new NacDecoder();
      const out = new Float32Array(FRAME_HOP);
      expect(decoder.decode(fromHex(hex), out, compact)).toBe(false);
      let peak = 0;
      for (const sample of out) peak = Math.max(peak, Math.abs(sample));
      expect(decoder.decode(quiet, out, false)).toBe(true);
      for (const sample of out) peak = Math.max(peak, Math.abs(sample));
      expect(peak).toBeLessThan(1);
    }
  });

  it('produces exactly one hop per frame, lost or not', () => {
    // The property digital modes depend on: no stretching, no skipping.
    const decoder = new NacDecoder();
    const out = new Float32Array(FRAME_HOP);
    const frames = (vectors.audio as AudioCase[])[1].frames;
    let produced = 0;
    for (let i = 0; i < frames.length; i++) {
      if (i % 4 === 3) decoder.conceal(out);
      else decoder.decode(fromHex(frames[i].payload), out, frames[i].compact);
      produced += out.length;
    }
    expect(produced).toBe(frames.length * FRAME_HOP);
  });
});

interface Nac3Case {
  sample_rate: number;
  bitrate: number;
  packets: { payload: string; frames: number; samples: number[] }[];
}

describe('NAC3 packets', () => {
  const cases = (vectors as unknown as { nac3: Nac3Case[] }).nac3;

  it('has vectors with every packet size', () => {
    const sizes = new Set(cases.flatMap((c) => c.packets.map((p) => p.frames)));
    expect([...sizes].sort()).toEqual([1, 2, 3, 4]);
  });

  for (const testCase of cases) {
    it(`matches the server at ${testCase.sample_rate} Hz, ${testCase.bitrate} bit/s`, () => {
      const decoder = new NacDecoder();
      const out = new Float32Array(FRAME_HOP * MAX_PACKET_FRAMES);
      testCase.packets.forEach((packet, index) => {
        const frames = decoder.decodePacket(fromHex(packet.payload), out, MAX_PACKET_FRAMES);
        expect(frames, `packet ${index}`).toBe(packet.frames);
        expect(decoder.lastPacketOk, `packet ${index} should decode`).toBe(true);
        for (let i = 0; i < frames * FRAME_HOP; i++) expect(out[i]).toBeCloseTo(packet.samples[i], 5);
      });
    });
  }

  it('refuses a packet larger than the space given, and a packet with no bytes', () => {
    const decoder = new NacDecoder();
    const out = new Float32Array(FRAME_HOP * MAX_PACKET_FRAMES);
    const four = cases[0].packets.find((p) => p.frames === 4)!;
    expect(decoder.decodePacket(fromHex(four.payload), out, 2)).toBe(0);
    expect(decoder.decodePacket(new Uint8Array(0), out, 4)).toBe(0);
  });

  it('conceals from a corrupt frame onwards and keeps whole frames on the clock', () => {
    for (const packet of cases[1].packets.filter((p) => p.frames > 1)) {
      const decoder = new NacDecoder();
      const out = new Float32Array(FRAME_HOP * MAX_PACKET_FRAMES).fill(Number.NaN);
      const payload = fromHex(packet.payload);
      // Cut the packet short: the last frames lose their coefficients.
      const frames = decoder.decodePacket(payload.slice(0, Math.max(1, payload.length >> 1)), out, 4);
      expect(frames).toBe(packet.frames);
      expect(decoder.lastPacketOk).toBe(false);
      for (let i = 0; i < frames * FRAME_HOP; i++) expect(Number.isFinite(out[i])).toBe(true);
    }
  });

  it('rejects invalid selectors, steps and Rice parameters', () => {
    const decoder = new NacDecoder();
    const out = new Float32Array(FRAME_HOP * MAX_PACKET_FRAMES);
    const frame = (write: (w: BitWriter) => void) => {
      const writer = new BitWriter();
      writer.putBits(0, 2); // one frame
      write(writer);
      return writer.finish();
    };
    const invalid = [
      // one active band, first step beyond +200
      frame((w) => { w.putBits(2, 2); w.putBits(1, 5); w.putBits(401, 9); w.putBits(0, 3); w.putBits(0, 4); }),
      // selector 6
      frame((w) => { w.putBits(2, 2); w.putBits(1, 5); w.putBits(200, 9); w.putBits(6, 3); w.putBits(0, 4); }),
      // interval mask running past the last band
      frame((w) => { w.putBits(3, 2); w.putBits(16, 5); w.putBits(2, 5); }),
      // two bands, Rice parameter stepping below zero
      frame((w) => { w.putBits(2, 2); w.putBits(2, 5); w.putBits(200, 9); w.putBits(1, 3); w.putSignedRice(0, 0);
        w.putBits(0, 4); w.putSignedRice(-1, 0); }),
    ];
    for (const payload of invalid) {
      expect(decoder.decodePacket(payload, out, 4)).toBe(1);
      expect(decoder.lastPacketOk).toBe(false);
      for (let i = 0; i < FRAME_HOP; i++) expect(Number.isFinite(out[i])).toBe(true);
    }
  });
});

describe('waterfall decoder', () => {
  const cases = vectors.waterfall as WaterfallCase[];

  it('rejects a precision change without an independent row and recovers on a keyframe', () => {
    const decoder = new WaterfallDecoder();
    const out = new Float32Array(16);
    const independent = (value: number) => {
      const writer = new BitWriter();
      writer.putBits(1, 1); writer.putBits(0, 4);
      writer.putSignedRice(value, 0);
      for (let i = 1; i < out.length; i++) writer.putSignedRice(0, 0);
      return writer.finish();
    };
    const dependent = new BitWriter();
    dependent.putBits(0, 1); dependent.putBits(0, 4);
    for (let i = 0; i < out.length; i++) dependent.putSignedRice(0, 0);
    const delta = dependent.finish();
    expect(decoder.decode(independent(0), 16, out)).toBe(true);
    expect(decoder.decode(delta, 16, out, false, false, 2)).toBe(false);
    expect(decoder.decode(independent(1), 16, out, false, false, 2)).toBe(true);
    expect([...out]).toEqual(Array(16).fill(-98));
    expect(decoder.decode(delta, 16, out, false, false, 1)).toBe(false);
    expect(decoder.decode(independent(1), 16, out, false, false, 1)).toBe(true);
    expect([...out]).toEqual(Array(16).fill(-99));
    expect(decoder.decode(independent(101), 16, out, false, false, 2)).toBe(false);
    expect(decoder.decode(delta, 16, out, false, false, 2)).toBe(false);
    expect(decoder.decode(independent(0), 16, out, false, false, 0)).toBe(false);
  });

  for (const testCase of cases) {
    it(`matches the server at ${testCase.width} bins`, () => {
      const decoder = new WaterfallDecoder();
      const out = new Float32Array(testCase.width);

      testCase.lines.forEach((line, index) => {
        if (line.intra && index === 6) decoder.reset();
        const ok = decoder.decode(fromHex(line.payload), testCase.width, out, line.zero_runs, line.adaptive, line.step_db);
        expect(ok, `line ${index} should decode`).toBe(true);
        for (let i = 0; i < testCase.width; i++) {
          expect(out[i]).toBeCloseTo(line.levels[i], 4);
        }
      });
    });
  }

  it('refuses a predicted line when it has no history', () => {
    const testCase = cases[1];
    // Line 1 is temporally predicted; a fresh decoder must not guess.
    const decoder = new WaterfallDecoder();
    const out = new Float32Array(testCase.width);
    const temporal = testCase.lines.find((line) => !line.intra)!;
    expect(decoder.decode(fromHex(temporal.payload), testCase.width, out, temporal.zero_runs, temporal.adaptive, temporal.step_db)).toBe(false);
  });

  it('resynchronises on the intra line the server sends after a viewport change', () => {
    const testCase = cases[1];
    const intraIndex = testCase.lines.findIndex((line, index) => index > 0 && line.intra);
    expect(intraIndex).toBeGreaterThan(0);

    const decoder = new WaterfallDecoder();
    const out = new Float32Array(testCase.width);
    // Join mid-stream, as a client that just retuned would.
    expect(decoder.decode(fromHex(testCase.lines[intraIndex].payload), testCase.width, out,
      testCase.lines[intraIndex].zero_runs, testCase.lines[intraIndex].adaptive, testCase.lines[intraIndex].step_db)).toBe(true);
    for (let i = 0; i < testCase.width; i++) {
      expect(out[i]).toBeCloseTo(testCase.lines[intraIndex].levels[i], 4);
    }
  });
});

describe('bit I/O', () => {
  it('round-trips every code through its own writer', () => {
    const writer = new BitWriter();
    const values: number[] = [];
    let seed = 12345;
    const next = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff);

    for (let i = 0; i < 400; i++) {
      const value = next() % 5000 - 2500;
      values.push(value);
      writer.putSignedExpGolomb(value);
      writer.putSignedRice(value, i % 9);
    }
    const reader = new BitReader(writer.finish());
    for (let i = 0; i < values.length; i++) {
      expect(reader.signedExpGolomb()).toBe(values[i]);
      expect(reader.signedRice(i % 9)).toBe(values[i]);
    }
    expect(reader.overrun).toBe(false);
  });

  it('handles the Rice escape path', () => {
    const writer = new BitWriter();
    const values = [0, 1, 1 << 20, 0x7fffffff, 5];
    for (const value of values) writer.putRice(value, 2);
    const reader = new BitReader(writer.finish());
    for (const value of values) expect(reader.rice(2)).toBe(value);
  });

  it('flags overrun rather than reading past the end', () => {
    const reader = new BitReader(new Uint8Array([0xff, 0xff]));
    reader.bits(16);
    expect(reader.overrun).toBe(false);
    reader.bits(4);
    expect(reader.overrun).toBe(true);
  });
});

describe('band table', () => {
  it('covers every coefficient and matches the server layout', () => {
    let total = 0;
    BAND_WIDTHS.forEach((width, index) => {
      expect(BAND_STARTS[index]).toBe(total);
      total += width;
    });
    expect(total).toBe(FRAME_HOP);
  });

  it('derives the Rice parameter the same way the encoder does', () => {
    expect(riceKForQuality(0)).toBe(0);
    expect(riceKForQuality(4)).toBe(0);
    expect(riceKForQuality(8)).toBe(1);
    expect(riceKForQuality(63)).toBe(14);
  });
});

interface RangedCase {
  width: number;
  lines: { payload: string; key: boolean; step_db: number; levels: number[] }[];
}

describe('WFC5 range-coded waterfall rows', () => {
  const cases = vectors.waterfall_ranged as RangedCase[];

  for (const testCase of cases) {
    it(`matches the server at ${testCase.width} bins`, () => {
      const decoder = new WaterfallDecoder();
      const out = new Float32Array(testCase.width);
      testCase.lines.forEach((line, index) => {
        const ok = decoder.decode(fromHex(line.payload), testCase.width, out, false, false, line.step_db, true);
        expect(ok, `line ${index} should decode`).toBe(true);
        for (let i = 0; i < testCase.width; i++) expect(out[i]).toBe(line.levels[i]);
      });
    });
  }

  it('waits for the next key row after a lost row', () => {
    const testCase = cases[0];
    const decoder = new WaterfallDecoder();
    const out = new Float32Array(testCase.width);
    const lost = testCase.lines.findIndex((line, i) => i > 0 && !line.key);
    const nextKey = testCase.lines.findIndex((line, i) => i > lost && line.key);
    expect(lost).toBeGreaterThan(0);
    expect(nextKey).toBeGreaterThan(lost + 1);
    testCase.lines.forEach((line, index) => {
      if (index === lost) return;
      const ok = decoder.decode(fromHex(line.payload), testCase.width, out, false, false, line.step_db, true);
      expect(ok, `line ${index}`).toBe(index < lost || index >= nextKey);
    });
  });

  it('refuses reserved header bits and a row cut short', () => {
    const testCase = cases[1];
    const decoder = new WaterfallDecoder();
    const out = new Float32Array(testCase.width);
    const key = fromHex(testCase.lines[0].payload);
    const reserved = key.slice();
    reserved[0] |= 2;
    expect(decoder.decode(reserved, testCase.width, out, false, false, testCase.lines[0].step_db, true)).toBe(false);
    expect(decoder.decode(key.subarray(0, 2), testCase.width, out, false, false, testCase.lines[0].step_db, true)).toBe(false);
    expect(decoder.decode(key, testCase.width, out, false, false, testCase.lines[0].step_db, true)).toBe(true);
  });
});
