/**
 * NAC decoder, mirroring server/src/codec/nac_decoder.cpp.
 *
 * Runs on the audio thread. It is deliberately allocation-free after
 * construction: a garbage collection pause inside an AudioWorklet callback is
 * a click in the audio, which is exactly what this whole system exists to
 * avoid.
 */
import { BitReader } from './bitio';
import { Imdct, makeSineWindow } from './mdct';

export const FRAME_HOP = 128;
export const NUM_COEFFS = FRAME_HOP;

export const BAND_WIDTHS = [4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 16, 16, 16] as const;
export const NUM_BANDS = BAND_WIDTHS.length;

export const BAND_STARTS: number[] = (() => {
  const starts: number[] = [];
  let total = 0;
  for (const width of BAND_WIDTHS) {
    starts.push(total);
    total += width;
  }
  if (total !== NUM_COEFFS) throw new Error('band table must cover every coefficient');
  return starts;
})();

const QUALITY_BITS = 6;
// A coefficient this far outside normalised receiver audio is a corrupt frame,
// not a loud signal; playing it would be a full-scale burst, and it would stay
// in the overlap for the frame after. As in server/src/codec/nac_decoder.cpp.
const MAX_COEFFICIENT = 1e7;
const EXPONENT_REFERENCE = -40;

/** NAC3 step indices are quarter octaves of log2(step). */
const MIN_STEP = -200;
const MAX_STEP = 200;
const MAX_RICE = 15;
/** A NAC3 packet carries one to four frames. */
export const MAX_PACKET_FRAMES = 4;

/**
 * After a packet's first frame, every band it left silent predicts from the
 * nearest active band: the last one to its left, or the first active band for
 * bands before it. Mirrors fill_references in server/src/codec/nac.h.
 */
function fillReferences(active: Uint8Array, step: Int32Array, rice: Int32Array): void {
  let first = -1;
  let last = -1;
  for (let b = 0; b < NUM_BANDS; b++) {
    if (active[b]) {
      if (first < 0) first = b;
      last = b;
    } else if (last >= 0) {
      step[b] = step[last];
      rice[b] = rice[last];
    }
  }
  for (let b = 0; b < NUM_BANDS && (first < 0 || b < first); b++) {
    step[b] = first < 0 ? 0 : step[first];
    rice[b] = first < 0 ? 0 : rice[first];
  }
}

/** Derived from the quality alone, so no side information is needed. */
export function riceKForQuality(qualityIndex: number): number {
  return Math.max(0, Math.min(20, (qualityIndex >> 2) - 1));
}

export class NacDecoder {
  private readonly imdct = new Imdct(FRAME_HOP);
  private readonly window = makeSineWindow(2 * FRAME_HOP);
  private readonly coefficients = new Float32Array(NUM_COEFFS);
  private readonly previousCoefficients = new Float32Array(NUM_COEFFS);
  private readonly time = new Float32Array(2 * FRAME_HOP);
  private readonly overlap = new Float32Array(FRAME_HOP);
  private readonly active = new Uint8Array(NUM_BANDS);
  private readonly exponents = new Int32Array(NUM_BANDS);
  private readonly steps = new Int32Array(NUM_BANDS);
  private readonly rice = new Int32Array(NUM_BANDS);
  private readonly referenceStep = new Int32Array(NUM_BANDS);
  private readonly referenceRice = new Int32Array(NUM_BANDS);
  private readonly referenceActive = new Uint8Array(NUM_BANDS);
  private concealGain = 1;
  /** Whether every frame of the last NAC3 packet decoded. */
  lastPacketOk = false;

  reset(): void {
    this.overlap.fill(0);
    this.previousCoefficients.fill(0);
    this.concealGain = 1;
  }

  copyStateFrom(other: NacDecoder): void {
    this.overlap.set(other.overlap);
    this.previousCoefficients.set(other.previousCoefficients);
    this.concealGain = other.concealGain;
  }

  /**
   * Decodes one frame into exactly FRAME_HOP samples. Returns false on a
   * malformed frame, in which case `out` has been filled by concealment.
   */
  decode(payload: Uint8Array, out: Float32Array, compact = false): boolean {
    if (payload.length === 0) {
      this.conceal(out);
      return false;
    }

    const reader = new BitReader(payload);
    const quality = reader.bits(QUALITY_BITS);

    const maskMode = compact ? reader.bits(2) : 0;
    this.active.fill(0);
    if (maskMode === 0) {
      for (let b = 0; b < NUM_BANDS; b++) this.active[b] = reader.bit();
    } else if (maskMode === 1) {
      this.active.fill(1);
    } else {
      const first = maskMode === 3 ? reader.bits(5) : 0;
      const count = reader.bits(5);
      if (first >= NUM_BANDS || count > NUM_BANDS - first || (maskMode === 3 && count === 0)) {
        this.conceal(out);
        return false;
      }
      this.active.fill(1, first, first + count);
    }

    let previous = EXPONENT_REFERENCE;
    let first = true;
    let scaleMode = 0;
    for (let b = 0; b < NUM_BANDS; b++) {
      if (!this.active[b]) continue;
      if (compact && first) {
        previous = reader.bits(9) - 200;
        scaleMode = reader.bits(3);
      } else {
        previous += scaleMode ? reader.signedRice(scaleMode - 1) : reader.signedExpGolomb();
      }
      if (scaleMode > 5 || previous < -200 || previous > 200 || reader.overrun) {
        this.conceal(out);
        return false;
      }
      first = false;
      this.exponents[b] = previous;
    }

    const k = riceKForQuality(quality);
    this.coefficients.fill(0);
    for (let b = 0; b < NUM_BANDS; b++) {
      if (!this.active[b]) continue;
      const start = BAND_STARTS[b];
      const width = BAND_WIDTHS[b];
      const step = Math.pow(2, (this.exponents[b] - quality) * 0.25);
      for (let i = 0; i < width; i++) {
        const value = reader.signedRice(k) * step;
        if (!(Math.abs(value) <= MAX_COEFFICIENT)) {
          this.conceal(out);
          return false;
        }
        this.coefficients[start + i] = value;
      }
    }

    if (reader.overrun) {
      // Truncated or corrupt: conceal rather than play whatever partial
      // spectrum happened to parse.
      this.conceal(out);
      return false;
    }

    this.previousCoefficients.set(this.coefficients);
    this.concealGain = 1;
    this.synthesise(out);
    return true;
  }

  /**
   * Decodes a NAC3 packet into `out`, which holds `capacity` frames of
   * FRAME_HOP samples. Returns how many frames the packet carried, or 0 when
   * even its frame count is unreadable or exceeds `capacity`.
   * `lastPacketOk` says whether every frame decoded; a malformed frame and
   * the frames after it, which predict from it, are concealed, so the output
   * always holds whole frames on the sample clock.
   */
  decodePacket(payload: Uint8Array, out: Float32Array, capacity: number): number {
    this.lastPacketOk = false;
    if (payload.length === 0) return 0;
    const reader = new BitReader(payload);
    const frames = reader.bits(2) + 1;
    if (frames > capacity || out.length < frames * FRAME_HOP) return 0;
    let produced = 0;
    for (; produced < frames; produced++) {
      if (!this.decodePerBand(reader, produced > 0)) break;
      this.previousCoefficients.set(this.coefficients);
      this.concealGain = 1;
      this.synthesise(out, produced * FRAME_HOP);
    }
    this.lastPacketOk = produced === frames;
    for (let f = produced; f < frames; f++) this.concealAt(out, f * FRAME_HOP);
    return frames;
  }

  /**
   * One NAC3 frame: activity mask, a step index and a Rice parameter per
   * active band, then the coefficients. A later frame of a packet may repeat
   * the previous frame's mask with one bit, and codes its steps and Rice
   * parameters against the previous frame's. Mirrors decode_per_band in
   * server/src/codec/nac_decoder.cpp.
   */
  private decodePerBand(reader: BitReader, predicted: boolean): boolean {
    const active = this.active;
    if (predicted && reader.bit() === 1) {
      active.set(this.referenceActive);
    } else {
      const maskMode = reader.bits(2);
      active.fill(0);
      if (maskMode === 0) {
        for (let b = 0; b < NUM_BANDS; b++) active[b] = reader.bit();
      } else if (maskMode === 1) {
        active.fill(1);
      } else {
        const first = maskMode === 3 ? reader.bits(5) : 0;
        const count = reader.bits(5);
        if (first >= NUM_BANDS || count > NUM_BANDS - first || (maskMode === 3 && count === 0)) return false;
        active.fill(1, first, first + count);
      }
    }

    let activeCount = 0;
    for (let b = 0; b < NUM_BANDS; b++) activeCount += active[b];
    if (activeCount > 0) {
      let selector = predicted ? reader.bits(3) : 0;
      let first = true;
      let step = 0;
      for (let b = 0; b < NUM_BANDS; b++) {
        if (!active[b]) continue;
        if (predicted) {
          if (selector > 5) return false;
          step = this.referenceStep[b] + (selector ? reader.signedRice(selector - 1) : reader.signedExpGolomb());
        } else if (first) {
          step = reader.bits(9) + MIN_STEP;
          selector = reader.bits(3);
          if (selector > 5) return false;
        } else {
          step += selector ? reader.signedRice(selector - 1) : reader.signedExpGolomb();
        }
        first = false;
        if (step < MIN_STEP || step > MAX_STEP || reader.overrun) return false;
        this.steps[b] = step;
      }
      first = true;
      let parameter = 0;
      for (let b = 0; b < NUM_BANDS; b++) {
        if (!active[b]) continue;
        if (predicted) parameter = this.referenceRice[b] + reader.signedRice(0);
        else parameter = first ? reader.bits(4) : parameter + reader.signedRice(0);
        first = false;
        if (parameter < 0 || parameter > MAX_RICE || reader.overrun) return false;
        this.rice[b] = parameter;
      }
    }

    this.coefficients.fill(0);
    for (let b = 0; b < NUM_BANDS; b++) {
      if (!active[b]) continue;
      const start = BAND_STARTS[b];
      const width = BAND_WIDTHS[b];
      const stepSize = Math.fround(Math.pow(2, 0.25 * this.steps[b]));
      const k = this.rice[b];
      for (let i = 0; i < width; i++) {
        const value = reader.signedRice(k) * stepSize;
        if (!(Math.abs(value) <= MAX_COEFFICIENT)) return false;
        this.coefficients[start + i] = value;
      }
    }
    if (reader.overrun) return false;

    for (let b = 0; b < NUM_BANDS; b++) {
      if (active[b]) {
        this.referenceStep[b] = this.steps[b];
        this.referenceRice[b] = this.rice[b];
      }
      this.referenceActive[b] = active[b];
    }
    if (!predicted) fillReferences(active, this.referenceStep, this.referenceRice);
    return true;
  }

  /**
   * Fills one hop for a lost frame. A decaying repeat of the last spectrum,
   * never a time stretch: the output clock must stay at exactly one hop per
   * frame or anything riding on the audio loses symbol timing across the gap.
   */
  conceal(out: Float32Array): void {
    this.concealAt(out, 0);
  }

  private concealAt(out: Float32Array, offset: number): void {
    this.concealGain *= 0.5;
    for (let i = 0; i < NUM_COEFFS; i++) {
      this.coefficients[i] = this.previousCoefficients[i] * this.concealGain;
    }
    this.synthesise(out, offset);
  }

  private synthesise(out: Float32Array, offset = 0): void {
    this.imdct.inverse(this.coefficients, this.time);
    for (let i = 0; i < FRAME_HOP; i++) {
      out[offset + i] = this.overlap[i] + this.time[i] * this.window[i];
      this.overlap[i] = this.time[FRAME_HOP + i] * this.window[FRAME_HOP + i];
    }
  }
}
