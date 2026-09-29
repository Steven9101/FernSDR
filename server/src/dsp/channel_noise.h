// The band noise around one listener's channel, read off the band's shared
// transform, for a gain control that is referenced to it.
#pragma once
#include <cstddef>
#include <vector>

#include "channelizer.h"

namespace fernsdr {

/**
 * Mean noise power of one bin among `count` bins starting at bin `first`
 * (negative bins count down from the top of the transform, as they do for a
 * complex input), worked out from the quietest of them only.
 *
 * Complex Gaussian noise gives every bin an exponentially distributed power,
 * and the quietest bins of a window are noise even on a busy band. So the
 * bins below 1.5 times the 10th percentile are averaged, and that truncated
 * mean is scaled up to the whole distribution's mean: for an exponential with
 * mean m, the mean of the part below z * m is m * (1 - z e^-z / (1 - e^-z)),
 * and 1.5 times the 10th percentile is z = 1.5 * -ln(0.9). A mean or a
 * median of the window would climb with the first station in it; this does
 * not, because stations do not reach the quietest bins.
 *
 * They do push the 10th percentile up the noise's own distribution, though:
 * with a share p of the window occupied, the estimate reads about 1 / (1 - p)
 * high, 1 dB with a fifth of it in use and 3 dB with half. That is the safe
 * side for the gain control built on it, which then holds the noise further
 * under its target, never closer.
 *
 * Returns 0 when there is nothing to go on (no bins, or all of them silent).
 */
double noise_bin_power(const float* re, const float* im, size_t fft_size, long first, size_t count,
                       std::vector<float>& scratch);

/**
 * The noise a listener's channel carries, kept current a few times a second.
 *
 * Read from the window of kWindowBins around the channel rather than from the
 * passband itself: a station that talks for twenty seconds with half-second
 * breaths leaves its own passband no quiet moment to measure, while the bins
 * beside it are noise all along. Around the channel rather than across the
 * whole band, because band noise is not flat; on a 0 to 30 MHz front end the
 * low bands sit tens of dB above the high ones.
 *
 * One look takes 1 us when the window's bins are in cache and 7 us when they
 * have to come from memory, as they do on a 64 Msps band whose spectrum is
 * 8 MB. At two and a half looks a second that is at most 17 us a second, and
 * with the AGC's own 23 to 25 us a listener costs at least a fifth less than
 * with the peak-following AGC it replaced (52 us a second of 12 kHz audio).
 * Only paid while the AGC is on.
 */
class ChannelNoise {
public:
    static constexpr size_t kWindowBins = 1024;
    static constexpr double kIntervalSeconds = 0.4;
    /** Roughly how long the estimate takes to follow a real change. */
    static constexpr double kSettleSeconds = 1.0;

    void reset();

    /**
     * Takes a look at the block's spectrum when one is due. `center_hz` is the
     * middle of the channel's passband relative to the band's spectrum origin,
     * as Channel::set_passband counts it. `real_input` keeps the window in the
     * lower half of the transform, the only half a real input fills.
     */
    void update(const ChannelBlock& block, double center_hz, bool real_input);

    /**
     * Noise power per output sample of a channel passing `bandwidth_hz`, in
     * the units Channel::pull produces, or 0 before the first look.
     *
     * The channelizer's sine window gives white noise of variance s2 a mean
     * bin power of s2 * K / 2, and a unity-gain channel passes s2 * B / rate
     * of it.
     */
    double channel_power(double bandwidth_hz) const;

    /** Mean noise power of one bin, or 0 before the first look. */
    double bin_power() const { return bin_power_; }

private:
    double bin_power_ = 0.0;
    double bin_hz_ = 0.0;
    size_t fft_size_ = 0;
    double look_center_hz_ = 0.0;
    double since_look_s_ = 0.0;
    std::vector<float> scratch_;
};

}  // namespace fernsdr
