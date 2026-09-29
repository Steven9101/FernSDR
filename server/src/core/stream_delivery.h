#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fernsdr {

// Network-thread state. The byte queue decides which frames are still safe
// to edit; this policy decides how their audio and waterfall decoders recover.
class StreamDelivery {
public:
    static constexpr int64_t kMaxQueueAgeMs = 250;
    struct Repair { bool audio = false; bool waterfall = false; };

    void set_audio_discontinuity(bool enabled) { audio_discontinuity_ = enabled; }
    bool audio_discontinuity() const { return audio_discontinuity_; }
    bool needs_keyframe() const { return pending_.waterfall; }

    bool prepare(std::vector<uint8_t>& payload, bool discard_media = false);
    bool retain(uint8_t* payload, size_t size, bool expired, Repair& repair) const;
    void finish_expiry(const Repair& repair);

private:
    bool repair_payload(uint8_t* payload, size_t size, Repair& repair) const;
    Repair pending_;
    bool audio_discontinuity_ = false;
    int audio_generation_ = -1;
    int audio_sequence_ = -1;
    int audio_frames_ = 1;  // in the last audio message, which the next sequence follows
    int waterfall_sequence_ = -1;
};

}  // namespace fernsdr
