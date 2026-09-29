// Split-format FFT: real and imaginary parts in separate arrays.
//
// The interleaved std::complex<float> layout the rest of the code uses is
// convenient but defeats the vectoriser: every butterfly's real and imaginary
// work lands in the same SIMD lane group, so the compiler emits scalar code.
// Split arrays let a whole vector of lanes run independent butterflies. The
// kernels (fft_kernels.h) are four-lane on SSE2 and NEON, eight-lane with FMA
// on AVX2 machines and sixteen-lane with AVX-512 on AMD, chosen at run time;
// other targets run them one lane wide.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp_buffer.h"

namespace fernsdr {

class FftSplit {
public:
    // `n` must be a power of two.
    explicit FftSplit(size_t n);
    // A copy is a new plan of the same length with scratch of its own.
    FftSplit(const FftSplit& other);
    FftSplit& operator=(const FftSplit& other);
    ~FftSplit();

    size_t size() const { return n_; }

    // In-place forward transform, exp(-2*pi*i*k*n/N), unnormalised.
    void forward(float* re, float* im) const;
    // In-place inverse, scaled by 1/N.
    void inverse(float* re, float* im) const;
    // In-place inverse without the 1/N scaling.
    void inverse_unscaled(float* re, float* im) const;

    // The forward transform of a windowed complex front end: element e is
    // window[e] times sample e, where `older` holds samples 0 .. n/2 - 1 and
    // `newer` the rest, both as interleaved (re, im) pairs the way
    // std::complex<float> stores them. The bins go to re/im, which must not
    // overlap the inputs. The first pass reads the pairs and applies the
    // window itself, so a channelizer needs no sliding history, no split
    // copy and no windowed copy of its block.
    void forward_windowed(const float* older, const float* newer, const float* window, float* re,
                          float* im) const;

    // The instruction set the transforms run on: "avx512", "avx2", "sse2",
    // "neon" or "scalar". FERNSDR_FFT_ISA, read once, lowers it, so that tests
    // and benchmarks reach the narrower paths, or asks for AVX-512 on an
    // Intel CPU that has it (see detect_isa for why that is not the default).
    static const char* instruction_set();

    // Twiddles and layout, shared by every plan of one length: a thousand
    // listeners on one channel length read one copy instead of a thousand.
    struct Tables;

private:
    friend class RealFft;
    // The transform of a real signal's samples packed in pairs, element m
    // being (w[2m] s[2m], w[2m+1] s[2m+1]): `older` holds the first n
    // samples, `newer` the next n, `window` 2n weights or null. Plans with
    // vectors pack inside their first pass; plain ones pack first.
    void forward_packed(const float* older, const float* newer, const float* window, float* re, float* im) const;

    // The rows between the two passes, and the vectors each pass works on.
    struct Scratch {
        float* re;
        float* im;
        float* block;  // null when the plan has no use for one
    };
    Scratch scratch() const;

    // Long plans (Outer16 in fft_kernels.h): true when this plan runs a
    // radix-16 pass and sixteen shorter transforms. packed_blocks leaves the
    // real transform of forward_packed in the scratch in block order, for
    // RealFft to untangle straight into natural order.
    bool outer() const;
    void packed_blocks(const float* older, const float* newer, const float* window) const;
    void transform_blocks() const;

    size_t n_;
    std::shared_ptr<const Tables> tables_;
    std::unique_ptr<FftSplit> sub_;  // the sixteenth-length plan of a long one
    size_t block_floats_ = 0;
    // A long plan keeps its scratch, sized for it and on huge pages; a
    // listener's transforms borrow the calling thread's (see scratch()).
    mutable DspVector<float> scratch_re_;
    mutable DspVector<float> scratch_im_;
    mutable std::vector<float> block_;
};

// Real-input FFT of size `n`, producing the n/2+1 unique bins of a real
// signal from an n/2-point complex transform.
//
// Real front ends (an RX-888 sampling HF directly, for instance) hand us real
// samples, and taking only the positive-frequency bins is what turns them into
// the analytic signal the channelizer needs - the Hilbert transform comes free
// with the transform we were doing anyway.
class RealFft {
public:
    explicit RealFft(size_t n);

    size_t size() const { return n_; }
    // Number of unique output bins: n/2 + 1.
    size_t bins() const { return n_ / 2 + 1; }

    // `input` holds n real samples. `out_re`/`out_im` receive bins() values.
    void forward(const float* input, float* out_re, float* out_im) const;

    // Window two consecutive n/2-sample blocks while packing the transform.
    // Analytic output doubles interior bins, leaving DC and Nyquist unchanged.
    void forward_windowed_halves(const float* older, const float* newer, const float* window,
                                 float* out_re, float* out_im, bool analytic = false) const;

    // The same with the channelizer's window, window[i] = sin(pi (i + 1/2) / n),
    // which a long plan computes from small tables instead of reading: at a
    // million points the window is as large as the samples. Shorter plans
    // read `window`, which must be that window.
    void forward_sine_windowed_halves(const float* older, const float* newer, const float* window,
                                      float* out_re, float* out_im, bool analytic = false) const;

private:
    void untangle(float* out_re, float* out_im, float interior_scale) const;
    void build_sine_tables() const;
    size_t n_;
    FftSplit half_;
    mutable DspVector<float> work_re_;
    mutable DspVector<float> work_im_;
    DspVector<float> untangle_re_;
    DspVector<float> untangle_im_;
    // For a long half plan: W_n^(16 q) for q <= n / 64, and W_n^b for b < 16.
    std::vector<float> blocks_coarse_re_, blocks_coarse_im_, blocks_fine_re_, blocks_fine_im_;
    // The sine window in pieces (SineTables in fft_kernels.h), built on first use.
    mutable DspVector<float> window_sin_even_, window_cos_even_, window_sin_odd_, window_cos_odd_;
    mutable std::vector<float> window_sin_r_, window_cos_r_;
};

}  // namespace fernsdr
