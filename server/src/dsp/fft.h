// Self-contained complex FFT on interleaved std::complex data, a Stockham
// autosort transform. The receiver's own transforms run on FftSplit
// (fft_split.h), which is faster; this one remains as a separate
// implementation that the tests check FftSplit and RealFft against.
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace fernsdr {

using cfloat = std::complex<float>;

class Fft {
public:
    // `n` must be a power of two.
    explicit Fft(size_t n);

    size_t size() const { return n_; }

    // In-place forward transform, exp(-2*pi*i*k*n/N), unnormalised.
    void forward(cfloat* data) const;
    // In-place inverse transform, exp(+2*pi*i*k*n/N).  Scaled by 1/N so that
    // inverse(forward(x)) == x.
    void inverse(cfloat* data) const;
    // Inverse without the 1/N scaling, for callers that fold the gain
    // elsewhere (the channelizer does).
    void inverse_unscaled(cfloat* data) const;

private:
    void run(cfloat* data, bool conjugate) const;

    size_t n_;
    size_t stages_;
    std::vector<cfloat> twiddles_;      // concatenated per-stage tables
    std::vector<size_t> stage_offset_;  // where each stage's table starts
    mutable std::vector<cfloat> scratch_;
};

}  // namespace fernsdr
