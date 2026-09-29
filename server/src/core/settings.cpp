#include "settings.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

void apply_mode_defaults(ChannelSettings& settings) {
    const Passband passband = default_passband(settings.mode, settings.cw_pitch_hz);
    settings.bandwidth_low = passband.low;
    settings.bandwidth_high = passband.high;
}

std::string sanitise(ChannelSettings& settings, double band_low_hz, double band_high_hz,
                     double max_bandwidth_hz) {
    std::string note;
    // Broadcast FM takes its own channel whatever the band's limit for the
    // other modes: it is no use narrower, and the band only offers it where
    // the operator lets it (Band::wfm).
    if (settings.mode == Mode::Wfm) max_bandwidth_hz = kWfmMaxBandwidthHz;

    if (!std::isfinite(settings.frequency_hz)) settings.frequency_hz = (band_low_hz + band_high_hz) / 2;
    if (!std::isfinite(settings.bandwidth_low)) settings.bandwidth_low = -max_bandwidth_hz / 2;
    if (!std::isfinite(settings.bandwidth_high)) settings.bandwidth_high = max_bandwidth_hz / 2;
    if (std::abs(settings.bandwidth_low) > max_bandwidth_hz || std::abs(settings.bandwidth_high) > max_bandwidth_hz) {
        settings.bandwidth_low = std::clamp(settings.bandwidth_low, -max_bandwidth_hz, max_bandwidth_hz);
        settings.bandwidth_high = std::clamp(settings.bandwidth_high, -max_bandwidth_hz, max_bandwidth_hz);
        note = "passband clamped to the channel range";
    }

    if (!(settings.frequency_hz >= band_low_hz && settings.frequency_hz <= band_high_hz)) {
        settings.frequency_hz = std::clamp(settings.frequency_hz, band_low_hz, band_high_hz);
        note = "frequency clamped to the band";
    }

    if (settings.bandwidth_high < settings.bandwidth_low) {
        std::swap(settings.bandwidth_low, settings.bandwidth_high);
    }
    // A zero-width passband would produce silence and a confused user.
    if (settings.bandwidth_high - settings.bandwidth_low < 50.0) {
        const double centre = (settings.bandwidth_low + settings.bandwidth_high) / 2.0;
        settings.bandwidth_low = centre - 25.0;
        settings.bandwidth_high = centre + 25.0;
        note = "passband widened to the 50 Hz minimum";
    }
    if (settings.bandwidth_high - settings.bandwidth_low > max_bandwidth_hz) {
        const double centre = (settings.bandwidth_low + settings.bandwidth_high) / 2.0;
        settings.bandwidth_low = centre - max_bandwidth_hz / 2.0;
        settings.bandwidth_high = centre + max_bandwidth_hz / 2.0;
        note = "passband narrowed to the channel maximum";
    }

    if (settings.mode == Mode::Wfm && settings.bandwidth_high - settings.bandwidth_low < kWfmMinBandwidthHz) {
        const double centre = (settings.bandwidth_low + settings.bandwidth_high) / 2.0;
        settings.bandwidth_low = centre - kWfmMinBandwidthHz / 2.0;
        settings.bandwidth_high = centre + kWfmMinBandwidthHz / 2.0;
        note = "passband widened to the broadcast FM minimum";
    }
    settings.wfm_deemphasis_us = std::isfinite(settings.wfm_deemphasis_us)
        ? std::clamp(settings.wfm_deemphasis_us, 0.0f, 200.0f) : 50.0f;
    settings.cw_pitch_hz = std::clamp(settings.cw_pitch_hz, 200.0, 1500.0);
    settings.manual_gain_db = std::clamp(settings.manual_gain_db, -40.0f, 60.0f);
    settings.max_gain_db = std::clamp(settings.max_gain_db, 0.0f, 100.0f);
    settings.noise_reduction = std::clamp(settings.noise_reduction, 0.0f, 1.0f);
    settings.volume = std::clamp(settings.volume, 0.0f, 4.0f);
    settings.squelch_dbfs = std::clamp(settings.squelch_dbfs, -200.0, 0.0);
    settings.audio_bitrate = std::clamp(settings.audio_bitrate, 8000, 128000);
    settings.audio_packet_frames = std::clamp(settings.audio_packet_frames, 1, 4);
    settings.audio_noise_margin_db = std::isfinite(settings.audio_noise_margin_db)
        ? std::clamp(settings.audio_noise_margin_db, 6.0f, 30.0f) : 12.0f;
    settings.requested_audio_rate = std::clamp(settings.requested_audio_rate, 4000, 48000);

    // More than a handful of notches is a client bug, not a use case.
    if (settings.notches.size() > 8) settings.notches.resize(8);
    for (auto& notch : settings.notches) {
        notch.center_hz = std::clamp(notch.center_hz, 0.0, settings.requested_audio_rate / 2.0);
        notch.width_hz = std::clamp(notch.width_hz, 1.0, max_bandwidth_hz);
    }

    return note;
}

std::string sanitise(ViewportSettings& viewport, double band_low_hz, double band_high_hz, int max_width,
                     double max_lines_per_second) {
    std::string note;

    if (viewport.high_hz <= viewport.low_hz) {
        viewport.low_hz = band_low_hz;
        viewport.high_hz = band_high_hz;
        note = "viewport reset to the full band";
    }
    // Allow panning a little past the edges so the display does not fight the
    // user, but not so far that the whole view is empty.
    const double span = band_high_hz - band_low_hz;
    viewport.low_hz = std::max(viewport.low_hz, band_low_hz - span);
    viewport.high_hz = std::min(viewport.high_hz, band_high_hz + span);
    if (viewport.high_hz <= viewport.low_hz) {
        viewport.low_hz = band_low_hz;
        viewport.high_hz = band_high_hz;
    }

    if (viewport.width < 16 || viewport.width > max_width) {
        viewport.width = std::clamp(viewport.width, 16, max_width);
        note = "waterfall width clamped";
    }
    if (viewport.lines_per_second <= 0.0 || viewport.lines_per_second > max_lines_per_second) {
        viewport.lines_per_second = std::clamp(viewport.lines_per_second, 1.0, max_lines_per_second);
        note = "waterfall rate clamped";
    }
    return note;
}

}  // namespace fernsdr
