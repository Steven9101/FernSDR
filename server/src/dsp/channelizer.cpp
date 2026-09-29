#include "channelizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// The synthesis window, the analysis window's shape sampled at the output
// rate. Every channel of one length reads the same copy, which stays in the
// cache for all of them. Kept for the life of the process: there are only as
// many as there are channel lengths.
const float* shared_synthesis_window(size_t length) {
    static std::mutex mutex;
    static std::map<size_t, std::unique_ptr<std::vector<float>>> windows;
    std::lock_guard<std::mutex> lock(mutex);
    std::unique_ptr<std::vector<float>>& slot = windows[length];
    if (!slot) {
        slot = std::make_unique<std::vector<float>>(length);
        for (size_t i = 0; i < length; i++) {
            (*slot)[i] = static_cast<float>(std::sin(kPi * (i + 0.5) / static_cast<double>(length)));
        }
    }
    return slot->data();
}

// What one pull works on and no pull keeps: the channel's slice of the
// spectrum on its way through the inverse transform. One set per thread, as
// with the transforms' own scratch.
struct PullScratch {
    std::vector<float> re;
    std::vector<float> im;
};

PullScratch& pull_scratch(size_t length) {
    thread_local PullScratch scratch;
    if (scratch.re.size() < length) {
        scratch.re.resize(length);
        scratch.im.resize(length);
    }
    return scratch;
}
}  // namespace

Channelizer::Channelizer(double sample_rate, size_t fft_size, SignalKind kind)
    : sample_rate_(sample_rate),
      fft_size_(fft_size),
      kind_(kind),
      fft_(kind == SignalKind::Iq ? fft_size : 1),
      real_fft_(kind == SignalKind::Real ? fft_size : 4) {
    if (sample_rate <= 0.0) throw std::invalid_argument("Channelizer sample rate must be positive");

    // Sine window: paired with the same window on synthesis and 50% overlap,
    // sin^2 + cos^2 = 1, so the analysis/synthesis pair reconstructs exactly.
    analysis_window_.resize(fft_size_);
    for (size_t i = 0; i < fft_size_; i++) {
        analysis_window_[i] = static_cast<float>(std::sin(kPi * (i + 0.5) / fft_size_));
    }

    // Zeroed once. For a real input the upper half is never written again, so
    // a slice reaching past Nyquist reads silence instead of an alias.
    spectrum_re_.assign(fft_size_, 0.0f);
    spectrum_im_.assign(fft_size_, 0.0f);
}

void Channelizer::process(const cfloat* input) {
    if (kind_ != SignalKind::Iq) return;
    if (previous_.empty()) previous_.assign(fft_size_ / 2, cfloat(0.0f, 0.0f));
    process_pair(previous_.data(), input);
    std::memcpy(previous_.data(), input, fft_size_ / 2 * sizeof(cfloat));
}

void Channelizer::process_real(const float* input) {
    if (kind_ != SignalKind::Real) return;
    if (history_re_.empty()) history_re_.assign(fft_size_ / 2, 0.0f);
    process_real_pair(history_re_.data(), input);
    std::memcpy(history_re_.data(), input, fft_size_ / 2 * sizeof(float));
}

void Channelizer::process_pair(const cfloat* older, const cfloat* newer) {
    if (kind_ != SignalKind::Iq) return;
    // The previous block is this transform's older half, which is what makes
    // the second half of each output valid. The transform reads both blocks
    // as they are and windows them itself, so nothing slides, splits or
    // windows a copy first.
    fft_.forward_windowed(reinterpret_cast<const float*>(older), reinterpret_cast<const float*>(newer),
                          analysis_window_.data(), spectrum_re_.data(), spectrum_im_.data());
    block_index_++;
}

void Channelizer::process_real_pair(const float* older, const float* newer) {
    if (kind_ != SignalKind::Real) return;
    // Pack the old and new halves directly into the real transform, applying
    // the analysis window there, so there is no windowed copy either.
    real_fft_.forward_sine_windowed_halves(older, newer, analysis_window_.data(), spectrum_re_.data(),
                                           spectrum_im_.data(), true);
    block_index_++;
}

Channel::Channel(const Channelizer& parent, size_t ifft_size)
    : fft_size_(parent.fft_size()),
      ifft_size_(ifft_size),
      sample_rate_(parent.sample_rate()),
      bin_hz_(parent.bin_hz()),
      output_rate_(parent.sample_rate() * static_cast<double>(ifft_size) /
                   static_cast<double>(parent.fft_size())),
      ifft_(ifft_size) {
    if (ifft_size < 8 || (ifft_size & (ifft_size - 1)) != 0)
        throw std::invalid_argument("Channel ifft_size must be a power of two >= 8");
    if (parent.fft_size() % ifft_size != 0)
        throw std::invalid_argument("Channel ifft_size must divide the channelizer FFT size");

    mask_.assign(ifft_size_, 0.0f);
    synthesis_window_ = shared_synthesis_window(ifft_size_);
    tail_re_.assign(ifft_size_ / 2, 0.0f);
    tail_im_.assign(ifft_size_ / 2, 0.0f);
    set_passband(0.0, -1500.0, 1500.0);
}

void Channel::set_passband(double center_hz, double low_hz, double high_hz) {
    if (high_hz < low_hz) std::swap(low_hz, high_hz);
    center_hz_ = center_hz;
    low_hz_ = low_hz;
    high_hz_ = high_hz;

    // Split the tuning into an integer bin offset (free: a circular shift of
    // the slice) and a sub-bin remainder (corrected by a rotation on the
    // decimated output, which is cheap at the low rate).
    const double bins = center_hz / bin_hz_;
    center_bin_ = static_cast<long>(std::llround(bins));
    residual_hz_ = center_hz - static_cast<double>(center_bin_) * bin_hz_;
    step_ = -kTwoPi * residual_hz_ / output_rate_;
    step_re_ = std::cos(step_);
    step_im_ = std::sin(step_);
    step4_re_ = std::cos(4.0 * step_);
    step4_im_ = std::sin(4.0 * step_);
    mask_dirty_ = true;
}

void Channel::rebuild_mask() {
    // Built around the integer-bin centre but offset by the sub-bin residual,
    // so that after the time-domain rotation removes that residual the
    // passband lands exactly where it was asked to.
    const double lo = low_hz_ + residual_hz_;
    const double hi = high_hz_ + residual_hz_;

    const double width = hi - lo;
    // The transition cannot be narrower than the filter length allows. With an
    // impulse response limited to L/2 taps, that floor is roughly two output
    // bins; asking for less just buys ripple.
    const double transition = std::max(3.0 * bin_hz_, std::min(0.15 * width, 300.0));

    const long half = static_cast<long>(ifft_size_ / 2);
    for (long j = -half; j < half; j++) {
        const double f = static_cast<double>(j) * bin_hz_;
        double gain;
        if (f <= lo - transition / 2 || f >= hi + transition / 2) {
            gain = 0.0;
        } else {
            // The cutoffs are the middle of each transition. Previously they
            // marked full gain, leaving almost all of a nearby excluded tone
            // audible outside the filter shown in the browser. A finite
            // response still has a transition; it cannot be a brick wall.
            const double rising = std::clamp((f - lo) / transition + 0.5, 0.0, 1.0);
            const double falling = std::clamp((hi - f) / transition + 0.5, 0.0, 1.0);
            gain = 0.25 * (1.0 - std::cos(kPi * rising)) * (1.0 - std::cos(kPi * falling));
        }
        const size_t slot =
            static_cast<size_t>((j + static_cast<long>(ifft_size_)) % static_cast<long>(ifft_size_));
        mask_[slot] = static_cast<float>(gain);
    }

    // Overlap-save only produces a valid second half if the filter's impulse
    // response is shorter than that half. A mask drawn straight onto the bin
    // grid has a response as long as the whole transform, so the tail wraps
    // around and aliases back into the output - inaudible as an obvious fault,
    // but it put a hard ceiling near 24 dB on the whole receive chain.
    //
    // So the mask is turned into an actual filter: transform it to the time
    // domain, truncate it to half the transform with a smooth window, and
    // transform back. What comes out is realisable, and the aliasing is gone.
    std::vector<float> impulse_re(ifft_size_);
    std::vector<float> impulse_im(ifft_size_, 0.0f);
    for (size_t i = 0; i < ifft_size_; i++) impulse_re[i] = mask_[i];
    ifft_.inverse(impulse_re.data(), impulse_im.data());

    const size_t keep = ifft_size_ / 2;      // taps the overlap allows
    const size_t half_keep = keep / 2;
    for (size_t i = 0; i < ifft_size_; i++) {
        // Distance from zero delay, wrapping: the response is centred on n = 0.
        const size_t distance = std::min(i, ifft_size_ - i);
        if (distance >= half_keep) {
            impulse_re[i] = 0.0f;
            impulse_im[i] = 0.0f;
            continue;
        }
        // Blackman taper over the kept span, which trades a slightly wider
        // transition for much lower sidelobes.
        const double x = static_cast<double>(distance) / static_cast<double>(half_keep);
        const double window = 0.42 + 0.5 * std::cos(kPi * x) + 0.08 * std::cos(2.0 * kPi * x);
        impulse_re[i] *= static_cast<float>(window);
        impulse_im[i] *= static_cast<float>(window);
    }

    ifft_.forward(impulse_re.data(), impulse_im.data());
    double passed = 0.0;
    for (size_t i = 0; i < ifft_size_; i++) {
        // The windowed response is real and symmetric, so its transform is
        // real; the imaginary part is numerical dust.
        mask_[i] = impulse_re[i];
        passed += static_cast<double>(mask_[i]) * mask_[i];
    }
    noise_bandwidth_hz_ = passed * bin_hz_;

    mask_dirty_ = false;
}

ChannelBlock Channelizer::current_block() const {
    ChannelBlock block;
    block.re_ = spectrum_re_.data();
    block.im_ = spectrum_im_.data();
    block.size_ = fft_size_;
    block.rate_ = sample_rate_;
    block.index_ = block_index_;
    return block;
}

ChannelBlock Channelizer::take_block(DspVector<float>& re, DspVector<float>& im) {
    if (re.size() != fft_size_ || im.size() != fft_size_)
        throw std::invalid_argument("Spectrum buffers must match the channelizer FFT size");
    const auto block = current_block();
    spectrum_re_.swap(re);
    spectrum_im_.swap(im);
    return block;
}

void Channel::pull(const Channelizer& parent, cfloat* out) {
    pull(parent.current_block(), out);
}

void Channel::pull(const ChannelBlock& parent, cfloat* out) {
    if (mask_dirty_) rebuild_mask();
    PullScratch& scratch = pull_scratch(ifft_size_);
    float* work_re = scratch.re.data();
    float* work_im = scratch.im.data();

    const float* spectrum_re = parent.spectrum_re();
    const float* spectrum_im = parent.spectrum_im();
    const size_t spectrum_mask = fft_size_ - 1;
    const size_t slice_mask = ifft_size_ - 1;
    const long half = static_cast<long>(ifft_size_ / 2);

    // Bins -half .. -1 land in the top half of the slice and 0 .. half-1 in
    // the bottom half. When the source bins do not wrap round the end of the
    // spectrum, as they do only for a channel at its very edge, each half is
    // one straight run the compiler can vectorise; a bin the mask shuts out
    // still reads as +0, as below.
    const size_t first = static_cast<size_t>(center_bin_ - half) & spectrum_mask;
    if (first + ifft_size_ <= fft_size_) {
        const auto run = [](const float* re, const float* im, const float* gain, float* out_re, float* out_im,
                            size_t count) {
            size_t k = 0;
#if defined(__GNUC__) && (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
            // Left to itself the compiler keeps this scalar.
            typedef float Floats __attribute__((vector_size(16)));
            typedef int32_t Ints __attribute__((vector_size(16)));
            const auto load = [](const float* p) {
                Floats v;
                std::memcpy(&v, p, sizeof v);
                return v;
            };
            for (; k + 4 <= count; k += 4) {
                const Floats g = load(gain + k);
                const Ints open = g != Floats{};
                const Floats product_re = (Floats)((Ints)(load(re + k) * g) & open);
                const Floats product_im = (Floats)((Ints)(load(im + k) * g) & open);
                std::memcpy(out_re + k, &product_re, sizeof product_re);
                std::memcpy(out_im + k, &product_im, sizeof product_im);
            }
#endif
            for (; k < count; k++) {
                out_re[k] = gain[k] != 0.0f ? re[k] * gain[k] : 0.0f;
                out_im[k] = gain[k] != 0.0f ? im[k] * gain[k] : 0.0f;
            }
        };
        const size_t count = ifft_size_ / 2;
        run(spectrum_re + first, spectrum_im + first, mask_.data() + count, work_re + count,
            work_im + count, count);
        run(spectrum_re + first + count, spectrum_im + first + count, mask_.data(), work_re,
            work_im, count);
    } else {
        for (long j = -half; j < half; j++) {
            const size_t slot = static_cast<size_t>(j) & slice_mask;
            const float gain = mask_[slot];
            if (gain == 0.0f) {
                work_re[slot] = 0.0f;
                work_im[slot] = 0.0f;
                continue;
            }
            const size_t src = static_cast<size_t>(center_bin_ + j) & spectrum_mask;
            work_re[slot] = spectrum_re[src] * gain;
            work_im[slot] = spectrum_im[src] * gain;
        }
    }

    ifft_.inverse_unscaled(work_re, work_im);

    // Selecting bins around center_bin_ is a down-conversion whose phase
    // reference is the start of the FFT window - and that window restarts
    // every block, while a true down-conversion's reference runs from absolute
    // time zero. Blocks advance by K/2 samples, so the phase owed at block b is
    // -pi * center_bin * b. Modulo 2*pi that is 0 or pi, so the correction
    // collapses to a sign flip on odd blocks when the centre bin is odd.
    //
    // Left out, this tunes the channel exactly one bin off and smears it, for
    // half of all tuning positions. It is applied per frame, before the
    // overlap-add, or two frames with opposite signs would cancel.
    const bool invert = (center_bin_ & 1L) != 0 && (parent.block_index() & 1ull) != 0;
    const float scale = (invert ? -1.0f : 1.0f) / static_cast<float>(fft_size_);

    const size_t count = ifft_size_ / 2;
    const float* window = synthesis_window_;

    // Rebuild the oscillator from the accumulated phase each block. This
    // bounds recurrence drift without normalising every sample or letting
    // rounding error change the tuning over a long-running connection.
    //
    // Within the block it runs as four interleaved recurrences, one for each
    // sample position modulo four, stepped by four samples' rotation. One
    // recurrence made every sample wait for the complex multiply before it,
    // which left this loop latency-bound at about eight cycles a sample and
    // made it the most expensive part of a listener's channel. count is a
    // power of two of at least four.
    double oscillator_re[4], oscillator_im[4];
    oscillator_re[0] = std::cos(phase_);
    oscillator_im[0] = std::sin(phase_);
    for (int k = 1; k < 4; k++) {
        oscillator_re[k] = oscillator_re[k - 1] * step_re_ - oscillator_im[k - 1] * step_im_;
        oscillator_im[k] = oscillator_re[k - 1] * step_im_ + oscillator_im[k - 1] * step_re_;
    }
    for (size_t i = 0; i < count; i += 4) {
        for (size_t k = 0; k < 4; k++) {
            // Overlap-add: this frame's first half plus the previous frame's
            // second half, both through the synthesis window.
            const float re = work_re[i + k] * window[i + k] * scale + tail_re_[i + k];
            const float im = work_im[i + k] * window[i + k] * scale + tail_im_[i + k];

            const float c = static_cast<float>(oscillator_re[k]);
            const float sn = static_cast<float>(oscillator_im[k]);
            out[i + k] = cfloat(re * c - im * sn, re * sn + im * c);
            const double next_re = oscillator_re[k] * step4_re_ - oscillator_im[k] * step4_im_;
            oscillator_im[k] = oscillator_re[k] * step4_im_ + oscillator_im[k] * step4_re_;
            oscillator_re[k] = next_re;
        }
    }
    phase_ += step_ * static_cast<double>(count);
    if (phase_ < -kTwoPi) phase_ += kTwoPi;
    if (phase_ > kTwoPi) phase_ -= kTwoPi;

    for (size_t i = 0; i < count; i++) {
        tail_re_[i] = work_re[count + i] * window[count + i] * scale;
        tail_im_[i] = work_im[count + i] * window[count + i] * scale;
    }
}

}  // namespace fernsdr
