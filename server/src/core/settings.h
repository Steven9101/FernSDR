// User-controllable receiver settings.
//
// Kept as plain structs so the network thread can hand a whole coherent set
// to the DSP thread in one go: applying fields individually would let a
// listener be briefly tuned with the old mode's passband, which is audible.
#pragma once
#include <string>
#include <vector>

#include "../dsp/agc.h"
#include "../dsp/audio_post.h"
#include "../dsp/demod.h"

namespace fernsdr {

struct ChannelSettings {
    double frequency_hz = 0.0;  // absolute RF frequency of the tuning point
    Mode mode = Mode::Usb;
    // Passband edges relative to the tuning point.
    double bandwidth_low = 300.0;
    double bandwidth_high = 2700.0;
    double cw_pitch_hz = 700.0;

    AgcProfile agc = AgcProfile::Auto;
    float manual_gain_db = 0.0f;
    float max_gain_db = 60.0f;

    float noise_reduction = 0.0f;
    bool auto_notch = false;
    std::vector<Notch> notches;
    // Audio below this is taken out after demodulation: hum on AM, the
    // subaudible tone under NFM speech. Zero leaves the audio as it is.
    float highpass_hz = 0.0f;
    // The de-emphasis time constant for NFM, in microseconds: the
    // transmitter's pre-emphasis undone. Zero switches it off, for a data
    // signal a decoder wants flat.
    float deemphasis_us = 300.0f;
    // The same for broadcast FM: 50 microseconds in most of the world, 75 in
    // the Americas and South Korea.
    float wfm_deemphasis_us = 50.0f;
    // On NFM, take out the station's CTCSS tone, once measured, with a notch
    // at its frequency. On unless the listener turns it off.
    bool ctcss_filter = true;
    // Tone squelch on NFM: the audio stays muted unless this CTCSS tone, in
    // Hz, is received. 0 for off.
    double ctcss_squelch = 0.0;

    float volume = 1.0f;
    double squelch_dbfs = -200.0;  // effectively open
    /**
     * Squelch by the shape of the passband rather than its level. Independent
     * of the manual threshold above; either can mute, neither disables the
     * other.
     */
    bool auto_squelch = false;

    bool audio_enabled = true;
    int requested_audio_rate = 12000;
    int audio_bitrate = 48000;
    bool compact_audio = false;
    // NAC3, when the client negotiated it: frames per packet, trading
    // 10.7 ms of latency (at 12 kHz) per extra frame for smaller side
    // information and fewer packet headers; and how far below the channel
    // noise the codec's own noise sits.
    bool packet_audio = false;
    int audio_packet_frames = 1;
    float audio_noise_margin_db = 12.0f;
};

// The gain control a channel gets: what was asked for, Auto being Slow, and
// none for NFM, whose discriminator hears only the phase.
inline AgcProfile agc_in_effect(const ChannelSettings& channel) {
    if (channel.mode == Mode::Nfm || channel.mode == Mode::Wfm) return AgcProfile::Off;
    return channel.agc == AgcProfile::Auto ? AgcProfile::Slow : channel.agc;
}

// Broadcast FM's channel: at most 240 kHz wide, at least 120 kHz.
constexpr double kWfmMaxBandwidthHz = 240000.0;
constexpr double kWfmMinBandwidthHz = 120000.0;

struct ViewportSettings {
    bool zero_runs = false;
    bool adaptive_codec = false;
    // WFC5: rows through the context-modelled range coder (codec/waterfall_rc.h).
    bool range_coded = false;
    bool native_grid = false;
    int step_db = 1;
    bool enabled = true;
    double low_hz = 0.0;
    double high_hz = 0.0;
    int width = 1024;
    double lines_per_second = 12.0;
};

// Applies the mode's default passband.  Called when a client changes mode
// without naming a bandwidth.
void apply_mode_defaults(ChannelSettings& settings);

// Clamps user input to something the DSP can actually honour, returning a
// human-readable note when a value had to be changed.
std::string sanitise(ChannelSettings& settings, double band_low_hz, double band_high_hz,
                     double max_bandwidth_hz);
std::string sanitise(ViewportSettings& viewport, double band_low_hz, double band_high_hz,
                     int max_width, double max_lines_per_second);

}  // namespace fernsdr
