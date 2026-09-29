// Automatic squelch: does the passband contain a signal, or only noise?
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace fernsdr {

/**
 * Squelch that needs no threshold, after PA3FWM's algorithm for the WebSDR.
 * https://www.pa3fwm.nl/technotes/tn16f.html
 *
 * A level-based squelch asks "is this loud?", which is the wrong question. It
 * has to be set by hand, it has to be re-set every time the band noise
 * changes, and it cannot tell a weak signal on a quiet band from noise on a
 * noisy one. That is why every receiver with one has a knob for it, and why
 * the knob is always slightly wrong.
 *
 * This asks "is this shaped like noise?" instead, which has an answer that
 * does not depend on how loud anything is.
 *
 * Take the FFT bins across the passband - and only the passband, because a
 * signal two kilohertz away is not what the listener is trying to hear - and
 * compute the variance of the bin powers divided by the square of their mean.
 * For noise that ratio is 1, and it is 1 whatever the gain, whatever the
 * antenna, whatever the band conditions: the power of a bin of Gaussian noise
 * is exponentially distributed, and an exponential distribution has a standard
 * deviation equal to its mean. A signal puts its energy in some bins and not
 * others, so the ratio climbs. The measurement is therefore self-calibrating,
 * which is the whole point.
 *
 * Scaled by the square root of the number of bins, because the ratio's own
 * scatter narrows as bins are added and a fixed threshold would otherwise mean
 * something different at every bandwidth.
 */
class AutoSquelch {
public:
    /** Open at once: pure noise reaches this about once in a hundred thousand. */
    static constexpr double kOpenNow = 18.0;
    /** Open on repetition. Three in a row is as unlikely as one at kOpenNow. */
    static constexpr double kOpenSoon = 5.0;
    static constexpr int kRepeatsNeeded = 3;
    /**
     * Rows of noise before it closes again.
     *
     * Speech has gaps, and CW is mostly gaps. A squelch that shuts between
     * words chops the transmission into pieces and is worse than none.
     */
    static constexpr int kHangRows = 25;

    /**
     * Folds one row of passband bins into the decision.
     *
     * `bins` are levels in dB, which is what the spectrum already produces.
     * Fewer than four bins is not enough to say anything about a distribution,
     * so the squelch holds open rather than guess.
     */
    void update(const float* bins, size_t count) {
        if (count < 4) {
            open_ = true;
            return;
        }
        // The statistic is about power, and these bins arrive as levels.
        double sum = 0.0;
        double sum_squares = 0.0;
        for (size_t i = 0; i < count; i++) {
            const double power = std::pow(10.0, static_cast<double>(bins[i]) / 10.0);
            sum += power;
            sum_squares += power * power;
        }
        decide(sum, sum_squares, count);
    }

    /**
     * The same decision taken straight from the transform.
     *
     * This is how the receiver actually feeds it: the channelizer already
     * holds the passband as real and imaginary parts, so the power is one
     * multiply-add per bin and no level is computed and un-computed on the way.
     */
    void update_from_bins(const float* re, const float* im, size_t count) {
        if (count < 4) {
            open_ = true;
            return;
        }
        double sum = 0.0;
        double sum_squares = 0.0;
        for (size_t i = 0; i < count; i++) {
            const double power =
                static_cast<double>(re[i]) * re[i] + static_cast<double>(im[i]) * im[i];
            sum += power;
            sum_squares += power * power;
        }
        decide(sum, sum_squares, count);
    }

    bool open() const { return open_; }
    /** The scaled statistic, for a readout and for tuning the thresholds. */
    double statistic() const { return statistic_; }

    void reset() {
        open_ = true;
        consecutive_ = 0;
        hold_ = 0;
        statistic_ = 0.0;
    }

private:
    void decide(double sum, double sum_squares, size_t count) {
        const double mean = sum / static_cast<double>(count);
        if (!(mean > 0.0)) {
            open_ = true;
            return;
        }
        const double variance = sum_squares / static_cast<double>(count) - mean * mean;
        const double relative = variance / (mean * mean);
        statistic_ = (relative - 1.0) * std::sqrt(static_cast<double>(count));

        if (statistic_ >= kOpenNow) {
            consecutive_ = 0;
            hold_ = kHangRows;
            open_ = true;
            return;
        }
        if (statistic_ >= kOpenSoon) {
            // Weak, but weak and repeated is not what noise does.
            if (++consecutive_ >= kRepeatsNeeded) {
                hold_ = kHangRows;
                open_ = true;
            }
            return;
        }

        consecutive_ = 0;
        if (hold_ > 0) {
            hold_--;
            return;
        }
        open_ = false;
    }

    // Starts open: a squelch that mutes a receiver before it has looked at
    // anything is indistinguishable from a receiver that is broken.
    bool open_ = true;
    int consecutive_ = 0;
    int hold_ = 0;
    double statistic_ = 0.0;
};

}  // namespace fernsdr
