// Corrections for the flaws of the front end itself, applied to a band's raw
// samples before the noise blanker, the spectrum and every listener see them.
//
// Three, in the order they have to run:
//
//   - swapping I and Q, for an input that delivers the spectrum mirrored:
//     signals move the wrong way when the operator tunes;
//   - removing the DC offset a zero-IF tuner adds, the spike in the middle of
//     every RTL-SDR waterfall. Before the balance, because a DC offset in one
//     arm looks to the balance like a gain error;
//   - balancing I against Q, which in hardware never have quite the same gain
//     or quite 90 degrees between them. What that costs is mirror images: a
//     strong signal at +f shows a weaker copy of itself at -f.
//
// Both estimates are blind, taken from the band itself. The DC offset is the
// long-run mean of each arm. The balance rests on a band being made of many
// independent signals and noise, whose I and Q have equal power and are
// uncorrelated; whatever the front end did to break that is undone. Both
// follow slowly, over a fraction of a second and a couple of seconds, so they
// track a gain change without chasing the signals.
#pragma once
#include <atomic>
#include <cstddef>

#include "channelizer.h"

namespace fernsdr {

class InputConditioner {
public:
    void configure(double sample_rate);

    // Written by the admin panel's thread and read by the band's thread every
    // block; a change takes effect on the next one.
    void set_swap(bool on) { swap_.store(on, std::memory_order_relaxed); }
    void set_dc_remove(bool on) { dc_remove_.store(on, std::memory_order_relaxed); }
    void set_balance(bool on) { balance_.store(on, std::memory_order_relaxed); }
    bool swap() const { return swap_.load(std::memory_order_relaxed); }
    bool dc_remove() const { return dc_remove_.load(std::memory_order_relaxed); }
    bool balance() const { return balance_.load(std::memory_order_relaxed); }

    void process(cfloat* samples, size_t count);
    // A real input has no Q to swap or balance; only its DC offset is removed.
    void process_real(float* samples, size_t count);

    // What was found, for the admin panel. The offset in dB below full scale;
    // the imbalance as the gain and phase error of Q against I, and the image
    // rejection those amount to without the correction.
    float dc_offset_dbfs() const { return dc_offset_dbfs_.load(std::memory_order_relaxed); }
    float gain_error_db() const { return gain_error_db_.load(std::memory_order_relaxed); }
    float phase_error_degrees() const { return phase_error_degrees_.load(std::memory_order_relaxed); }
    float image_rejection_db() const { return image_rejection_db_.load(std::memory_order_relaxed); }

    // Forgets both estimates, so a correction switched back on starts again
    // from what the band looks like now.
    void reset();

private:
    void remove_dc(cfloat* samples, size_t count);
    void remove_dc_real(float* samples, size_t count);
    void balance_block(cfloat* samples, size_t count);

    std::atomic<bool> swap_{false};
    std::atomic<bool> dc_remove_{false};
    std::atomic<bool> balance_{false};
    // What the last block was processed with. The estimates describe the arms
    // as they were then, and a swap changes which arm is which.
    bool swapped_ = false;

    double sample_rate_ = 0.0;
    // DC: the offset being removed at the end of the last block, per arm, and
    // how many samples it has seen; see weight() in the implementation.
    bool dc_primed_ = false;
    double dc_seen_ = 0.0;
    double dc_i_ = 0.0;
    double dc_q_ = 0.0;
    // Balance: the long-run powers of I and Q and their correlation, and the
    // correction they give, Q' = q_gain * Q + i_to_q * I.
    bool balance_primed_ = false;
    double balance_seen_ = 0.0;
    double ii_ = 0.0;
    double qq_ = 0.0;
    double iq_ = 0.0;
    float q_gain_ = 1.0f;
    float i_to_q_ = 0.0f;

    std::atomic<float> dc_offset_dbfs_{-160.0f};
    std::atomic<float> gain_error_db_{0.0f};
    std::atomic<float> phase_error_degrees_{0.0f};
    std::atomic<float> image_rejection_db_{0.0f};
};

}  // namespace fernsdr
