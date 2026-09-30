#include "zoom_spectrum.h"

#include <algorithm>
#include <cmath>

#include "simd.h"

namespace fernsdr {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Pixels a band bin may be stretched over before a view gets its own
// spectrum; see zoom_wanted().
constexpr double kStretchAllowed = 1.25;

// A view whose pixels are this many of the parent's bins or more reads the
// bins themselves: the sin^3 window's main lobe is about 1.7 bins wide, so a
// pixel of 1.5 still shows what it would with finer bins.
constexpr double kBinsPerPixel = 1.5;

// A channel passes nine tenths of its width; any zoom's centre moves on a
// grid of a sixteenth of it.
constexpr double kPassed = 0.9;
constexpr double kGrid = 1.0 / 16.0;

// Parent bins lost at each end of a range read directly: the kernel reads
// one neighbour, and a bin at the edge sees only half of its main lobe.
constexpr long kBinsEdge = 2;

// What a channel's filter takes at each edge of its passband: the
// transition Channel::rebuild_mask() draws, and four of the parent's bins
// more for what making the mask realisable widens it by.
double filter_edge(double parent_bin_hz) {
    return std::max(3.0 * parent_bin_hz, 300.0) + 4.0 * parent_bin_hz;
}

// The kernel over a run of bins with both neighbours inside the array:
// 0.5 X[k] - 0.25 (X[k-1] + X[k+1]) is the transform of the block
// windowed by sin^3 instead of sin (see the header), and its power is added
// to `acc`. The neighbours' phase factor, exp(+-i pi / K), is left out: a
// range is read directly only on a transform of 65,536 points or more,
// where it is under 5e-5 rad.
void kernel_power(const float* re, const float* im, size_t count, float* acc) {
    for (size_t j = 0; j < count; j++) {
        const float yr = 0.5f * re[j] - 0.25f * (re[j - 1] + re[j + 1]);
        const float yi = 0.5f * im[j] - 0.25f * (im[j - 1] + im[j + 1]);
        acc[j] += yr * yr + yi * yi;
    }
}

}  // namespace

bool zoom_wanted(double low_hz, double high_hz, int width, double band_bin_hz) {
    if (width <= 0 || !(high_hz > low_hz) || !(band_bin_hz > 0.0)) return false;
    return band_bin_hz > kStretchAllowed * (high_hz - low_hz) / width;
}

bool ZoomSpectrum::plan(const Channelizer& parent, double origin_hz, double rf_scale, double low_hz, double high_hz,
                        int width, ZoomKey& key) {
    if (width <= 0 || !(high_hz > low_hz) || !(rf_scale > 0.0)) return false;
    const double bin = parent.bin_hz();
    const double span = (high_hz - low_hz) / rf_scale;
    const double centre = (0.5 * (low_hz + high_hz) - origin_hz) / rf_scale;
    const double pixel = span / width;

    if (pixel >= kBinsPerPixel * bin) {
        // The view, less a grid step, fits between the unusable edge bins:
        // count / 2 - edge >= span / (2 bin) + count * kGrid / 2.
        const double needed = (span / bin + 2.0 * kBinsEdge) / (1.0 - kGrid);
        size_t count = 16;
        while (count < parent.fft_size() && static_cast<double>(count) < needed) count *= 2;
        if (static_cast<double>(count) < needed) return false;
        const double step = static_cast<double>(count) * kGrid * bin;
        key.centre_hz = std::round(centre / step) * step;
        key.channel_size = count;
        key.transform_size = 0;
        return true;
    }

    // The view must fit in the flat part, 0.45 of the width less an edge on
    // each side, with its centre up to half a grid step off the channel's:
    // span / 2 + width * kGrid / 2 <= width * kPassed / 2 - edge / 2.
    const double needed = (span + filter_edge(bin)) / (kPassed - kGrid);
    size_t size = 8;
    while (size < parent.fft_size() && static_cast<double>(size) * bin < needed) size *= 2;
    if (static_cast<double>(size) * bin < needed) return false;
    const double rate = static_cast<double>(size) * bin;
    const double step = rate * kGrid;
    // Bins no wider than a pixel, nor finer than kFinestBinHz.
    const double target = std::max(pixel, kFinestBinHz);
    size_t transform = 64;
    while (transform < (1u << 16) && rate / static_cast<double>(transform) > target) transform *= 2;
    key.centre_hz = std::round(centre / step) * step;
    key.channel_size = size;
    key.transform_size = transform;
    return true;
}

ZoomSpectrum::ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                           double lines_per_second, int averages, float smoothing)
    : key_(key), lines_per_second_(lines_per_second), smoothing_(std::clamp(smoothing, 0.0f, 0.95f)) {
    real_parent_ = parent.kind() == SignalKind::Real;
    parent_size_ = parent.fft_size();
    const double centre_rf = origin_hz + rf_scale * key.centre_hz;
    const size_t n = reads_bins() ? key.channel_size : key.transform_size;
    // Where the line's bin 0 is centred, from the zoom's centre; each cell
    // starts half a bin before its centre, as the band's do.
    double first_hz = 0.0;
    double usable = 0.0;
    if (reads_bins()) {
        bin_hz_ = parent.bin_hz();
        first_bin_ = std::lround(key.centre_hz / bin_hz_) - static_cast<long>(key.channel_size / 2);
        first_hz = static_cast<double>(first_bin_) * bin_hz_ - key.centre_hz;
        usable = (0.5 * static_cast<double>(key.channel_size) - kBinsEdge) * bin_hz_;
        power_.assign(key.channel_size, 0.0f);
        smoothed_.assign(key.channel_size, -160.0f);
        // A full-scale complex tone sums to the effective window's sum, that
        // of sin^3; a real band's bins are the analytic signal's, which
        // carries a real tone at twice what the band's own line reads.
        double sum = 0.0;
        for (size_t i = 0; i < parent_size_; i++) sum += std::pow(std::sin(kPi * (i + 0.5) / parent_size_), 3.0);
        gain_db_ = static_cast<float>(20.0 * std::log10(sum) + (real_parent_ ? 20.0 * std::log10(2.0) : 0.0));
    } else {
        channel_ = std::make_unique<Channel>(parent, key.channel_size);
        const double rate = channel_->output_rate();
        analyzer_ = std::make_unique<SpectrumAnalyzer>(rate, key.transform_size, lines_per_second, averages,
                                                       smoothing, SignalKind::Iq);
        channel_->set_passband(key.centre_hz, -0.5 * kPassed * rate, 0.5 * kPassed * rate);
        bin_hz_ = analyzer_->bin_hz();
        first_hz = -0.5 * rate;
        usable = 0.5 * kPassed * rate - 0.5 * filter_edge(parent.bin_hz());
        baseband_.resize(channel_->output_per_block());
    }
    low_hz_ = centre_rf + rf_scale * (first_hz - 0.5 * bin_hz_);
    high_hz_ = low_hz_ + rf_scale * bin_hz_ * static_cast<double>(n);
    flat_low_hz_ = centre_rf - rf_scale * usable;
    flat_high_hz_ = centre_rf + rf_scale * usable;

    // Past the edges of what the parent sampled a zoom reads the other edge
    // of an IQ band (the channelizer's spectrum is circular) or nothing at
    // all of a real one; either way nothing is there to draw.
    const double sampled_low = real_parent_ ? 0.0 : -0.5 * parent.sample_rate();
    const double sampled_high = 0.5 * parent.sample_rate();
    sampled_first_ = n;
    sampled_end_ = 0;
    for (size_t i = 0; i < n; i++) {
        const double hz = key.centre_hz + first_hz + static_cast<double>(i) * bin_hz_;
        if (hz < sampled_low || hz > sampled_high) continue;
        sampled_first_ = std::min(sampled_first_, i);
        sampled_end_ = i + 1;
    }
}

void ZoomSpectrum::accumulate_bins(const ChannelBlock& block) {
    const float* re = block.spectrum_re();
    const float* im = block.spectrum_im();
    const long size = static_cast<long>(parent_size_);
    const size_t count = power_.size();
    for (size_t i = 0; i < count;) {
        const long k = ((first_bin_ + static_cast<long>(i)) % size + size) % size;
        if (k == 0 || k == size - 1) {
            // The ends of the circular spectrum, one bin at a time.
            const long before = (k + size - 1) % size, after = (k + 1) % size;
            const float yr = 0.5f * re[k] - 0.25f * (re[before] + re[after]);
            const float yi = 0.5f * im[k] - 0.25f * (im[before] + im[after]);
            power_[i] += yr * yr + yi * yi;
            i++;
            continue;
        }
        const size_t run = std::min(count - i, static_cast<size_t>(size - 1 - k));
        kernel_power(re + k, im + k, run, power_.data() + i);
        i += run;
    }
    blocks_++;
}

void ZoomSpectrum::finish_line() {
    const size_t count = power_.size();
    spectrum_line_.resize(count);
    simd::power_to_db(power_.data(), count, 1.0f / static_cast<float>(blocks_), gain_db_, spectrum_line_.data());
    for (size_t i = 0; i < count; i++) {
        // In decibels, as the band's line is smoothed.
        const float db = spectrum_line_[i];
        smoothed_[i] = smoothed_primed_ ? smoothed_[i] + (1.0f - smoothing_) * (db - smoothed_[i]) : db;
        spectrum_line_[i] = smoothed_[i];
    }
    smoothed_primed_ = true;
    std::fill(power_.begin(), power_.end(), 0.0f);
    blocks_ = 0;
}

void ZoomSpectrum::process(const ChannelBlock& block) {
    has_line_ = false;
    if (reads_bins()) {
        accumulate_bins(block);
        credit_ += block.block_seconds() * lines_per_second_;
        if (credit_ < 1.0) return;
        credit_ = std::min(credit_ - 1.0, 1.0);
        finish_line();
    } else {
        channel_->pull(block, baseband_.data());
        if (real_parent_) {
            // A real band's channel is the analytic signal, which carries a
            // real carrier of amplitude A at A; the band's line reads it at A / 2.
            for (cfloat& v : baseband_) v *= 0.5f;
        }
        analyzer_->push(baseband_.data(), baseband_.size());
        if (!analyzer_->take_line(spectrum_line_)) return;
    }
    for (size_t i = 0; i < spectrum_line_.size(); i++) {
        if (i < sampled_first_ || i >= sampled_end_) spectrum_line_[i] = -160.0f;
    }
    line_.build(spectrum_line_.data(), spectrum_line_.size(), low_hz_, high_hz_);
    if (previous_line_.size() == spectrum_line_.size()) {
        paired_line_.resize(spectrum_line_.size());
        for (size_t i = 0; i < spectrum_line_.size(); i++) {
            paired_line_[i] = 0.5f * (previous_line_[i] + spectrum_line_[i]);
        }
        paired_.build(paired_line_.data(), paired_line_.size(), low_hz_, high_hz_);
    } else {
        paired_.build(spectrum_line_.data(), spectrum_line_.size(), low_hz_, high_hz_);
    }
    previous_line_.swap(spectrum_line_);
    has_line_ = true;
    ready_ = true;
}

}  // namespace fernsdr
