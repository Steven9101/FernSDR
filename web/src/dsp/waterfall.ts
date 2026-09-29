/**
 * Waterfall line decoder, mirroring server/src/codec/waterfall_codec.cpp.
 */
import { BitReader } from './bitio';

export const DB_STEP = 1.0;
const INTRA_SEED_Q = -100;
const MODE_BITS = 1;
const RICE_PARAM_BITS = 4;
const MODE_INTRA = 1;
const MODE_GRADIENT = 2;
const MODE_LINEAR = 3;

/** Rows between WFC5 key rows; mirrors kKeyEvery in waterfall_rc.h. */
export const RANGED_KEY_EVERY = 24;
const PROB_BITS = 11;
const ADAPT = 5;
const TOP = 1 << 24;
const UNARY = 12;

function bucket(v: number): number {
  v = Math.abs(v);
  return v === 0 ? 0 : v === 1 ? 1 : v <= 3 ? 2 : 3;
}

/** LZMA's range decoder, 32-bit unsigned, without the always-zero first byte. */
class RangeDecoder {
  private range = 0xffffffff;
  private code = 0;
  private pos = 0;
  past = 0;

  constructor(private readonly data: Uint8Array, start: number) {
    this.pos = start;
    for (let i = 0; i < 4; i++) this.code = this.code * 256 + this.next();
  }

  private next(): number {
    if (this.pos < this.data.length) return this.data[this.pos++];
    this.past++;
    return 0;
  }

  private normalize(): void {
    while (this.range < TOP) {
      this.range *= 256;
      this.code = this.code * 256 + this.next();
    }
  }

  bit(probs: Uint16Array, index: number): number {
    const p = probs[index];
    const bound = (this.range >>> PROB_BITS) * p;
    let b: number;
    if (this.code < bound) {
      this.range = bound;
      probs[index] = p + (((1 << PROB_BITS) - p) >> ADAPT);
      b = 0;
    } else {
      this.code -= bound;
      this.range -= bound;
      probs[index] = p - (p >> ADAPT);
      b = 1;
    }
    this.normalize();
    return b;
  }

  direct(bits: number): number {
    let value = 0;
    while (bits-- > 0) {
      this.range = this.range >>> 1;
      let b = 0;
      if (this.code >= this.range) { this.code -= this.range; b = 1; }
      value = value * 2 + b;
      this.normalize();
    }
    return value;
  }

  get overrun(): boolean {
    return this.past > 4;
  }
}

/** The adaptive probabilities, laid out as in RangedModel (waterfall_rc.h). */
class RangedModel {
  zero = new Uint16Array(5 * 4 * 4);
  sign = new Uint16Array(5 * 4 * 3);
  magnitude = new Uint16Array(5 * 5 * 3);
  reset(): void {
    this.zero.fill(1024);
    this.sign.fill(1024);
    this.magnitude.fill(1024);
  }
}

export class WaterfallDecoder {
  private previous: Int32Array | null = null;
  private previousStep = 1;
  private readonly model = new RangedModel();
  private rangedPrevious: Int32Array | null = null;
  private rangedStep = 1;

  reset(): void {
    this.previous = null;
    this.rangedPrevious = null;
  }

  /** WFC5 rows, mirroring RangedLineDecoder in server/src/codec/waterfall_rc.cpp. */
  private decodeRanged(payload: Uint8Array, width: number, out: Float32Array, stepDb: number): boolean {
    const header = payload[0];
    if ((header & ~1) !== 0) { this.reset(); return false; }
    const key = (header & 1) !== 0;
    if (!key && (this.rangedPrevious === null || this.rangedPrevious.length !== width || this.rangedStep !== stepDb)) {
      this.reset();
      return false;
    }
    if (key) this.model.reset();
    const prev = key ? null : this.rangedPrevious;
    const low = -200 / stepDb, high = 100 / stepDb, seed = INTRA_SEED_Q / stepDb;
    const rc = new RangeDecoder(payload, 1);
    const line = new Int32Array(width);
    const m = this.model;
    let last = 0;
    for (let i = 0; i < width; i++) {
      const left = i ? line[i - 1] : (prev ? prev[0] : seed);
      let aboveBucket = 4, direction = 3;
      if (prev) {
        const above = prev[i];
        aboveBucket = bucket(above - left);
        direction = above > left ? 1 : above < left ? 2 : 0;
      }
      const lastBucket = bucket(last), lastSign = last > 0 ? 1 : last < 0 ? 2 : 0;
      let residual = 0;
      if (rc.bit(m.zero, (aboveBucket * 4 + lastBucket) * 4 + direction)) {
        const negative = rc.bit(m.sign, (aboveBucket * 4 + direction) * 3 + lastSign) === 1;
        const agree = !prev || prev[i] === left ? 0 : (prev[i] > left) === !negative ? 1 : 2;
        let magnitude = 0;
        while (magnitude < UNARY && rc.bit(m.magnitude, ((magnitude < 4 ? magnitude : 4) * 5 + aboveBucket) * 3 + agree)) {
          magnitude++;
        }
        if (magnitude === UNARY) {
          const bits = rc.direct(4);
          if (bits > 9) { this.reset(); return false; }
          magnitude += (2 ** bits + rc.direct(bits)) - 1;
        }
        residual = negative ? -(magnitude + 1) : magnitude + 1;
      }
      const value = left + residual;
      if (value < low || value > high || rc.overrun) { this.reset(); return false; }
      line[i] = value;
      last = residual;
    }
    this.rangedPrevious = line;
    this.rangedStep = stepDb;
    for (let i = 0; i < width; i++) out[i] = line[i] * stepDb;
    return true;
  }

  /**
   * Decodes one line into `out` (which must hold `width` entries, in dBFS).
   * Returns false for a malformed line, or a temporally-predicted line with no
   * usable history - which happens after a viewport change and is why the
   * server sends an intra line whenever the span moves.
   */
  decode(payload: Uint8Array, width: number, out: Float32Array, zeroRuns = false, adaptive = false, stepDb = 1,
         rangeCoded = false): boolean {
    if (payload.length === 0 || !Number.isInteger(width) || width < 1 || width > 4096 || out.length < width ||
        (stepDb !== 1 && stepDb !== 2)) { this.reset(); return false; }
    if (rangeCoded) return this.decodeRanged(payload, width, out, stepDb);

    const reader = new BitReader(payload);
    const mode = reader.bits(adaptive ? 2 : MODE_BITS);
    const k = reader.bits(RICE_PARAM_BITS);

    if ((mode === 0 || mode === MODE_GRADIENT) &&
        (this.previous === null || this.previous.length !== width || this.previousStep !== stepDb)) {
      this.reset();
      return false;
    }

    const low = -200 / stepDb, high = 100 / stepDb, seed = INTRA_SEED_Q / stepDb;
    const line = new Int32Array(width);
    for (let i = 0; i < width;) {
      const run = zeroRuns && reader.bit() === 0;
      const count = run ? reader.expGolomb() + 1 : 1;
      if (count < 1 || count > width - i || reader.overrun) { this.reset(); return false; }
      const residual = run ? 0 : reader.signedRice(k);
      for (const end = i + count; i < end; i++) {
        const left = i ? line[i - 1] : seed;
        let predictor: number;
        if (mode === MODE_INTRA) predictor = left;
        else if (mode === MODE_LINEAR) predictor = i > 1 ? Math.max(low, Math.min(high, 2 * left - line[i - 2])) : left;
        else if (mode === 0 || i === 0) predictor = this.previous![i];
        else {
          const above = this.previous![i], diagonal = this.previous![i - 1];
          predictor = diagonal >= Math.max(left, above) ? Math.min(left, above) :
            diagonal <= Math.min(left, above) ? Math.max(left, above) : left + above - diagonal;
        }
        const value = predictor + residual;
        if (value < low || value > high || reader.overrun) { this.reset(); return false; }
        line[i] = value;
      }
    }

    if (reader.overrun) return false;

    this.previous = line;
    this.previousStep = stepDb;
    for (let i = 0; i < width; i++) out[i] = line[i] * stepDb;
    return true;
  }
}
