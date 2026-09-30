// Small vector kernels for the per-listener paths that run at a channel's
// rate rather than the audio's: broadcast FM is demodulated at 200 kHz and
// more, where a scalar loop over atan2 or a long FIR is most of a listener's
// cost. Chosen at run time on the same instruction set as the transforms
// (FftSplit::instruction_set(), which FERNSDR_FFT_ISA lowers for tests):
// eight or sixteen lanes on x86 with AVX2 or AVX-512, four with SSE2 or
// NEON, and plain loops otherwise. Results differ between them only in the
// order floating-point sums are taken.
#pragma once

#include <cstddef>

#include "fft.h"

namespace fernsdr::simd {

// The sum of a[i] * b[i] for i below n.
float dot(const float* a, const float* b, size_t n);

// dot(a, b0, n) and dot(a, b1, n) in one pass over a: a real signal against
// complex taps kept as their real and imaginary parts.
void dot2(const float* a, const float* b0, const float* b1, size_t n, float* out0, float* out1);

// For each sample, the angle from the one before to it, in radians in
// (-pi, pi]: the phase step an FM discriminator turns into audio. `previous`
// is the sample before in[0]. The angle comes from a polynomial, within
// 1.5e-5 rad of atan2 everywhere, which on a full-deviation broadcast step of
// 1.8 rad is 100 dB down.
void phase_steps(const cfloat* in, size_t n, cfloat previous, float* out);

// The polynomial atan2 on its own, for tests: y and x as for std::atan2.
float fast_atan2(float y, float x);

// out[i] = 10 log10(power[i] * scale + 1e-30) - offset_db: a spectrum's
// powers as decibels. Within 1e-4 dB of std::log10 for every finite power,
// which a waterfall drawn in whole decibels cannot show.
void power_to_db(const float* power, size_t n, float scale, float offset_db, float* out);

// acc[j] += |0.5 X[j] - 0.25 (X[j-1] + X[j+1])|^2 for j below n, X held as
// its real and imaginary parts: the power of a sine-windowed transform's
// bins seen through sin^3 (see ZoomSpectrum). re[-1] and re[n], and the
// same of im, are read.
void kernel_power(const float* re, const float* im, size_t n, float* acc);

// Which kernels are in use, as FftSplit::instruction_set() names them.
const char* instruction_set();

}  // namespace fernsdr::simd
