/**
 * Inverse MDCT, mirroring server/src/dsp/mdct.cpp.
 *
 * Same factorisation as the server: a DCT-IV evaluated through a quarter-length
 * complex FFT, then the transposed fold. At M = 128 this is a few microseconds
 * per frame, which matters because it runs on the audio thread where a missed
 * deadline is an audible click.
 */

/** Sine window; satisfies w[n]^2 + w[n+M]^2 == 1 so overlap-add is exact. */
export function makeSineWindow(length: number): Float32Array {
  const w = new Float32Array(length);
  for (let n = 0; n < length; n++) w[n] = Math.sin((Math.PI / length) * (n + 0.5));
  return w;
}

/** In-place Stockham complex FFT over split real/imaginary arrays. */
class Fft {
  private readonly n: number;
  private readonly cos: Float64Array;
  private readonly sin: Float64Array;
  private readonly offsets: number[] = [];
  private readonly scratchRe: Float64Array;
  private readonly scratchIm: Float64Array;

  constructor(n: number) {
    this.n = n;
    const twiddles: number[][] = [];
    let total = 0;
    for (let l = n >> 1; l >= 1; l >>= 1) {
      this.offsets.push(total);
      const table: number[] = [];
      for (let j = 0; j < l; j++) {
        const angle = (-Math.PI * j) / l;
        table.push(Math.cos(angle), Math.sin(angle));
      }
      twiddles.push(table);
      total += l;
      if (l === 1) break;
    }
    this.cos = new Float64Array(total);
    this.sin = new Float64Array(total);
    let index = 0;
    for (const table of twiddles) {
      for (let j = 0; j < table.length; j += 2) {
        this.cos[index] = table[j];
        this.sin[index] = table[j + 1];
        index++;
      }
    }
    this.scratchRe = new Float64Array(n);
    this.scratchIm = new Float64Array(n);
  }

  forward(re: Float64Array, im: Float64Array): void {
    const n = this.n;
    if (n === 1) return;

    let sourceRe = re;
    let sourceIm = im;
    let targetRe = this.scratchRe;
    let targetIm = this.scratchIm;

    let l = n >> 1;
    let m = 1;
    let stage = 0;

    while (l >= 1) {
      const base = this.offsets[stage];
      for (let j = 0; j < l; j++) {
        const wr = this.cos[base + j];
        const wi = this.sin[base + j];
        const aOffset = j * m;
        const bOffset = (j + l) * m;
        const o0 = 2 * j * m;
        const o1 = o0 + m;
        for (let k = 0; k < m; k++) {
          const ur = sourceRe[aOffset + k];
          const ui = sourceIm[aOffset + k];
          const vr = sourceRe[bOffset + k];
          const vi = sourceIm[bOffset + k];
          targetRe[o0 + k] = ur + vr;
          targetIm[o0 + k] = ui + vi;
          const dr = ur - vr;
          const di = ui - vi;
          targetRe[o1 + k] = dr * wr - di * wi;
          targetIm[o1 + k] = dr * wi + di * wr;
        }
      }
      [sourceRe, targetRe] = [targetRe, sourceRe];
      [sourceIm, targetIm] = [targetIm, sourceIm];
      stage++;
      m *= 2;
      if (l === 1) break;
      l >>= 1;
    }

    if (sourceRe !== re) {
      re.set(sourceRe);
      im.set(sourceIm);
    }
  }
}

export class Imdct {
  private readonly m: number;
  private readonly quarter: number;
  private readonly fft: Fft;
  private readonly twiddleCos: Float64Array;
  private readonly twiddleSin: Float64Array;
  private readonly workRe: Float64Array;
  private readonly workIm: Float64Array;
  private readonly unfold: Float64Array;

  /** `half` is M: consumes M coefficients and produces 2M samples. */
  constructor(half: number) {
    this.m = half;
    this.quarter = half >> 1;
    this.fft = new Fft(this.quarter);
    this.twiddleCos = new Float64Array(this.quarter);
    this.twiddleSin = new Float64Array(this.quarter);
    for (let i = 0; i < this.quarter; i++) {
      const angle = (-Math.PI * (i + 0.125)) / this.m;
      this.twiddleCos[i] = Math.cos(angle);
      this.twiddleSin[i] = Math.sin(angle);
    }
    this.workRe = new Float64Array(this.quarter);
    this.workIm = new Float64Array(this.quarter);
    this.unfold = new Float64Array(this.m);
  }

  /** out[k] = sum_n in[n] cos(pi/M (n+0.5)(k+0.5)) */
  private dct4(input: Float32Array, output: Float64Array): void {
    const p = this.quarter;
    const m = this.m;
    for (let i = 0; i < p; i++) {
      const a = input[2 * i];
      const b = input[m - 1 - 2 * i];
      const wr = this.twiddleCos[i];
      const wi = this.twiddleSin[i];
      this.workRe[i] = a * wr - b * wi;
      this.workIm[i] = a * wi + b * wr;
    }
    this.fft.forward(this.workRe, this.workIm);
    for (let i = 0; i < p; i++) {
      const wr = this.twiddleCos[i];
      const wi = this.twiddleSin[i];
      const re = this.workRe[i];
      const im = this.workIm[i];
      output[2 * i] = re * wr - im * wi;
      output[m - 1 - 2 * i] = -(re * wi + im * wr);
    }
  }

  /**
   * Produces 2M samples from M coefficients, including the 2/M normalisation,
   * so window -> forward -> inverse -> window -> overlap-add is the identity.
   */
  inverse(coefficients: Float32Array, output: Float32Array): void {
    this.dct4(coefficients, this.unfold);

    const m = this.m;
    const q = m >> 1;
    const scale = 2 / m;
    for (let i = 0; i < q; i++) {
      output[i] = scale * this.unfold[q + i];
      output[q + i] = -scale * this.unfold[m - 1 - i];
      output[m + i] = -scale * this.unfold[q - 1 - i];
      output[m + q + i] = -scale * this.unfold[i];
    }
  }
}
