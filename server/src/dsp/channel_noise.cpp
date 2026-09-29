#include "channel_noise.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

namespace {

constexpr double kQuantile = 0.10;
constexpr double kCut = 1.5;

// 1 / (1 - z e^-z / (1 - e^-z)) for z = kCut * -ln(1 - kQuantile); see
// noise_bin_power. About 13: the bins below the cut average a thirteenth of
// the noise's mean.
double truncation_correction() {
    const double z = kCut * -std::log(1.0 - kQuantile);
    return 1.0 / (1.0 - z * std::exp(-z) / (1.0 - std::exp(-z)));
}

}  // namespace

double noise_bin_power(const float* re, const float* im, size_t fft_size, long first, size_t count,
                       std::vector<float>& scratch) {
    if (fft_size == 0 || count == 0) return 0.0;
    count = std::min(count, fft_size);
    scratch.resize(count);
    const long size = static_cast<long>(fft_size);
    size_t index = static_cast<size_t>(((first % size) + size) % size);
    for (size_t i = 0; i < count; i++) {
        scratch[i] = re[index] * re[index] + im[index] * im[index];
        if (++index == fft_size) index = 0;
    }
    const size_t q = static_cast<size_t>(static_cast<double>(count) * kQuantile);
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<long>(q), scratch.end());
    const float cut = static_cast<float>(kCut) * scratch[q];
    if (!(cut > 0.0f)) return 0.0;
    double sum = 0.0;
    size_t below = 0;
    for (const float power : scratch) {
        if (power <= cut) {
            sum += power;
            below++;
        }
    }
    static const double correction = truncation_correction();
    return below ? sum / static_cast<double>(below) * correction : 0.0;
}

void ChannelNoise::reset() {
    bin_power_ = 0.0;
    since_look_s_ = 0.0;
}

void ChannelNoise::update(const ChannelBlock& block, double center_hz, bool real_input) {
    const size_t size = block.fft_size();
    const double bin_hz = block.bin_hz();
    if (size < 16 || !(bin_hz > 0.0)) return;

    if (size != fft_size_ || bin_hz != bin_hz_) bin_power_ = 0.0;
    fft_size_ = size;
    bin_hz_ = bin_hz;

    // Moved an eighth of the window since the last look, by a retune or bit
    // by bit in a drag: that is somewhere else, so look now and start from
    // what is there, rather than gliding over from the old place's noise for
    // a second, during which a noisier place would come out too loud.
    if (std::fabs(center_hz - look_center_hz_) > static_cast<double>(kWindowBins) * bin_hz / 8.0) bin_power_ = 0.0;

    since_look_s_ += block.block_seconds();
    if (bin_power_ > 0.0 && since_look_s_ < kIntervalSeconds) return;
    since_look_s_ = 0.0;
    look_center_hz_ = center_hz;

    // Kept inside the part of the transform that holds signal. A complex
    // window that crossed Nyquist would join the band's two edges, which are
    // different places, and the front end's anti-alias filter has rolled off
    // there; a real input's upper half is empty.
    const long half = static_cast<long>(size / 2);
    const long lowest = real_input ? 0 : -half;
    const long span = half - lowest;
    const long count = std::min<long>(static_cast<long>(kWindowBins), span);
    const long centre = std::lround(center_hz / bin_hz);
    const long first = std::clamp(centre - count / 2, lowest, half - count);

    const double instant =
        noise_bin_power(block.spectrum_re(), block.spectrum_im(), size, first, static_cast<size_t>(count), scratch_);
    if (!(instant > 0.0) || !std::isfinite(instant)) return;
    if (bin_power_ <= 0.0) {
        bin_power_ = instant;
        return;
    }
    const double alpha = 1.0 - std::exp(-kIntervalSeconds / kSettleSeconds);
    bin_power_ += alpha * (instant - bin_power_);
}

double ChannelNoise::channel_power(double bandwidth_hz) const {
    if (bin_power_ <= 0.0 || fft_size_ == 0 || !(bin_hz_ > 0.0)) return 0.0;
    const double k = static_cast<double>(fft_size_);
    return 2.0 * bin_power_ * std::fabs(bandwidth_hz) / (k * k * bin_hz_);
}

}  // namespace fernsdr
