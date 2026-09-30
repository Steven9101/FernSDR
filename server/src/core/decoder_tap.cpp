#include "decoder_tap.h"

#include "listener.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

DecoderTap::DecoderTap(uint16_t channel, const Channelizer& channelizer, double origin_hz, double rf_scale,
                       double dial_hz, double offset_hz, double width_hz)
    : index_in_decoder_(channel), dial_hz_(dial_hz), offset_hz_(offset_hz), width_hz_(width_hz) {
    const double half = width_hz / 2.0;
    // The lowest rate that still carries the channel: what a decoder does
    // costs in proportion to the rate. Unlike a listener's there is no
    // ceiling, since a channel the section says is 48 kHz wide must be.
    const size_t decimation = wfm_channel_decimation(channelizer.sample_rate(), channelizer.fft_size(), -half, half);
    channel_ = std::make_unique<Channel>(channelizer, channelizer.fft_size() / std::max<size_t>(1, decimation));
    const double centre = (dial_hz + offset_hz - origin_hz) / rf_scale;
    channel_->set_passband(centre, -half, half);
    scratch_.resize(channel_->output_per_block());
    frame_samples_ = std::max<size_t>(channel_->output_per_block(),
                                      static_cast<size_t>(channel_->output_rate() * 0.25));
    window_samples_ = static_cast<uint64_t>(channel_->output_rate() * 30.0);
    delay_us_ = static_cast<int64_t>(std::llround(channelizer.block_seconds() * 1e6));
}

void DecoderTap::process(const Channelizer& channelizer, int64_t now_us) {
    channel_->pull(channelizer, scratch_.data());
    take(scratch_.data(), scratch_.size(), now_us);
}

void DecoderTap::process(const ChannelBlock& block, int64_t now_us) {
    channel_->pull(block, scratch_.data());
    take(scratch_.data(), scratch_.size(), now_us);
}

void DecoderTap::reanchor() {
    anchored_ = false;
    pending_flags_ |= kReanchored;
    // A frame half built before is dropped: its samples were stamped with
    // the old anchor, and the new ones would be joined on to them as if no
    // time had passed.
    building_ = TapFrame();
}

void DecoderTap::take(const cfloat* samples, size_t count, int64_t now_us) {
    const double rate = channel_->output_rate();
    // The last of these samples cannot have arrived after now, so the first
    // sample of all cannot have been earlier than this; the smallest such
    // value seen is the best estimate of when it was.
    const int64_t candidate =
        now_us - static_cast<int64_t>(std::llround(static_cast<double>(next_index_ + count) * 1e6 / rate));
    if (!anchored_) {
        anchored_ = true;
        anchor_us_ = window_min_us_ = previous_window_min_us_ = candidate;
        window_start_index_ = next_index_;
    } else {
        window_min_us_ = std::min(window_min_us_, candidate);
        if (next_index_ - window_start_index_ >= window_samples_) {
            previous_window_min_us_ = window_min_us_;
            window_min_us_ = candidate;
            window_start_index_ = next_index_;
        }
        anchor_us_ = std::min(window_min_us_, previous_window_min_us_);
    }

    size_t used = 0;
    while (used < count) {
        if (building_.samples.empty()) {
            building_.channel = index_in_decoder_;
            building_.flags = pending_flags_;
            pending_flags_ = 0;
            building_.index = next_index_;
            building_.utc_us = anchor_us_ - delay_us_ +
                               static_cast<int64_t>(std::llround(static_cast<double>(next_index_) * 1e6 / rate));
            building_.samples.reserve(frame_samples_);
        }
        const size_t n = std::min(count - used, frame_samples_ - building_.samples.size());
        building_.samples.insert(building_.samples.end(), samples + used, samples + used + n);
        used += n;
        next_index_ += n;
        if (building_.samples.size() == frame_samples_) {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            if (ready_.size() >= kMaxFrames) {
                ready_.pop_front();
                // The frame after the dropped one is the first a decoder will
                // see again; it has to know something is missing before it.
                if (!ready_.empty()) ready_.front().flags |= kGap;
            }
            ready_.push_back(std::move(building_));
            building_ = TapFrame();
        }
    }
}

std::vector<TapFrame> DecoderTap::take_frames() {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    std::vector<TapFrame> out(std::make_move_iterator(ready_.begin()), std::make_move_iterator(ready_.end()));
    ready_.clear();
    return out;
}

}  // namespace fernsdr
