#include "shared_fm.h"

#include <algorithm>
#include <cmath>

#include "../dsp/simd.h"
#include "band.h"
#include "listener.h"

namespace fernsdr {

FmKey FmKey::for_channel(double frequency_hz, double low_hz, double high_hz, float deemphasis_us) {
    FmKey key;
    key.frequency_hz = std::round(frequency_hz / 1000.0) * 1000.0;
    key.low_hz = low_hz;
    key.high_hz = high_hz;
    key.deemphasis_us = deemphasis_us;
    return key;
}

SharedFm::SharedFm(const Band& band, const FmKey& key) : key_(key) {
    const size_t decimation = wfm_channel_decimation(band.sample_rate(), band.fft_size(), key.low_hz, key.high_hz);
    channel_ = std::make_unique<Channel>(band.channelizer(), std::max<size_t>(16, band.fft_size() / decimation));
    channel_->set_passband((key.frequency_hz - band.spectrum_origin_hz()) / band.rf_scale(), key.low_hz, key.high_hz);
    const double channel_rate = channel_->output_rate();
    demodulator_.configure(Mode::Wfm, channel_rate);
    const size_t factor = std::max<size_t>(1, static_cast<size_t>(channel_rate / kWfmMinimumAudioRate));
    decimator_.configure(channel_rate, factor, kWfmAudioHz, kWfmStopHz, 70.0, key.deemphasis_us);
    // ~20 Hz, as the demodulator's own blocker for the other modes.
    dc_alpha_ = static_cast<float>(std::exp(-2.0 * M_PI * 20.0 / decimator_.output_rate()));
    // The subcarrier and its data reach 59.4 kHz, which the multiplex keeps
    // only at a channel rate over twice that. The narrowest WFM filter,
    // 120 kHz, already asks for a rate above 120 kHz, so this is a guard.
    if (channel_rate >= 120000.0) rds_ = std::make_unique<RdsDecoder>(channel_rate);
    baseband_.assign(channel_->output_per_block(), cfloat(0.0f, 0.0f));
    discriminated_.assign(channel_->output_per_block(), 0.0f);
}

void SharedFm::process(const ChannelBlock& block) {
    const size_t count = channel_->output_per_block();
    channel_->pull(block, baseband_.data());
    // The level before demodulation: the S-meter reads the antenna.
    const float* floats = reinterpret_cast<const float*>(baseband_.data());
    const double power = simd::dot(floats, floats, 2 * count);
    level_dbfs_ = static_cast<float>(10.0 * std::log10(power / static_cast<double>(count) + 1e-30));
    demodulator_.process(baseband_.data(), count, discriminated_.data());
    if (rds_) rds_->process(discriminated_.data(), count);
    audio_.resize(count);
    const size_t heard = decimator_.process(discriminated_.data(), count, audio_.data());
    audio_.resize(heard);
    // A tuning error is DC after the discriminator: out before the codec.
    for (float& sample : audio_) {
        dc_state_ = dc_alpha_ * dc_state_ + (1.0f - dc_alpha_) * sample;
        sample -= dc_state_;
    }
}

}  // namespace fernsdr
