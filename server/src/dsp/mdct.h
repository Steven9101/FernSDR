// MDCT / IMDCT with 50% overlap, built on a quarter-length complex FFT.
//
// The transform is the textbook one:
//   X[k] = sum_{n=0}^{2M-1} x[n] * cos(pi/M * (n + 0.5 + M/2) * (k + 0.5))
// factored as a time-domain fold followed by a DCT-IV, with the DCT-IV in
// turn evaluated by an M/2-point complex FFT.  tests/test_mdct.cpp checks
// both stages against the direct sums and checks that window + overlap-add
// reconstructs exactly.
#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "fft_split.h"

namespace fernsdr {

// Sine window; satisfies the Princen-Bradley condition w[n]^2 + w[n+M]^2 == 1
// so that overlap-add reconstruction is exact.
std::vector<float> make_sine_window(size_t length);

class Mdct {
public:
    // `half` is M: the transform consumes 2M samples and produces M
    // coefficients.  Must be a power of two, at least 4.
    explicit Mdct(size_t half);

    size_t half() const { return m_; }
    size_t window_length() const { return 2 * m_; }

    // `input` holds 2M already-windowed samples; `output` receives M coefficients.
    void forward(const float* input, float* output) const;

    // `input` holds M coefficients; `output` receives 2M samples, ready to be
    // windowed and overlap-added.  Includes the 2/M normalisation, so
    // window -> forward -> inverse -> window -> overlap-add is the identity.
    void inverse(const float* input, float* output) const;

    // The DCT-IV's twiddles for one length, shared like the FFT's tables.
    struct Twiddles {
        std::vector<float> re;
        std::vector<float> im;
    };

private:
    void dct4(const float* in, float* out, float* re, float* im) const;

    size_t m_;
    size_t quarter_;  // M/2, the complex FFT length
    // Split arrays: the interleaved transform took about 1.5 times as long
    // at this size, and every listener's encoder runs one per frame.
    FftSplit fft_;
    // Nothing else is kept per plan. A thousand listeners' encoders each
    // holding their own copy of the same twiddles and scratch arrays found
    // them cold in the cache at every frame; one copy, and scratch per
    // thread, stay warm across all of them.
    std::shared_ptr<const Twiddles> twiddles_;
};

}  // namespace fernsdr
