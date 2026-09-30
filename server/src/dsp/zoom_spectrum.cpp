#include "zoom_spectrum.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

namespace {

// Pixels a band bin may be stretched over before a view gets its own
// transform; see zoom_wanted().
constexpr double kStretchAllowed = 1.25;

// The channel passes nine tenths of its width; its centre moves on a grid of
// a sixteenth of it.
constexpr double kPassed = 0.9;
constexpr double kGrid = 1.0 / 16.0;

// What the channel's filter takes at each edge of its passband: the
// transition Channel::rebuild_mask() draws, and four of the parent's bins
// more for what making the mask realisable widens it by.
double filter_edge(double parent_bin_hz) {
    return std::max(3.0 * parent_bin_hz, 300.0) + 4.0 * parent_bin_hz;
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
    const double target = std::max(span / width, kFinestBinHz);
    size_t transform = 64;
    while (transform < (1u << 16) && rate / static_cast<double>(transform) > target) transform *= 2;
    key.centre_hz = std::round(centre / step) * step;
    key.channel_size = size;
    key.transform_size = transform;
    return true;
}

ZoomSpectrum::ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                           double lines_per_second, int averages, float smoothing)
    : key_(key),
      channel_(parent, key.channel_size),
      analyzer_(channel_.output_rate(), key.transform_size, lines_per_second, averages, smoothing, SignalKind::Iq) {
    const double rate = channel_.output_rate();
    channel_.set_passband(key.centre_hz, -0.5 * kPassed * rate, 0.5 * kPassed * rate);
    const double centre_rf = origin_hz + rf_scale * key.centre_hz;
    // The analyser's bin 0 is centred at -rate/2 from the channel's centre;
    // its cells start half a bin before that, as the band's do.
    const double bin = rate / static_cast<double>(key.transform_size);
    low_hz_ = centre_rf + rf_scale * (-0.5 * rate - 0.5 * bin);
    high_hz_ = low_hz_ + rf_scale * rate;
    const double flat = 0.5 * kPassed * rate - 0.5 * filter_edge(parent.bin_hz());
    flat_low_hz_ = centre_rf - rf_scale * flat;
    flat_high_hz_ = centre_rf + rf_scale * flat;

    // Past the edges of what the parent sampled the channel reads the other
    // edge of an IQ band (the channelizer's spectrum is circular) or nothing
    // at all of a real one; either way nothing is there to draw.
    real_parent_ = parent.kind() == SignalKind::Real;
    const double sampled_low = real_parent_ ? 0.0 : -0.5 * parent.sample_rate();
    const double sampled_high = 0.5 * parent.sample_rate();
    const size_t n = key.transform_size;
    sampled_first_ = n;
    sampled_end_ = 0;
    for (size_t i = 0; i < n; i++) {
        const double hz = key.centre_hz + (static_cast<double>(i) - 0.5 * static_cast<double>(n)) * bin;
        if (hz < sampled_low || hz > sampled_high) continue;
        sampled_first_ = std::min(sampled_first_, i);
        sampled_end_ = i + 1;
    }
    baseband_.resize(channel_.output_per_block());
}

void ZoomSpectrum::process(const ChannelBlock& block) {
    has_line_ = false;
    channel_.pull(block, baseband_.data());
    if (real_parent_) {
        // A real band's channel is the analytic signal, which carries a real
        // carrier of amplitude A at A; the band's line reads it at A / 2.
        for (cfloat& v : baseband_) v *= 0.5f;
    }
    analyzer_.push(baseband_.data(), baseband_.size());
    if (!analyzer_.take_line(spectrum_line_)) return;
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
