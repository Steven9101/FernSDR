// One user's receiver.
//
// Owns everything per-user: the channel slice, demodulator, AGC, audio
// post-processing and both encoders.  Deliberately small - the expensive
// shared work lives in Band - because this is the object that exists 200
// times over.
//
// Threading: settings arrive from the network thread and are applied by the
// DSP thread at a block boundary.  Encoded frames go the other way through a
// mutex-protected outbox.  Nothing else is shared.
#pragma once
#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../codec/nac.h"
#include "../codec/waterfall_codec.h"
#include "../codec/waterfall_rc.h"
#include "../dsp/spectrum.h"
#include "../dsp/agc.h"
#include "../dsp/channel_noise.h"
#include "../dsp/squelch.h"
#include "../dsp/audio_post.h"
#include "../dsp/channelizer.h"
#include "../dsp/biquad.h"
#include "../dsp/demod.h"
#include "../dsp/ctcss.h"
#include "../dsp/cw_filter.h"
#include "shared_fm.h"
#include "shared_waterfall.h"
#include "settings.h"
#include "stream_budget.h"

namespace fernsdr {
double channel_audio_rate(double band_rate, int requested_rate, size_t fft_size);
// The rate a listener's audio actually runs at: the lowest the band's
// channelizer offers that still carries the passband, never above
// channel_audio_rate() nor below 4 kHz.
double passband_audio_rate(double band_rate, int requested_rate, size_t fft_size, double low_hz, double high_hz);
// The channel broadcast FM is demodulated at: the lowest the passband fits in.
double wfm_channel_rate(double band_rate, size_t fft_size, double low_hz, double high_hz);
size_t wfm_channel_decimation(double band_rate, size_t fft_size, double low_hz, double high_hz);

class Band;

// What the DSP thread reports back for the UI's meters.
struct ListenerTelemetry {
    float signal_dbfs = -140.0f;
    /** How far the passband is from looking like noise. See AutoSquelch. */
    float squelch_statistic = 0.0f;
    float agc_gain_db = 0.0f;
    bool squelch_open = true;
    bool pll_locked = false;
    double pll_offset_hz = 0.0;
    // The CTCSS tone on NFM, in Hz; 0 for none, or in any other mode.
    double ctcss_hz = 0.0;
    int audio_bitrate = 0;
    int waterfall_bitrate = 0;
    // The line rate actually being sent, which may be below the requested one
    // when the budget binds.  Shown in the UI so the reduction is visible
    // rather than mysterious.
    double waterfall_lines_per_second = 0.0;
    // Moves whenever the RDS changes or the station does; see rds().
    // rds_station moves only with the station (or leaving broadcast FM).
    uint64_t rds_sequence = 0;
    uint64_t rds_station = 0;
};

class Listener {
public:
    Listener(uint64_t id, const Band& band, uint8_t generation_seed = 0);

    // Ceiling on this listener's combined stream, in bits per second.  Audio
    // is protected; the waterfall line rate is what gives way.
    void set_bitrate_budget(int bits_per_second);

    uint64_t id() const { return id_; }

    // --- network thread ---

    // Replaces the channel settings as one atomic update.
    void set_channel(const ChannelSettings& settings);
    void set_viewport(const ViewportSettings& viewport);
    ChannelSettings channel() const;
    ViewportSettings viewport() const;

    // The audio sample rate actually in use, which is the band rate divided
    // by a power of two and so rarely a round number.  Announced to the
    // client, which resamples.
    // The rate of this listener's audio: follows the passband, so that a
    // narrow mode runs at a lower rate than a wide one.
    double actual_audio_rate() const;
    uint8_t audio_generation() const;

    // Moves all queued binary messages out.  Called by the network thread.
    struct AudioFormat { double rate = 0.0; uint8_t generation = 0; };
    void drain(std::vector<std::vector<uint8_t>>& out, AudioFormat* format = nullptr);
    size_t queued_bytes() const;

    ListenerTelemetry telemetry() const;
    // Broadcast FM's RDS as last received, kept apart from the telemetry so
    // the meter's ten readings a second do not copy its text: a session asks
    // for it only when telemetry().rds_sequence has moved.
    RdsState rds() const;
    void request_waterfall_keyframe() { waterfall_reset_.store(true, std::memory_order_relaxed); }
    void set_transport_backlog(size_t bytes, int queue_delay_ms = 0) {
        transport_backlog_.store(bytes, std::memory_order_relaxed);
        queue_delay_ms_.store(queue_delay_ms, std::memory_order_relaxed);
    }
    void account_control_bytes(size_t bytes) { control_bytes_.fetch_add(bytes, std::memory_order_relaxed); }
    void note_media_expiry() { media_expired_.store(true, std::memory_order_relaxed); }

    // --- DSP thread ---

    // Produces audio (and, when due, a waterfall line) for one band block.
    // `spectrum` is the band's shared pyramid, or null on blocks that did not
    // produce a line.
    // `paired`, when given, holds each line averaged with the one before; a
    // listener showing no more than half the band's lines draws from it.
    void process_block(const Channelizer& channelizer, const SpectrumPyramid* spectrum,
                       const SpectrumPyramid* paired = nullptr);
    void process_block(const ChannelBlock& block, const SpectrumPyramid* spectrum,
                       const SpectrumPyramid* paired = nullptr);

private:
    void apply_pending();
    void rebuild_channel();
    // The channel decimation the current settings need.
    size_t wanted_decimation() const;
    FmKey wfm_key() const {
        return FmKey::for_channel(active_.frequency_hz, active_.bandwidth_low, active_.bandwidth_high,
                                  active_.wfm_deemphasis_us);
    }
    void configure_filter();
    // The gain control for the mode and settings in `active_`; the one place
    // that decides it.
    void configure_agc();
    void emit_audio_frame(const std::vector<uint8_t>& payload, bool muted);
    void emit_audio_message(const std::vector<uint8_t>& payload, uint16_t sequence, int frames,
                            uint8_t flags);
    // NAC3: finishes the packet being built and queues it.
    void flush_audio_packet();
    // What NAC3 should preserve for this mode and passband.
    nac::Nac3Target audio_target() const;
    void release_held_line();
    void emit_waterfall_line(const std::vector<uint8_t>& payload, double low_hz, double high_hz,
                             uint16_t width, bool native_grid);
    // Returns false when the message was dropped for backpressure.
    bool enqueue(std::vector<uint8_t>&& message, bool droppable);

    uint64_t id_;
    const Band& band_;
    double band_sample_rate_;
    double band_center_hz_;
    double spectrum_origin_hz_;
    // See Band::rf_scale().
    double band_rf_scale_;
    size_t band_fft_size_;

    mutable std::mutex settings_mutex_;
    ChannelSettings pending_channel_;
    ViewportSettings pending_viewport_;
    bool channel_dirty_ = true;
    bool viewport_dirty_ = true;
    // Raised with either dirty flag, under the mutex, so that the DSP thread
    // can see there is nothing to apply without taking it every block.
    std::atomic<bool> settings_pending_{true};

    // DSP-thread-only state.
    ChannelSettings active_;
    ViewportSettings active_viewport_;
    std::unique_ptr<Channel> channel_;
    Demodulator demodulator_;
    // Broadcast FM only: the station's demodulation, shared with everyone on
    // it (see SharedFm); this listener's channel is not pulled then.
    std::shared_ptr<SharedFm> fm_;
    // The shared rows for this view, and whether this listener is sending
    // them rather than its own.
    std::shared_ptr<SharedWaterfall> shared_waterfall_;
    bool waterfall_joined_ = false;
    bool waterfall_lost_ = false;  // a row was dropped since this listener joined
    void leave_shared_waterfall(bool at_once);
    // How many times the full line rate the budget would carry.
    double waterfall_room_ = 0.0;
    void note_waterfall_line(size_t payload_bytes);
    // The RDS last put in the telemetry: from which station, at which count.
    const SharedFm* rds_source_ = nullptr;
    uint64_t rds_seen_ = 0;
    RdsState rds_;  // under telemetry_mutex_
    CtcssDetector ctcss_;
    Biquad tone_notch_;
    Biquad highpass_;
    CwFilter cw_filter_;
    void gather_passband(const ChannelBlock& channelizer);
    // Where the tuned frequency sits in the band's samples, in hertz from the
    // spectrum origin as the samples count them. The tuned frequency is an RF
    // frequency, so a crystal error the band corrects for is taken out here.
    double channel_offset_hz() const { return (active_.frequency_hz - spectrum_origin_hz_) / band_rf_scale_; }
    /** The configured bitrate, adjusted for what this mode actually needs. */
    int scaled_bitrate() const;

    Agc agc_;
    // The band noise around the channel, for the AGC; looked at only while
    // the AGC is on.
    ChannelNoise channel_noise_;
    bool real_input_ = false;
    bool narrow_cw_ = false;
    AutoSquelch auto_squelch_;
    // The passband's bins, gathered once a block for the automatic squelch.
    std::vector<float> squelch_re_;
    std::vector<float> squelch_im_;
    AudioPost post_;
    std::unique_ptr<nac::Encoder> encoder_;
    wfc::LineEncoder waterfall_encoder_;
    wfc::RangedLineEncoder ranged_encoder_;

    double actual_audio_rate_ = 0.0;
    // The channel's decimation from the band rate; see channel_decimation().
    size_t decimation_ = 0;
    uint8_t audio_generation_ = 0;
    bool needs_channel_rebuild_ = true;

    std::vector<cfloat> baseband_;
    std::vector<float> audio_;
    std::vector<float> pending_audio_;   // carries the remainder between blocks
    std::vector<float> viewport_line_;

    uint16_t audio_sequence_ = 0;   // counts frames, not messages
    // The NAC3 packet under construction: its frames share one header, one
    // squelch state and the sequence of their first frame.
    int packet_frames_ = 0;
    bool packet_muted_ = false;
    uint16_t packet_sequence_ = 0;
    uint16_t waterfall_sequence_ = 0;
    double waterfall_credit_ = 0.0;
    bool force_intra_ = true;
    std::atomic<int> bitrate_budget_{100000};
    std::atomic<bool> waterfall_reset_{false};
    double keyframe_seconds_ = 0;
    std::atomic<size_t> transport_backlog_{0};
    std::atomic<int> queue_delay_ms_{0};
    std::atomic<size_t> control_bytes_{0};
    std::atomic<bool> media_expired_{false};
    // The audio bitrate the DSP thread last put in the telemetry, kept here
    // too so that it can read its own figure back without the lock.
    int measured_audio_bitrate_ = 0;
    // A waterfall line waiting to leave with the next audio message; see
    // emit_waterfall_line().
    std::vector<uint8_t> held_line_;
    double held_line_seconds_ = 0.0;
    StreamBudget stream_budget_;
    double waterfall_bits_per_line_ = 0.0;  // smoothed estimate
    double audio_wire_bits_per_frame_ = 0.0;
    double effective_lines_per_second_ = 0.0;

    mutable std::mutex outbox_mutex_;
    std::deque<std::vector<uint8_t>> outbox_;
    size_t outbox_bytes_ = 0;
    AudioFormat outbox_format_;
    // Whether drain() has anything to hand over: a message, or a new audio
    // format. Set and cleared only under outbox_mutex_; read without it, so
    // that the network thread's pass over every listener takes no lock for
    // the many that have nothing new.
    std::atomic<bool> outbox_ready_{false};
    uint64_t dropped_messages_ = 0;

    mutable std::mutex telemetry_mutex_;
    ListenerTelemetry telemetry_;
    uint64_t audio_bits_ = 0;
    uint64_t waterfall_bits_ = 0;
    double telemetry_window_seconds_ = 0.0;
    double bitrate_window_seconds_ = 0.0;
};

}  // namespace fernsdr
