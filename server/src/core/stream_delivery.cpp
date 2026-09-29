#include "stream_delivery.h"
#include "protocol.h"

namespace fernsdr {
namespace {
bool is_audio(const uint8_t* data, size_t size) {
    return size >= proto::kAudioHeaderBytes && data[0] == proto::kStreamAudio;
}
bool is_waterfall(const uint8_t* data, size_t size) {
    return size > proto::kWaterfallHeaderBytes && data[0] == proto::kStreamWaterfall;
}
bool independent_waterfall(const uint8_t* data) {
    const auto prefix = data[proto::kWaterfallHeaderBytes];
    const auto mode = data[1] & 2 ? prefix >> 6 : prefix >> 7;
    return mode == 1 || mode == 3;
}
int sequence(const uint8_t* data) { return data[2] | (data[3] << 8); }
}

bool StreamDelivery::repair_payload(uint8_t* payload, size_t size, Repair& repair) const {
    if (is_audio(payload, size) && repair.audio) {
        if (audio_discontinuity_) payload[1] |= proto::kAudioFlagDiscontinuity;
        repair.audio = false;
    } else if (is_waterfall(payload, size)) {
        if (independent_waterfall(payload)) repair.waterfall = false;
        else if (repair.waterfall) return false;
    }
    return true;
}

bool StreamDelivery::prepare(std::vector<uint8_t>& payload, bool discard_media) {
    if (is_audio(payload.data(), payload.size())) {
        const int generation = payload[1] >> 4;
        if (generation == audio_generation_ && audio_sequence_ >= 0 &&
            sequence(payload.data()) != ((audio_sequence_ + audio_frames_) & 0xffff)) pending_.audio = true;
        audio_generation_ = generation;
        audio_sequence_ = sequence(payload.data());
        audio_frames_ = proto::audio_frames(payload.data(), payload.size());
        if (discard_media) { pending_.audio = true; return false; }
    } else if (is_waterfall(payload.data(), payload.size())) {
        if (waterfall_sequence_ >= 0 && sequence(payload.data()) != ((waterfall_sequence_ + 1) & 0xffff)) {
            pending_.waterfall = true;
        }
        waterfall_sequence_ = sequence(payload.data());
        if (discard_media) { pending_.waterfall = true; return false; }
    }
    return repair_payload(payload.data(), payload.size(), pending_);
}

bool StreamDelivery::retain(uint8_t* payload, size_t size, bool expired, Repair& repair) const {
    if (expired) {
        if (is_audio(payload, size) && audio_discontinuity_) { repair.audio = true; return false; }
        if (is_waterfall(payload, size)) { repair.waterfall = true; return false; }
    }
    return repair_payload(payload, size, repair);
}

void StreamDelivery::finish_expiry(const Repair& repair) {
    // pending_ describes a loss at the tail, after the already queued frames.
    // An older keyframe encountered while scanning those frames cannot repair
    // that later loss. A scan therefore carries its own repair state forward
    // and only adds unresolved repairs to the tail when it finishes.
    pending_.audio |= repair.audio;
    pending_.waterfall |= repair.waterfall;
}

}  // namespace fernsdr
