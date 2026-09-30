#include "zoom_spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "simd.h"

namespace fernsdr {

namespace {

// Pixels a band bin may be stretched over before a view gets its own
// spectrum; see zoom_wanted().
constexpr double kStretchAllowed = 1.25;

// A view whose pixels are this many of the parent's bins or more reads the
// bins themselves: the sin^3 window's main lobe is about 1.7 bins wide, so a
// pixel of 1.5 still shows what it would with finer bins.
constexpr double kBinsPerPixel = 1.5;

// A channel passes nine tenths of its width, and its centre moves on a grid
// of a sixteenth of it.
constexpr double kPassed = 0.9;
constexpr double kGrid = 1.0 / 16.0;

// Parent bins read beyond each end of a view: its edge pixels then have
// their neighbours' main lobes to draw from, as pixels inside it do.
constexpr long kBinsEdge = 2;

// What a channel's filter takes at each edge of its passband: the
// transition Channel::rebuild_mask() draws, and four of the parent's bins
// more for what making the mask realisable widens it by.
double filter_edge(double parent_bin_hz) {
    return std::max(3.0 * parent_bin_hz, 300.0) + 4.0 * parent_bin_hz;
}

long floor_div(long a, long b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// The kernel over a run of bins with both neighbours inside the array:
// 0.5 X[k] - 0.25 (X[k-1] + X[k+1]) is the transform of the block
// windowed by sin^3 instead of sin (see the header), and its power is added
// to `acc`. The neighbours' phase factor, exp(+-i pi / K), is left out: a
// view reads bins only where the band's own line is capped below the
// channelizer's resolution, on transforms of 131,072 points or more, where
// it is under 3e-5 rad.
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

BinTile::BinTile(size_t parent_size, long first_bin, float gain_db, float smoothing)
    : parent_size_(parent_size),
      first_bin_(first_bin),
      gain_db_(gain_db),
      smoothing_(std::clamp(smoothing, 0.0f, 0.95f)),
      power_(kBins, 0.0f),
      line_(kBins, -160.0f) {}

void BinTile::accumulate(const ChannelBlock& block) {
    const float* re = block.spectrum_re();
    const float* im = block.spectrum_im();
    const long size = static_cast<long>(parent_size_);
    for (size_t i = 0; i < kBins;) {
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
        const size_t run = std::min(kBins - i, static_cast<size_t>(size - 1 - k));
        kernel_power(re + k, im + k, run, power_.data() + i);
        i += run;
    }
    blocks_++;
}

void BinTile::finish_line() {
    if (blocks_ == 0) return;
    std::vector<float>& db = power_;
    // In place: the powers are not needed once they are decibels.
    simd::power_to_db(power_.data(), kBins, 1.0f / static_cast<float>(blocks_), gain_db_, db.data());
    for (size_t i = 0; i < kBins; i++) {
        // In decibels, as the band's line is smoothed.
        line_[i] = has_line_ ? line_[i] + (1.0f - smoothing_) * (db[i] - line_[i]) : db[i];
    }
    std::fill(power_.begin(), power_.end(), 0.0f);
    blocks_ = 0;
    has_line_ = true;
}

bool ZoomSpectrum::plan(const Channelizer& parent, double origin_hz, double rf_scale, double low_hz, double high_hz,
                        int width, ZoomKey& key) {
    if (width <= 0 || !(high_hz > low_hz) || !(rf_scale > 0.0)) return false;
    const double bin = parent.bin_hz();
    const double low = (low_hz - origin_hz) / rf_scale;
    const double high = (high_hz - origin_hz) / rf_scale;
    const double span = high - low;
    const double pixel = span / width;
    key = ZoomKey{};

    if (pixel >= kBinsPerPixel * bin) {
        const long first = static_cast<long>(std::floor(low / bin)) - kBinsEdge;
        const long end = static_cast<long>(std::ceil(high / bin)) + kBinsEdge + 1;
        if (end - first > static_cast<long>(parent.fft_size())) return false;
        key.first_bin = first;
        key.bins = static_cast<size_t>(end - first);
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
    key.centre_hz = std::round(0.5 * (low + high) / step) * step;
    key.channel_size = size;
    key.transform_size = transform;
    return true;
}

ZoomSpectrum::ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                           double lines_per_second, int averages, float smoothing)
    : key_(key) {
    channel_ = std::make_unique<Channel>(parent, key.channel_size);
    const double rate = channel_->output_rate();
    analyzer_ = std::make_unique<SpectrumAnalyzer>(rate, key.transform_size, lines_per_second, averages, smoothing,
                                                   SignalKind::Iq);
    channel_->set_passband(key.centre_hz, -0.5 * kPassed * rate, 0.5 * kPassed * rate);
    bin_hz_ = analyzer_->bin_hz();
    const double usable = 0.5 * kPassed * rate - 0.5 * filter_edge(parent.bin_hz());
    place(parent, origin_hz, rf_scale, key.centre_hz - 0.5 * rate, key.transform_size, key.centre_hz - usable,
          key.centre_hz + usable);
    baseband_.resize(channel_->output_per_block());
}

ZoomSpectrum::ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                           std::vector<std::shared_ptr<BinTile>> tiles)
    : key_(key), tiles_(std::move(tiles)) {
    bin_hz_ = parent.bin_hz();
    const double first = static_cast<double>(key.first_bin);
    const double last = first + static_cast<double>(key.bins) - 1.0;
    place(parent, origin_hz, rf_scale, first * bin_hz_, key.bins, (first + kBinsEdge) * bin_hz_,
          (last - kBinsEdge) * bin_hz_);
    spectrum_line_.resize(key.bins);
}

void ZoomSpectrum::place(const Channelizer& parent, double origin_hz, double rf_scale, double first_hz, size_t bins,
                         double usable_low_hz, double usable_high_hz) {
    real_parent_ = parent.kind() == SignalKind::Real;
    // Each cell starts half a bin before its centre, as the band's do.
    low_hz_ = origin_hz + rf_scale * (first_hz - 0.5 * bin_hz_);
    high_hz_ = low_hz_ + rf_scale * bin_hz_ * static_cast<double>(bins);
    flat_low_hz_ = origin_hz + rf_scale * usable_low_hz;
    flat_high_hz_ = origin_hz + rf_scale * usable_high_hz;
    // Past the edges of what the parent sampled a zoom reads the other edge
    // of an IQ band (the channelizer's spectrum is circular) or nothing at
    // all of a real one; either way nothing is there to draw.
    const double sampled_low = real_parent_ ? 0.0 : -0.5 * parent.sample_rate();
    const double sampled_high = 0.5 * parent.sample_rate();
    sampled_first_ = bins;
    sampled_end_ = 0;
    for (size_t i = 0; i < bins; i++) {
        const double hz = first_hz + static_cast<double>(i) * bin_hz_;
        if (hz < sampled_low || hz > sampled_high) continue;
        sampled_first_ = std::min(sampled_first_, i);
        sampled_end_ = i + 1;
    }
}

void ZoomSpectrum::process(const ChannelBlock& block) {
    has_line_ = false;
    channel_->pull(block, baseband_.data());
    if (real_parent_) {
        // A real band's channel is the analytic signal, which carries a real
        // carrier of amplitude A at A; the band's line reads it at A / 2.
        for (cfloat& v : baseband_) v *= 0.5f;
    }
    analyzer_->push(baseband_.data(), baseband_.size());
    if (analyzer_->take_line(spectrum_line_)) publish();
}

void ZoomSpectrum::assemble(bool line) {
    has_line_ = false;
    if (!line) return;
    for (const auto& tile : tiles_) {
        if (!tile->has_line()) return;
    }
    const long tile_bins = static_cast<long>(BinTile::kBins);
    const long end = key_.first_bin + static_cast<long>(key_.bins);
    for (long bin = key_.first_bin; bin < end;) {
        const size_t index = static_cast<size_t>(floor_div(bin, tile_bins) - floor_div(key_.first_bin, tile_bins));
        const BinTile& tile = *tiles_[index];
        const long offset = bin - tile.first_bin();
        const long run = std::min(tile_bins - offset, end - bin);
        std::memcpy(spectrum_line_.data() + (bin - key_.first_bin), tile.line() + offset,
                    static_cast<size_t>(run) * sizeof(float));
        bin += run;
    }
    publish();
}

void ZoomSpectrum::publish() {
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
    // A view of tiles copies into spectrum_line_ every line, so it keeps its
    // size; a channel's analyser hands over a fresh one.
    previous_line_ = spectrum_line_;
    has_line_ = true;
    ready_ = true;
}

}  // namespace fernsdr
