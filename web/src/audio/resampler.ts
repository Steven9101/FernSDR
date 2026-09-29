/**
 * Windowed-sinc polyphase resampler.
 *
 * The server's audio rate is the band rate divided by a power of two, so it is
 * rarely a round number and almost never equal to the browser's audio context
 * rate. Linear interpolation would be cheaper, but its images and high-
 * frequency droop are audible on a 12 kHz to 48 kHz upsample - and on a
 * receiver whose whole point is faithful audio, that is the wrong place to
 * save a few microseconds.
 */

const TAPS = 16;
const PHASES = 512;

function sinc(x: number): number {
  if (Math.abs(x) < 1e-9) return 1;
  const t = Math.PI * x;
  return Math.sin(t) / t;
}

/** Builds the polyphase kernel for a given ratio of output to input rate. */
export function buildKernel(inputRate: number, outputRate: number): Float32Array {
  // Cutoff sits at the lower of the two Nyquist limits, expressed in cycles
  // per input sample.
  const cutoff = 0.5 * Math.min(1, outputRate / inputRate);
  const table = new Float32Array(PHASES * TAPS);
  const center = TAPS / 2 - 1;

  for (let phase = 0; phase < PHASES; phase++) {
    const fraction = phase / PHASES;
    let sum = 0;
    for (let tap = 0; tap < TAPS; tap++) {
      const x = tap - center - fraction;
      // Blackman window: -58 dB sidelobes, which keeps resampler images well
      // below anything the codec left behind.
      const w = 0.42 - 0.5 * Math.cos((2 * Math.PI * tap) / (TAPS - 1)) +
                0.08 * Math.cos((4 * Math.PI * tap) / (TAPS - 1));
      const value = 2 * cutoff * sinc(2 * cutoff * x) * w;
      table[phase * TAPS + tap] = value;
      sum += value;
    }
    // Normalise each phase so a constant input gives a constant output; an
    // un-normalised kernel adds a faint whine at the phase-cycle rate.
    if (sum !== 0) {
      for (let tap = 0; tap < TAPS; tap++) table[phase * TAPS + tap] /= sum;
    }
  }
  return table;
}

export const KERNEL_TAPS = TAPS;
export const KERNEL_PHASES = PHASES;
export const KERNEL_HISTORY = TAPS;

/**
 * Reads one sample from `ring` at fractional position `position`.
 * `ring` is a circular buffer of `size` entries.
 */
export function resampleAt(
  ring: Float32Array,
  size: number,
  position: number,
  kernel: Float32Array,
): number {
  const base = Math.floor(position);
  const fraction = position - base;
  const phase = (fraction * KERNEL_PHASES) | 0;
  const offset = phase * KERNEL_TAPS;
  const start = base - (KERNEL_TAPS / 2 - 1);

  let accumulator = 0;
  let index = ((start % size) + size) % size;
  for (let tap = 0; tap < KERNEL_TAPS; tap++) {
    accumulator += ring[index] * kernel[offset + tap];
    if (++index === size) index = 0;
  }
  return accumulator;
}
