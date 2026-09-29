// Noise floor estimation from a spectrum line.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fernsdr {

/**
 * The noise floor of a band, in dBFS.
 *
 * What an operator means by "the noise floor" is the level between the
 * signals, and the useful thing about it is that it is the reference every
 * other number wants: a signal report is only meaningful against it, a squelch
 * threshold is only sensible relative to it, and an antenna is doing its job
 * when the band noise sits above the receiver's own.
 *
 * Estimated as a low percentile of the bins rather than a mean, because a mean
 * is dragged upwards by exactly the signals the floor is supposed to exclude.
 * A busy 40m evening can have a quarter of the band occupied; the twenty-fifth
 * percentile still lands in the noise, while the median is already climbing.
 *
 * Then smoothed hard. The instantaneous percentile of one FFT hops around by
 * several dB, which is real - it is the noise - but a readout that flickers by
 * 3 dB is unreadable and a squelch that follows it chatters.
 */
class NoiseFloor {
public:
    /** Fraction of bins taken to be noise. */
    static constexpr float kPercentile = 0.25f;
    /** Roughly how long the estimate takes to follow a real change, seconds. */
    static constexpr double kSettleSeconds = 5.0;

    /**
     * Folds one spectrum line into the estimate.
     *
     * `line_interval_s` is how often lines arrive, which sets the smoothing:
     * the same wall-clock response whatever rate the band runs at.
     */
    void update(const float* bins, size_t count, double line_interval_s) {
        const float instant = percentile(bins, count);
        if (!std::isfinite(instant)) return;

        if (!primed_) {
            floor_dbfs_ = instant;
            primed_ = true;
            return;
        }
        // One pole, with the coefficient derived from the interval so that a
        // band producing 25 lines a second and one producing 5 settle alike.
        const double alpha =
            line_interval_s <= 0.0 ? 1.0 : 1.0 - std::exp(-line_interval_s / kSettleSeconds);
        floor_dbfs_ += static_cast<float>(alpha) * (instant - floor_dbfs_);
    }

    /** The estimate, or -160 before the first line. */
    float dbfs() const { return primed_ ? floor_dbfs_ : -160.0f; }
    bool ready() const { return primed_; }

    void reset() {
        primed_ = false;
        floor_dbfs_ = -160.0f;
    }

    /**
     * The percentile of one line, without smoothing.
     *
     * Sampled rather than sorted whole: a 65 536-bin line arrives 25 times a
     * second and the answer does not improve past a few thousand samples, so
     * every eighth bin is taken. `nth_element` then partitions in linear time
     * instead of sorting.
     */
    float percentile(const float* bins, size_t count) const {
        if (count == 0) return -160.0f;
        const size_t stride = count > 8192 ? count / 4096 : 1;

        scratch_.clear();
        scratch_.reserve(count / stride + 1);
        for (size_t i = 0; i < count; i += stride) {
            // A dead bin - a DC spike notched out, or a gap between two bands -
            // is not noise, and letting it into the sample drags the floor down
            // to whatever the padding value is.
            if (bins[i] > -200.0f) scratch_.push_back(bins[i]);
        }
        if (scratch_.empty()) return -160.0f;

        const size_t index =
            std::min(scratch_.size() - 1, static_cast<size_t>(scratch_.size() * kPercentile));
        std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<long>(index),
                         scratch_.end());
        return scratch_[index];
    }

private:
    bool primed_ = false;
    float floor_dbfs_ = -160.0f;
    mutable std::vector<float> scratch_;
};

}  // namespace fernsdr
