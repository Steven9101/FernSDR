#include "spectrum.h"

#include "simd.h"

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

DspVector<float> make_blackman_harris_window(size_t length) {
    // 4-term Blackman-Harris: -92 dB sidelobes, which keeps a strong
    // broadcast carrier from painting a skirt across the whole band.
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    DspVector<float> w(length);
    for (size_t n = 0; n < length; n++) {
        const double t = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(length - 1);
        w[n] = static_cast<float>(a0 - a1 * std::cos(t) + a2 * std::cos(2 * t) - a3 * std::cos(3 * t));
    }
    return w;
}

SpectrumAnalyzer::SpectrumAnalyzer(double sample_rate, size_t fft_size, double lines_per_second,
                                   int averages, float smoothing, SignalKind kind)
    : sample_rate_(sample_rate),
      fft_size_(fft_size),
      averages_(std::max(1, averages)),
      kind_(kind),
      fft_(kind == SignalKind::Iq ? fft_size : 1),
      real_fft_(kind == SignalKind::Real ? fft_size : 4),
      window_(make_blackman_harris_window(fft_size)) {
    if (lines_per_second <= 0.0) throw std::invalid_argument("lines_per_second must be positive");

    const double samples_per_line = sample_rate / lines_per_second;
    // Past 75 % overlap a Blackman-Harris transform sees almost the same
    // samples as its neighbour, weighted almost the same way: at 84 % (a
    // 2 Msps band, 25 lines of 8 averages) the line's noise was 1 % lower
    // than at 75 % for 60 % more transforms. So averages that would overlap
    // more than that are not taken. A wide band, whose hop is longer than a
    // transform, keeps all it asked for.
    const double quarter = static_cast<double>(fft_size) / 4.0;
    averages_ = std::max(1, std::min(averages_, static_cast<int>(samples_per_line / quarter)));
    hop_ = static_cast<size_t>(std::max(1.0, samples_per_line / averages_));

    // Normalise so a full-scale complex sinusoid on a bin reads 0 dBFS.
    double coherent_gain = 0.0;
    for (float v : window_) coherent_gain += v;
    window_gain_db_ = static_cast<float>(20.0 * std::log10(coherent_gain));

    if (kind_ == SignalKind::Iq) ring_.assign(4 * fft_size_, 0.0f);
    else history_re_.assign(fft_size_, 0.0f);
    work_re_.assign(fft_size_, 0.0f);
    work_im_.assign(fft_size_, 0.0f);
    accumulator_.assign(fft_size_, 0.0f);
    ready_.assign(bins(), -160.0f);
    smoothed_.assign(bins(), -160.0f);
    set_smoothing(smoothing);
}

void SpectrumAnalyzer::set_smoothing(float smoothing) {
    smoothing_ = smoothing < 0.0f ? 0.0f : (smoothing > 0.95f ? 0.95f : smoothing);
}

// When a line's hop is longer than the transform, the samples at the start
// of each hop never reach a window: only the last fft_size_ before a hop are
// transformed. They are counted and not copied. At 64 Msps, twelve lines a
// second and a million-point transform, that is four samples in five.
size_t SpectrumAnalyzer::unused_ahead(size_t count) const {
    if (hop_ <= fft_size_ || since_hop_ >= hop_ - fft_size_) return 0;
    return std::min(count, hop_ - fft_size_ - since_hop_);
}

void SpectrumAnalyzer::push(const cfloat* input, size_t count) {
    if (kind_ != SignalKind::Iq) return;
    while (count) {
        if (const size_t skip = unused_ahead(count)) {
            input += skip;
            count -= skip;
            finish_span(skip);
            continue;
        }
        // Stop at both boundaries. Crossing a hop before transforming would
        // replace samples needed by that FFT; crossing the ring end would
        // overrun storage.
        const size_t span = std::min({count, fft_size_ - write_pos_, hop_ - since_hop_});
        std::memcpy(ring_.data() + 2 * write_pos_, input, span * sizeof(cfloat));
        std::memcpy(ring_.data() + 2 * (write_pos_ + fft_size_), input, span * sizeof(cfloat));
        input += span;
        count -= span;
        finish_span(span);
    }
}

void SpectrumAnalyzer::push_real(const float* input, size_t count) {
    if (kind_ != SignalKind::Real) return;
    while (count) {
        if (const size_t skip = unused_ahead(count)) {
            input += skip;
            count -= skip;
            finish_span(skip);
            continue;
        }
        const size_t span = std::min({count, fft_size_ - write_pos_, hop_ - since_hop_});
        std::memcpy(history_re_.data() + write_pos_, input, span * sizeof(float));
        input += span;
        count -= span;
        finish_span(span);
    }
}

void SpectrumAnalyzer::finish_span(size_t count) {
    // Both FFT plans require powers of two; the ring has that same length.
    write_pos_ = (write_pos_ + count) & (fft_size_ - 1);
    primed_ = std::min(fft_size_, primed_ + count);
    since_hop_ += count;
    if (since_hop_ == hop_) {
        since_hop_ = 0;
        const uint64_t cycle = static_cast<uint64_t>(averages_) * static_cast<uint64_t>(stride_);
        // Until the first line exists every hop is wanted, so a band that has
        // just started has something to show at once.
        const bool wanted = stride_ == 1 || accumulated_ > 0 || transforms_ < static_cast<uint64_t>(averages_) ||
                            hops_ % cycle < static_cast<uint64_t>(averages_);
        hops_++;
        hops_since_line_++;
        if (primed_ >= fft_size_ && wanted) transform();
    }
}

void SpectrumAnalyzer::transform() {
    transforms_++;
    if (kind_ == SignalKind::Real) {
        // Unwrap the ring into the work buffer, oldest first, applying the
        // window: the oldest samples run from write_pos_ to the end of the
        // ring, the rest from its start. Two straight runs rather than one
        // masked index, so the loops vectorise.
        const size_t older = fft_size_ - write_pos_;
        for (size_t i = 0; i < older; i++) work_re_[i] = history_re_[write_pos_ + i] * window_[i];
        for (size_t i = 0; i < write_pos_; i++) work_re_[older + i] = history_re_[i] * window_[older + i];
        // A real input occupies 0 .. rate/2, so only that half is transformed
        // and only that half is published.
        real_fft_.forward(work_re_.data(), work_re_.data(), work_im_.data());
        for (size_t i = 0; i < fft_size_ / 2; i++) {
            const float re = work_re_[i];
            const float im = work_im_[i];
            accumulator_[i] += re * re + im * im;
        }
    } else {
        // The window's oldest sample is at write_pos_; its second half starts
        // half a transform on, both in the doubled ring's one run.
        const float* older = ring_.data() + 2 * write_pos_;
        fft_.forward_windowed(older, older + fft_size_, window_.data(), work_re_.data(), work_im_.data());
        for (size_t i = 0; i < fft_size_; i++) {
            const float re = work_re_[i];
            const float im = work_im_[i];
            accumulator_[i] += re * re + im * im;
        }
    }

    if (++accumulated_ < averages_) return;

    const size_t count = bins();
    const size_t half = fft_size_ / 2;
    const float scale = 1.0f / static_cast<float>(accumulated_);
    // Every bin to decibels in one pass over vectors: at 25 lines a second
    // of 65,536 bins, a logarithm apiece was a visible share of a band's cost.
    db_line_.resize(count);
    simd::power_to_db(accumulator_.data(), count, scale, window_gain_db_, db_line_.data());
    for (size_t i = 0; i < count; i++) {
        // IQ lines are shifted so index 0 is the lowest frequency; a real
        // line already starts at DC.
        const size_t src = kind_ == SignalKind::Iq ? (i + half) & (fft_size_ - 1) : i;
        const float db = db_line_[src];
        // Smoothing runs in dB rather than power so a brief strong signal does
        // not dominate the average for several lines afterwards.
        smoothed_[i] = smoothed_primed_ ? smoothed_[i] + (1.0f - smoothing_) * (db - smoothed_[i]) : db;
        ready_[i] = smoothed_[i];
    }
    smoothed_primed_ = true;

    std::fill(accumulator_.begin(), accumulator_.end(), 0.0f);
    accumulated_ = 0;
    has_line_ = true;
    last_line_hops_ = hops_since_line_;
    hops_since_line_ = 0;
}

bool SpectrumAnalyzer::take_line(std::vector<float>& out) {
    if (!has_line_) return false;
    out = ready_;
    has_line_ = false;
    return true;
}

void render_viewport(const float* src, size_t src_bins, double src_low_hz, double src_high_hz,
                     double view_low_hz, double view_high_hz, float* dst, size_t width) {
    if (width == 0 || src_bins == 0) return;

    const double src_span = src_high_hz - src_low_hz;
    if (src_span <= 0.0) {
        std::fill(dst, dst + width, -160.0f);
        return;
    }
    const double bins_per_hz = static_cast<double>(src_bins) / src_span;
    const double view_span = view_high_hz - view_low_hz;

    // Source range covered by each output pixel. A pixel's right edge is
    // computed exactly as the next one's left edge, so it is carried over
    // rather than divided out again.
    double f1 = view_low_hz + view_span * 0.0 / static_cast<double>(width);
    for (size_t i = 0; i < width; i++) {
        const double f0 = f1;
        f1 = view_low_hz + view_span * static_cast<double>(i + 1) / static_cast<double>(width);

        double b0 = (f0 - src_low_hz) * bins_per_hz;
        double b1 = (f1 - src_low_hz) * bins_per_hz;

        if (b1 <= 0.0 || b0 >= static_cast<double>(src_bins)) {
            dst[i] = -160.0f;
            continue;
        }

        if (b1 - b0 >= 1.0) {
            // Zoomed out: peak-hold across the covered bins.
            const size_t lo = static_cast<size_t>(std::max(0.0, std::floor(b0)));
            const size_t hi = std::min(src_bins, static_cast<size_t>(std::ceil(b1)));
            float peak = -160.0f;
            for (size_t b = lo; b < hi; b++) peak = std::max(peak, src[b]);
            dst[i] = peak;
        } else {
            // Zoomed in: interpolate between neighbouring bins.
            const double centre = (b0 + b1) * 0.5 - 0.5;
            const double clamped = std::clamp(centre, 0.0, static_cast<double>(src_bins - 1));
            const size_t lo = static_cast<size_t>(clamped);
            const size_t hi = std::min(src_bins - 1, lo + 1);
            const float t = static_cast<float>(clamped - static_cast<double>(lo));
            dst[i] = src[lo] * (1.0f - t) + src[hi] * t;
        }
    }
}

void SpectrumPyramid::build(const float* line, size_t bins, double low_hz, double high_hz) {
    static std::atomic<uint64_t> versions{0};
    version_ = versions.fetch_add(1, std::memory_order_relaxed) + 1;
    low_hz_ = low_hz;
    high_hz_ = high_hz;
    if (bins == 0) {
        levels_.clear();
        return;
    }

    // Keep the inner buffers too. Shrinking to one level here would discard
    // every coarser level and allocate them again for the next line.
    if (levels_.empty()) levels_.resize(1);
    levels_[0].assign(line, line + bins);

    size_t level = 0;
    // Stop at 8 bins: below that a level saves nothing and the half-bin
    // alignment error starts to matter.
    while (levels_[level].size() > 8) {
        const size_t half = levels_[level].size() / 2;
        // Grow first: resizing the outer vector can reallocate, so no
        // reference into it may be held across this line.
        if (levels_.size() <= level + 1) levels_.resize(level + 2);
        levels_[level + 1].resize(half);
        const float* src = levels_[level].data();
        float* dst = levels_[level + 1].data();
        for (size_t i = 0; i < half; i++) dst[i] = std::max(src[2 * i], src[2 * i + 1]);
        // An odd line leaves its last bin over. It goes into the last cell
        // rather than away: a carrier there would otherwise vanish from every
        // coarser level, at the right edge of whatever view reads them.
        if (levels_[level].size() % 2) dst[half - 1] = std::max(dst[half - 1], src[2 * half]);
        level++;
    }
    levels_.resize(level + 1);
}

namespace {

// Rows a worker rendered lately. Listeners with the same view of the same
// line ask for the same pixels, and a waterfall at the full band's width is
// what every page opens on; a repeat is a copy instead of a render.
struct RenderedRow {
    uint64_t version = 0;
    double low_hz = 0.0;
    double high_hz = 0.0;
    std::vector<float> pixels;
};

struct RenderedRows {
    RenderedRow rows[4];
    unsigned next = 0;
};

}  // namespace

void SpectrumPyramid::render(double view_low_hz, double view_high_hz, float* dst,
                             size_t width) const {
    if (width == 0) return;
    thread_local RenderedRows recent;
    if (version_ != 0) {
        for (const RenderedRow& row : recent.rows) {
            if (row.version == version_ && row.low_hz == view_low_hz && row.high_hz == view_high_hz &&
                row.pixels.size() == width) {
                std::copy(row.pixels.begin(), row.pixels.end(), dst);
                return;
            }
        }
    }
    render_uncached(view_low_hz, view_high_hz, dst, width);
    if (version_ != 0) {
        RenderedRow& row = recent.rows[recent.next++ % 4];
        row.version = version_;
        row.low_hz = view_low_hz;
        row.high_hz = view_high_hz;
        row.pixels.assign(dst, dst + width);
    }
}

void SpectrumPyramid::render_uncached(double view_low_hz, double view_high_hz, float* dst, size_t width) const {
    if (levels_.empty()) {
        std::fill(dst, dst + width, -160.0f);
        return;
    }

    const double span = high_hz_ - low_hz_;
    const double view_span = view_high_hz - view_low_hz;
    size_t chosen = 0;
    if (span > 0.0 && view_span > 0.0) {
        // Bins one level 'up' cover twice the frequency; take the coarsest
        // level whose bins are still no wider than an output pixel, so the
        // peak-hold below never has to skip over a bin it should have seen.
        const double pixel_hz = view_span / static_cast<double>(width);
        double bin_hz = span / static_cast<double>(levels_[0].size());
        while (chosen + 1 < levels_.size() && bin_hz * 2.0 <= pixel_hz) {
            bin_hz *= 2.0;
            chosen++;
        }
    }

    render_viewport(levels_[chosen].data(), levels_[chosen].size(), low_hz_, high_hz_, view_low_hz,
                    view_high_hz, dst, width);
}

bool SpectrumPyramid::native_viewport(double view_low_hz, double view_high_hz, size_t requested_width,
                                      std::vector<float>& out, double& row_low_hz, double& row_high_hz) const {
    if (levels_.empty() || requested_width < 2 || levels_[0].size() < 2 ||
        !std::isfinite(view_low_hz) || !std::isfinite(view_high_hz) ||
        view_low_hz < low_hz_ || view_high_hz > high_hz_ || view_high_hz <= view_low_hz) return false;
    const auto& source = levels_[0];
    const double bin_hz = (high_hz_ - low_hz_) / source.size();
    if (!(bin_hz > 0) || (view_high_hz - view_low_hz) / requested_width >= bin_hz) return false;
    const double first = (view_low_hz - low_hz_) / bin_hz - 0.5;
    const double last = (view_high_hz - low_hz_) / bin_hz - 0.5;
    size_t begin = static_cast<size_t>(std::clamp(std::floor(first), 0.0, static_cast<double>(source.size() - 2)));
    size_t end = static_cast<size_t>(std::clamp(std::ceil(last) + 1, static_cast<double>(begin + 2),
                                               static_cast<double>(source.size())));
    if (end - begin >= requested_width) return false;
    out.assign(source.begin() + begin, source.begin() + end);
    row_low_hz = low_hz_ + begin * bin_hz;
    row_high_hz = low_hz_ + end * bin_hz;
    return true;
}

}  // namespace fernsdr
