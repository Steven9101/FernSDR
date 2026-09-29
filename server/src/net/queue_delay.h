#pragma once
#include <algorithm>
#include <cstdint>

namespace fernsdr {

// How far a connection's round trip stands above its floor, in milliseconds:
// the queue in the network beyond its socket.
//
// A queue in a slow link's modem or a mobile cell holds the audio behind the
// waterfall as surely as the socket's own, but the socket shows none of it:
// the kernel has handed the bytes on. The kernel's smoothed round trip shows
// it. Its floor is the lowest of the last 15 to 30 seconds, so a route that
// changes is followed within half a minute while a queue lasting seconds is
// not taken for one.
//
// A hold is not a queue, though. Wi-Fi retrying or a phone changing cells
// holds the packets in flight and delivers them late, and their
// acknowledgements come back with round trips as long as the hold. The
// smoothed round trip takes those in and sheds them over the next half
// second: over the kernel's TCP, a one-second hold on a 20 ms path came back
// as 300 ms for a tenth of a second, a three-second hold on a 120 ms path as
// up to 1.5 s, over 50 ms for 0.6 s. Read as a queue, each of those echoes
// lowered the waterfall for twenty seconds. Nothing comes back while a hold
// lasts, so after 200 ms without an acknowledgement for packets in flight the
// delay stays at what it was before, until a second and a half after the
// last such gap, which leaves room for longer holds and for the slower
// acknowledgements of a stream that has lost its waterfall. It stays rather
// than dropping to nothing because a link
// whose queue has grown until the kernel retransmits leaves gaps as well,
// and that link is still queued.
//
// An outage that loses the packets rather than holding them hardly moves the
// round trip: TCP times a retransmission by its own timestamps.
class QueueDelay {
public:
    // The kernel's view of the connection, every 100 ms or so: its smoothed
    // round trip, the packets in flight and the time since the last
    // acknowledgement.
    void update(int64_t now_ms, uint32_t rtt_us, uint32_t unacked, uint32_t ack_age_ms) {
        if (rtt_us == 0) return;
        if (unacked > 0 && ack_age_ms >= kGapMs) held_until_ms_ = now_ms + kEchoMs;
        if (now_ms - window_ms_ >= kWindowMs) {
            floor_us_[1] = floor_us_[0];
            floor_us_[0] = UINT32_MAX;
            window_ms_ = now_ms;
        }
        floor_us_[0] = std::min(floor_us_[0], rtt_us);
        if (now_ms < held_until_ms_) return;
        delay_ms_ = static_cast<int>((rtt_us - std::min(floor_us_[0], floor_us_[1])) / 1000);
    }

    int ms() const { return delay_ms_; }

private:
    static constexpr uint32_t kGapMs = 200;
    static constexpr int64_t kEchoMs = 1500;
    static constexpr int64_t kWindowMs = 15000;

    // The lowest round trip of the current and the previous window.
    uint32_t floor_us_[2] = {UINT32_MAX, UINT32_MAX};
    int64_t window_ms_ = 0;
    int64_t held_until_ms_ = 0;
    int delay_ms_ = 0;
};

}  // namespace fernsdr
