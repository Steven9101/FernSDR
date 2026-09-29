#include "fft.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#if defined(__SSE2__) && !defined(FERNSDR_SCALAR)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && !defined(FERNSDR_SCALAR)
#include <arm_neon.h>
#endif

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Fft::Fft(size_t n) : n_(n) {
    if (n == 0 || (n & (n - 1)) != 0) throw std::invalid_argument("Fft size must be a power of two");

    stages_ = 0;
    for (size_t t = n; t > 1; t >>= 1) stages_++;

    // Stage with half-span `l` needs l twiddles; the spans halve each stage so
    // the whole set fits in n-1 entries.
    twiddles_.reserve(n);
    stage_offset_.reserve(stages_);
    for (size_t l = n / 2; l >= 1; l >>= 1) {
        stage_offset_.push_back(twiddles_.size());
        for (size_t j = 0; j < l; j++) {
            double angle = -kPi * static_cast<double>(j) / static_cast<double>(l);
            twiddles_.emplace_back(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        }
        if (l == 1) break;
    }
    scratch_.resize(n);
}

// Stockham autosort: ping-pongs between the caller's buffer and scratch, which
// removes the bit-reversal permutation entirely.  With an even number of
// stages the result lands back in `data`; with an odd number we copy once.
void Fft::run(cfloat* data, bool conjugate) const {
    if (n_ == 1) return;

    cfloat* src = data;
    cfloat* dst = scratch_.data();

    size_t l = n_ / 2;  // number of distinct twiddles this stage
    size_t m = 1;       // stride of contiguous runs
    size_t stage = 0;

    while (l >= 1) {
        const cfloat* tw = twiddles_.data() + stage_offset_[stage];
        for (size_t j = 0; j < l; j++) {
            cfloat w = tw[j];
            if (conjugate) w = std::conj(w);
            const cfloat* a = src + j * m;
            const cfloat* b = src + (j + l) * m;
            cfloat* o0 = dst + 2 * j * m;
            cfloat* o1 = o0 + m;
            size_t k = 0;
#if defined(__SSE2__) && !defined(FERNSDR_SCALAR)
            // std::complex<float> has interleaved real/imaginary storage.
            // Two butterflies share one twiddle; SSE2 is part of x86-64 and
            // avoids making the binary depend on the build machine's AVX ISA.
            const __m128 wr = _mm_set1_ps(w.real());
            const __m128 wi = _mm_setr_ps(-w.imag(), w.imag(), -w.imag(), w.imag());
            for (; k + 1 < m; k += 2) {
                const __m128 u = _mm_loadu_ps(reinterpret_cast<const float*>(a + k));
                const __m128 v = _mm_loadu_ps(reinterpret_cast<const float*>(b + k));
                const __m128 d = _mm_sub_ps(u, v);
                const __m128 swapped = _mm_shuffle_ps(d, d, _MM_SHUFFLE(2, 3, 0, 1));
                _mm_storeu_ps(reinterpret_cast<float*>(o0 + k), _mm_add_ps(u, v));
                _mm_storeu_ps(reinterpret_cast<float*>(o1 + k),
                             _mm_add_ps(_mm_mul_ps(d, wr), _mm_mul_ps(swapped, wi)));
            }
#elif defined(__ARM_NEON) && !defined(FERNSDR_SCALAR)
            const float32x4_t wr = vdupq_n_f32(w.real());
            const float signs[] = {-w.imag(), w.imag(), -w.imag(), w.imag()};
            const float32x4_t wi = vld1q_f32(signs);
            for (; k + 1 < m; k += 2) {
                const float32x4_t u = vld1q_f32(reinterpret_cast<const float*>(a + k));
                const float32x4_t v = vld1q_f32(reinterpret_cast<const float*>(b + k));
                const float32x4_t d = vsubq_f32(u, v);
                vst1q_f32(reinterpret_cast<float*>(o0 + k), vaddq_f32(u, v));
                vst1q_f32(reinterpret_cast<float*>(o1 + k),
                          vaddq_f32(vmulq_f32(d, wr), vmulq_f32(vrev64q_f32(d), wi)));
            }
#endif
            for (; k < m; k++) {
                cfloat u = a[k];
                cfloat v = b[k];
                o0[k] = u + v;
                o1[k] = (u - v) * w;
            }
        }
        std::swap(src, dst);
        stage++;
        m *= 2;
        if (l == 1) break;
        l /= 2;
    }

    if (src != data) std::memcpy(data, src, n_ * sizeof(cfloat));
}

void Fft::forward(cfloat* data) const { run(data, false); }

void Fft::inverse_unscaled(cfloat* data) const { run(data, true); }

void Fft::inverse(cfloat* data) const {
    run(data, true);
    const float scale = 1.0f / static_cast<float>(n_);
    for (size_t i = 0; i < n_; i++) data[i] *= scale;
}

}  // namespace fernsdr
