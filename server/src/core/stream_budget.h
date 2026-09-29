#pragma once

#include <array>
#include <algorithm>
#include <cstddef>

namespace fernsdr {

// Queue growth is evidence that the link cannot carry the current stream.
// Give up waterfall traffic first, then reduce audio only if the queue keeps
// growing. Recover slowly so a marginal link does not alternate between a
// burst of high quality and a stalled player.
//
// A queue does not always mean that, though. Wi-Fi retrying, a phone changing
// cells or a relay pausing stops delivery for a second at a rate the link had
// carried for minutes, and TCP's retransmission timer adds its own wait on
// top. Taken for a slow link, a one-second stall every twenty seconds brought
// the waterfall down to one line a second within a minute, and it stayed
// there: each stall halved it again and doubled the wait before the next step
// up, and nothing ever shortened that wait.
//
// So the budget remembers the level the link carried through the last ten
// seconds with nothing queued (under 512 bytes), and returns to it as soon as
// the queue has been empty for a second after a stall. Two things lower that
// level. One is a queue that fills slowly while nothing changed on this side:
// a stall fills the queue at the stream's whole rate, a link that has become
// too slow only at the difference between the two. The other is a queue that
// follows one of the budget's own steps before it has held for three seconds,
// which also makes the next step up wait longer; the first return after a
// stall that meets a sudden queue is let off once, as the same stall again.
//
// The queue this counts is the server's own: frames waiting here and bytes
// the kernel has not sent. On a link a little too slow for the stream, the
// kernel hands the bytes on and the queue builds in the network instead, in
// the modem or the cell, where it delays the audio behind the waterfall just
// the same. In a browser over such a link that cost a dropout every twenty
// seconds while this side saw nothing. The connection's round trip does show
// it (net/queue_delay.h), so a round trip 50 ms over its recent floor counts
// as not quiet, and 150 ms over it as a queue. That queue is the waterfall's
// to answer: it stops the waterfall and lowers the level the waterfall returns
// to, but never cuts the audio; once the waterfall stops feeding it, it drains
// by itself. For the same reason a round trip only a little long does not
// hold back the audio's return after a stall: that waits for this socket to
// clear, and for a queue in the network, over 150 ms, to drain like
// everything else.
class StreamBudget {
public:
    // `measured_audio_bps` is what the audio actually used over the last
    // second. With a variable-rate codec that can sit far below the ceiling,
    // so cutting the ceiling alone would take several steps to bite.
    // `queue_delay_ms` is how far the connection's round trip is above its
    // recent floor: the queue in the network beyond this socket.
    void update(size_t queued_bytes, double seconds, int requested_audio_bps, size_t control_bytes = 0,
                bool media_expired = false, int measured_audio_bps = 0, int queue_delay_ms = 0) {
        control_bits_ += control_bytes * 8.0;
        control_seconds_ += seconds;
        if (control_seconds_ >= 1.0) {
            // Meter and station messages share the same socket. Reserve their
            // measured cost before assigning the remainder to the waterfall.
            // What is reserved is the steady part: the least of the last three
            // seconds. A welcome, or the replies to a filter dragged for a
            // second, is a one-off cost; reserved in full for the next second
            // it left an AM listener on a 100 kbit/s budget no waterfall at
            // all for two seconds after every drag. A link that cannot carry
            // the burst shows it as a backlog or a longer round trip, and that
            // stops the waterfall below anyway.
            control_windows_[control_window_++ % control_windows_.size()] = control_bits_ / control_seconds_;
            control_bps_ = *std::min_element(control_windows_.begin(), control_windows_.end());
            control_bits_ = control_seconds_ = 0;
        }
        const bool was_congested = congested_;
        // Expiring stale frames can keep a slow connection below the byte
        // threshold. Remember that pressure long enough for the audio-rate
        // controller to react, instead of refilling the same short queue at
        // a rate the connection has already failed to carry.
        if (media_expired) expiry_pressure_seconds_ = 0.35;
        const bool backlog = queued_bytes > 2048 || expiry_pressure_seconds_ > 0;
        congested_ = backlog || queue_delay_ms > kQueueDelayMs;
        expiry_pressure_seconds_ = std::max(0.0, expiry_pressure_seconds_ - seconds);
        // Nothing waiting in this socket; and nothing in the network either.
        const bool socket_clear = !backlog && queued_bytes < 512;
        const bool quiet = socket_clear && queue_delay_ms < kQuietDelayMs;
        if (quiet) quiet_seconds_ += seconds;
        else quiet_seconds_ = 0;
        if (socket_clear) filling_seconds_ = 0;
        else filling_seconds_ += seconds;
        if (congested_) {
            if (!was_congested) on_queue(!backlog);
            clear_seconds_ = 0;
            socket_clear_seconds_ = 0;
            // Only a backlog here says the audio itself does not fit. A queue
            // in the network drains once the waterfall stops feeding it, and
            // the round trip takes a while to show that; cutting the audio
            // meanwhile would cost the listener for nothing.
            if (backlog) pressure_seconds_ += seconds;
            else pressure_seconds_ = 0;
            if (pressure_seconds_ >= 0.3) {
                // Cut below what the audio actually uses, so the first cut
                // bites on a variable-rate stream, but never by more than
                // half in one step: a closed squelch measures a few kbit/s,
                // and a ceiling cut to that would hold speech at the floor
                // long after the burst that caused it has gone.
                const int current = std::min(ceiling_, requested_audio_bps);
                const int used = measured_audio_bps > 0 ? std::min(measured_audio_bps, current) : current;
                ceiling_ = std::max(8000, std::max(used, current / 2) * 4 / 5);
                waterfall_fraction_ = std::min(0.25, waterfall_fraction_);
                pressure_seconds_ = 0;
            }
        } else {
            pressure_seconds_ = 0;
            if (quiet) clear_seconds_ += seconds;
            else clear_seconds_ = 0;
            if (socket_clear) socket_clear_seconds_ += seconds;
            else socket_clear_seconds_ = 0;
            recover(requested_audio_bps);
        }
    }
    int audio_bitrate(int requested) const { return std::min(requested, ceiling_); }
    double control_bitrate() const { return control_bps_; }
    double waterfall_scale(int requested) const {
        return congested_ ? 0.0 : std::min(waterfall_fraction_, ceiling_ < requested ? 0.25 : 1.0);
    }

private:
    static constexpr double kReturnSeconds = 1.0;
    static constexpr double kTrustSeconds = 10.0;
    static constexpr double kStepSeconds = 3.0;
    static constexpr double kSlowFillSeconds = 1.0;
    static constexpr double kMinProbeSeconds = 3.0;
    static constexpr double kMaxProbeSeconds = 30.0;
    static constexpr int kQuietDelayMs = 50;
    static constexpr int kQueueDelayMs = 150;

    // The queue has just crossed the congestion threshold. One seen only in
    // the round trip is never a stall: the round trip a stall leaves behind
    // is not counted (net/queue_delay.h), and a queue in the network is the
    // link falling behind, however fast it filled.
    void on_queue(bool in_network) {
        const bool slow_fill = in_network || filling_seconds_ >= kSlowFillSeconds;
        // A probe stays open until the round trip has been down for a while
        // too, since the queue it builds in the network builds slowly. This
        // socket it fills at once or not at all, so one the socket has
        // carried for as long as a step takes to show held as far as the
        // socket goes: without this, a round trip that stayed a little long
        // kept a probe open for minutes, and the next stall was blamed on it.
        if (step_open_ && step_was_probe_ && !in_network && socket_clear_seconds_ >= kStepSeconds) {
            step_open_ = false;
            failed_returns_ = 0;
        }
        const bool step_was_open = step_open_;
        if (step_open_) {
            if (!step_was_probe_ && !slow_fill && failed_returns_ == 0) {
                // A return that meets a sudden queue is usually one more
                // stall of the same bad moment; a second one in a row means
                // the link really is slower now.
                failed_returns_ = 1;
            } else {
                // Our own step filled the queue before the new level had
                // held: the link carries the level before it and no more. A
                // quiet queue proves the reduced rate fits, not that the link
                // has become faster. Repeated failed probes need longer
                // intervals or a 48 kbit/s link stalls every few seconds.
                recovery_seconds_ = std::min(kMaxProbeSeconds, recovery_seconds_ * 2);
                held_ceiling_ = std::min(held_ceiling_, before_ceiling_);
                held_fraction_ = std::min(held_fraction_, before_fraction_);
                failed_returns_ = 0;
            }
            step_open_ = false;
        }
        waterfall_fraction_ = std::max(0.0625, waterfall_fraction_ * 0.5);
        // Nothing changed on this side and the queue took its time: the link
        // has become slower than what it carried, so returning there would
        // only fill the queue again. A queue in the network with nothing
        // changed is the waterfall's to answer, though, and leaves the level
        // the audio returns to alone: coming just after a stall, it would
        // otherwise keep the stall's cut. When one of the budget's own steps
        // fills it, above, the audio's level falls with the waterfall's: on a
        // link too slow for the whole audio the network fills before this
        // socket does, and a budget that never learned from it returned to
        // the whole audio after every cut.
        if (!step_was_open && slow_fill) {
            if (!in_network) held_ceiling_ = std::min(held_ceiling_, ceiling_);
            held_fraction_ = std::min(held_fraction_, waterfall_fraction_);
        }
    }

    void recover(int requested_audio_bps) {
        // A return goes to a level already carried and has held once this
        // socket has stayed clear; a probe goes above it, and a probe is what
        // builds a queue in the network, slowly, so it has held only once
        // the round trip has stayed down as well.
        const double held_for = step_was_probe_ ? clear_seconds_ : socket_clear_seconds_;
        if (step_open_ && held_for >= kStepSeconds) {
            // Any step that holds shows the link carrying what it is given,
            // so an earlier forgiven return is no longer part of a run.
            step_open_ = false;
            failed_returns_ = 0;
        }
        // Quiet time survives the steps taken during it, so a level earns
        // trust during a climb too; until the last probe has held, the level
        // before it is the one proven. A return goes to a level already
        // trusted, and the level before it is only the cut a stall left: a
        // listener tuning from CW to USB during one would otherwise have the
        // CW bitrate remembered as all the link can carry.
        if (quiet_seconds_ >= kTrustSeconds) {
            const bool probing = step_open_ && step_was_probe_;
            held_ceiling_ = probing ? before_ceiling_ : ceiling_;
            held_fraction_ = probing ? before_fraction_ : waterfall_fraction_;
        }
        const int held_audio = std::min(held_ceiling_, requested_audio_bps);
        const int ceiling_before = ceiling_;
        const double fraction_before = waterfall_fraction_;
        // The audio returns once this socket has been clear for a second. A
        // round trip a little long leaves the waterfall no room to grow into,
        // but says nothing against the audio, which comes first. (A queue in
        // the network is congestion, and nothing returns during that.)
        if (ceiling_ < held_audio && socket_clear_seconds_ >= kReturnSeconds) ceiling_ = held_audio;
        // The waterfall only comes back once the audio is whole again. On a
        // link too slow for the whole audio that leaves it at the floor after
        // a stall, which is the order the budget gives things up in, not a
        // loss: every bit it does not take goes to the audio.
        const bool audio_full = ceiling_ >= requested_audio_bps;
        if (audio_full && waterfall_fraction_ < held_fraction_ && clear_seconds_ >= kReturnSeconds) {
            waterfall_fraction_ = held_fraction_;
        }
        const bool returned = ceiling_ != ceiling_before || waterfall_fraction_ != fraction_before;
        if (!returned) {
            // Nothing is probed while a return is still due.
            if (ceiling_ < held_audio || (audio_full && waterfall_fraction_ < held_fraction_)) return;
            if (clear_seconds_ < recovery_seconds_) return;
            if (!audio_full) {
                // Multiplicative, so a deep cut is undone in a handful of
                // probes rather than minutes of small steps; the probe
                // interval, which doubles after each failure, is what keeps
                // a marginal link from oscillating.
                ceiling_ = std::min(requested_audio_bps, std::max(ceiling_ + 1000, ceiling_ * 5 / 4));
            } else {
                waterfall_fraction_ = std::min(1.0, waterfall_fraction_ + 0.125);
            }
            // At the top there is nothing to probe, and a queue that follows
            // is not the link refusing a step.
            if (ceiling_ == ceiling_before && waterfall_fraction_ == fraction_before) return;
        }
        before_ceiling_ = ceiling_before;
        before_fraction_ = fraction_before;
        step_open_ = true;
        step_was_probe_ = !returned;
        clear_seconds_ = 0;
        socket_clear_seconds_ = 0;
    }

    int ceiling_ = 128000;
    double control_bits_ = 0;
    double control_seconds_ = 0;
    double control_bps_ = 8192;
    std::array<double, 3> control_windows_{8192, 8192, 8192};
    size_t control_window_ = 0;
    double pressure_seconds_ = 0;
    double expiry_pressure_seconds_ = 0;
    // Time with nothing queued since the last step or queue, and with only
    // this socket clear; time with nothing queued since the last queue alone;
    // and the time this socket's queue has been filling.
    double clear_seconds_ = 0;
    double socket_clear_seconds_ = 0;
    double quiet_seconds_ = 0;
    double filling_seconds_ = 0;
    double recovery_seconds_ = kMinProbeSeconds;
    bool congested_ = false;
    double waterfall_fraction_ = 1;
    // The start is not a step, and the starting level counts as carried
    // until the link says otherwise: a stall in the first seconds of a
    // session says nothing about the link, and with nothing to return to it
    // took forty seconds of probes to climb back. A link that cannot carry
    // the start fills the queue slowly, or fails two returns in a row, and
    // either lowers the level to return to.
    bool step_open_ = false;
    bool step_was_probe_ = false;
    int failed_returns_ = 0;
    // The level before the last step up, and the one to return to after a
    // stall.
    int before_ceiling_ = 8000;
    double before_fraction_ = 0.0625;
    int held_ceiling_ = 128000;
    double held_fraction_ = 1;
};

}  // namespace fernsdr
