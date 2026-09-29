/**
 * MSB-first bit reader with the entropy codes both stream formats use.
 *
 * This is a direct mirror of server/src/util/bitio.h. The two are kept in step
 * by cross-check vectors: the C++ encoder emits them, and bitio.test.ts decodes
 * them here. If the two ever drift apart, that test fails rather than the
 * audio quietly turning to noise.
 */

/** Unary run length at which Rice coding gives up and stores the value raw. */
export const RICE_ESCAPE = 24;

export function zigzagDecode(u: number): number {
  return (u >>> 1) ^ -(u & 1);
}

export function zigzagEncode(v: number): number {
  return ((v << 1) ^ (v >> 31)) >>> 0;
}

export class BitReader {
  private readonly data: Uint8Array;
  private position = 0;
  /** Set once a read ran past the end; the frame should then be discarded. */
  overrun = false;

  constructor(data: Uint8Array) {
    this.data = data;
  }

  get bitsRead(): number {
    return this.position;
  }

  bit(): number {
    if (this.position >= this.data.length * 8) {
      this.overrun = true;
      return 0;
    }
    const byte = this.data[this.position >> 3];
    const value = (byte >> (7 - (this.position & 7))) & 1;
    this.position++;
    return value;
  }

  bits(count: number): number {
    let value = 0;
    while (count > 0) {
      if (this.position >= this.data.length * 8) {
        this.overrun = true;
        // Missing bits are zero, and consuming them must not advance the
        // position. JavaScript masks shift counts, so 32 needs its own case.
        return count >= 32 ? 0 : (value << count) >>> 0;
      }
      const offset = this.position & 7;
      const take = Math.min(count, 8 - offset);
      const chunk = (this.data[this.position >> 3] >> (8 - offset - take)) & ((1 << take) - 1);
      value = ((value << take) | chunk) >>> 0;
      this.position += take;
      count -= take;
    }
    return value >>> 0;
  }

  rice(k: number): number {
    let q = 0;
    while (this.bit()) {
      q++;
      if (q >= RICE_ESCAPE) {
        this.bit(); // the escape still writes its terminating zero
        return this.bits(32);
      }
      if (this.overrun) return 0;
    }
    return ((q << k) >>> 0) + (k ? this.bits(k) : 0);
  }

  signedRice(k: number): number {
    return zigzagDecode(this.rice(k));
  }

  expGolomb(): number {
    let leading = 0;
    while (!this.bit()) {
      leading++;
      if (leading > 32 || this.overrun) {
        this.overrun = true;
        return 0;
      }
    }
    // The terminating one is the value's leading bit.
    let value = 1;
    for (let i = 0; i < leading; i++) value = (value << 1) | this.bit();
    return value - 1;
  }

  signedExpGolomb(): number {
    return zigzagDecode(this.expGolomb());
  }
}

/** Only the tests need to write bits; kept here so the two stay together. */
export class BitWriter {
  private bytes: number[] = [];
  private accumulator = 0;
  private count = 0;

  putBit(bit: number): void {
    this.accumulator = (this.accumulator << 1) | (bit & 1);
    if (++this.count === 8) {
      this.bytes.push(this.accumulator & 0xff);
      this.accumulator = 0;
      this.count = 0;
    }
  }

  putBits(value: number, count: number): void {
    for (let i = count; i-- > 0; ) this.putBit((value >>> i) & 1);
  }

  putRice(value: number, k: number): void {
    const q = value >>> k;
    if (q >= RICE_ESCAPE) {
      for (let i = 0; i < RICE_ESCAPE; i++) this.putBit(1);
      this.putBit(0);
      this.putBits(value, 32);
      return;
    }
    for (let i = 0; i < q; i++) this.putBit(1);
    this.putBit(0);
    if (k) this.putBits(value, k);
  }

  putSignedRice(value: number, k: number): void {
    this.putRice(zigzagEncode(value), k);
  }

  putExpGolomb(value: number): void {
    const v = value + 1;
    let bits = 0;
    while (v >>> bits > 1) bits++;
    for (let i = 0; i < bits; i++) this.putBit(0);
    this.putBits(v, bits + 1);
  }

  putSignedExpGolomb(value: number): void {
    this.putExpGolomb(zigzagEncode(value));
  }

  finish(): Uint8Array {
    while (this.count !== 0) this.putBit(0);
    return new Uint8Array(this.bytes);
  }
}
