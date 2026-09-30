// One decoder channel cut from a band: a few kilohertz of complex baseband
// taken from the band's shared transform, the way a listener's channel is,
// and handed on in frames that say which samples they are and when those
// samples arrived. See docs/MODULES.md, "Decoders".
#pragma once

#include "../dsp/channelizer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace fernsdr {

struct TapFrame {
    uint16_t channel = 0;
    // 1: samples were lost before this frame; 2: the clock was set again.
    uint16_t flags = 0;
    uint64_t index = 0;
    int64_t utc_us = 0;
    std::vector<cfloat> samples;
};

class DecoderTap {
public:
    static constexpr uint16_t kGap = 1;
    static constexpr uint16_t kReanchored = 2;

    // `channel` is the tap's index in its decoder's list. `dial_hz` is the
    // RF frequency a receiver would be set to; the channel covers
    // dial + offset - width/2 to dial + offset + width/2. `origin_hz` and
    // `rf_scale` are the band's (see Band::spectrum_origin_hz, rf_scale).
    DecoderTap(uint16_t channel, const Channelizer& channelizer, double origin_hz, double rf_scale,
               double dial_hz, double offset_hz, double width_hz);

    double rate() const { return channel_->output_rate(); }
    double dial_hz() const { return dial_hz_; }
    double offset_hz() const { return offset_hz_; }
    double width_hz() const { return width_hz_; }

    // Called once per band block, on the band's thread, after the block has
    // been transformed. `now_us` is the wall clock when the block arrived.
    void process(const Channelizer& channelizer, int64_t now_us);
    void process(const ChannelBlock& block, int64_t now_us);

    // The band stopped or restarted: the next frame says the clock was set
    // again, and the anchor starts over.
    void reanchor();

    // Frames ready to go, oldest first; at most `max_frames` are kept and the
    // oldest are dropped past that, with the next frame flagged as a gap.
    // Safe from another thread.
    std::vector<TapFrame> take_frames();

    // The anchor in use: UTC of sample 0, in microseconds. For tests.
    int64_t anchor_us() const { return anchor_us_; }

    // Samples per frame: about a quarter of a second.
    size_t frame_samples() const { return frame_samples_; }

    // How many of this tap's samples a decoder may hold before it drops
    // frames: three seconds for a slow decoder, on top of the block that
    // arrives all at once, which with a large transform on a slow band can
    // itself be longer than that.
    size_t pending_budget() const {
        return static_cast<size_t>(rate() * 3.0) + channel_->output_per_block() + frame_samples_;
    }

private:
    void take(const cfloat* samples, size_t count, int64_t now_us);

    uint16_t index_in_decoder_;
    double dial_hz_;
    double offset_hz_;
    double width_hz_;
    std::unique_ptr<Channel> channel_;
    std::vector<cfloat> scratch_;
    // What comes out of a channel went into the band one channelizer block
    // earlier: measured with a burst, exactly the block, whatever the
    // channel's rate. 170 ms on a band with a 65536-point transform at
    // 192 kHz, which is more than a decoder's DT can ignore.
    int64_t delay_us_ = 0;

    // Where the frame being filled starts, and what is in it.
    TapFrame building_;
    size_t frame_samples_ = 0;
    uint64_t next_index_ = 0;
    uint16_t pending_flags_ = kReanchored;

    // The UTC time of sample 0, from the earliest arrival seen: a sample can
    // arrive late (buffering, scheduling, the network) but never early. Kept
    // over two windows so that a clock that drifts against the samples is
    // followed rather than held to a minimum from an hour ago.
    int64_t anchor_us_ = 0;
    bool anchored_ = false;
    int64_t window_min_us_ = 0;
    int64_t previous_window_min_us_ = 0;
    uint64_t window_start_index_ = 0;
    uint64_t window_samples_ = 0;

    std::mutex frames_mutex_;
    std::deque<TapFrame> ready_;
    static constexpr size_t kMaxFrames = 64;  // about 16 seconds
};

}  // namespace fernsdr
