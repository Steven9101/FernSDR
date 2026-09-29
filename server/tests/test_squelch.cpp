#include "../src/dsp/squelch.h"
#include "test_util.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::AutoSquelch;

namespace {

/**
 * A row of FFT bins, in dB, of Gaussian noise at `level` dB with an optional
 * tone. Noise bins are built from a complex Gaussian and squared, so their
 * powers are exponentially distributed exactly as they are on the air - the
 * property the whole algorithm rests on.
 */
std::vector<float> noise_row(size_t bins, double level_db, std::mt19937& rng,
                             double tone_db = -1e9, size_t tone_bin = 0, size_t tone_width = 1) {
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double scale = std::pow(10.0, level_db / 10.0);
    std::vector<float> row(bins);
    for (size_t i = 0; i < bins; i++) {
        const double re = gauss(rng), im = gauss(rng);
        const double power = scale * (re * re + im * im) / 2.0;
        row[i] = static_cast<float>(10.0 * std::log10(power + 1e-30));
    }
    if (tone_db > -1e8) {
        for (size_t i = tone_bin; i < std::min(bins, tone_bin + tone_width); i++) {
            row[i] = static_cast<float>(tone_db);
        }
    }
    return row;
}

/** Runs `rows` of the same kind through and reports whether it ends up open. */
bool settle(AutoSquelch& squelch, size_t bins, double level, std::mt19937& rng, int rows,
            double tone = -1e9, size_t tone_width = 1) {
    for (int i = 0; i < rows; i++) {
        const auto row = noise_row(bins, level, rng, tone, bins / 2, tone_width);
        squelch.update(row.data(), row.size());
    }
    return squelch.open();
}

}  // namespace

TEST_CASE(squelch_closes_on_noise) {
    std::mt19937 rng(1);
    AutoSquelch squelch;
    CHECK(!settle(squelch, 64, -95.0, rng, 60));
}

TEST_CASE(squelch_opens_on_a_signal) {
    std::mt19937 rng(2);
    AutoSquelch squelch;
    settle(squelch, 64, -95.0, rng, 60);
    CHECK(!squelch.open());
    // A carrier 30 dB out of the noise, in four bins of sixty-four.
    CHECK(settle(squelch, 64, -95.0, rng, 5, -65.0, 4));
}

TEST_CASE(squelch_does_not_care_how_loud_the_noise_is) {
    // The point of the algorithm: the same decision at any gain. A level
    // threshold set on a quiet band mutes a noisy one entirely.
    std::mt19937 rng(3);
    for (double level : {-120.0, -95.0, -60.0, -20.0}) {
        AutoSquelch quiet;
        CHECK(!settle(quiet, 64, level, rng, 60));
        AutoSquelch loud;
        CHECK(settle(loud, 64, level, rng, 60, level + 30.0, 4));
    }
}

TEST_CASE(squelch_opens_on_a_weak_signal_that_repeats) {
    std::mt19937 rng(4);
    AutoSquelch squelch;
    settle(squelch, 128, -95.0, rng, 60);
    CHECK(!squelch.open());
    // Too weak for the immediate threshold, but noise does not do the same
    // thing three rows running.
    const bool opened = settle(squelch, 128, -95.0, rng, 6, -83.0, 3);
    CHECK(opened);
}

TEST_CASE(squelch_holds_through_the_gaps_in_speech) {
    std::mt19937 rng(5);
    AutoSquelch squelch;
    settle(squelch, 64, -95.0, rng, 30, -60.0, 6);
    CHECK(squelch.open());
    // Half a second of nothing between two words, at 25 rows a second.
    CHECK(settle(squelch, 64, -95.0, rng, 12));
    // But it does close on a transmission that has actually ended.
    CHECK(!settle(squelch, 64, -95.0, rng, 40));
}

TEST_CASE(squelch_threshold_means_the_same_at_every_bandwidth) {
    // The scaling by sqrt(bins) exists for this: a wide filter and a narrow
    // one must not need different thresholds.
    std::mt19937 rng(6);
    for (size_t bins : {16u, 64u, 256u, 1024u}) {
        AutoSquelch squelch;
        CHECK(!settle(squelch, bins, -95.0, rng, 80));
    }
}

TEST_CASE(squelch_starts_open) {
    // A receiver that is silent before it has looked at anything is
    // indistinguishable from a receiver that is broken.
    AutoSquelch squelch;
    CHECK(squelch.open());
}

TEST_CASE(squelch_holds_open_when_there_is_too_little_to_judge) {
    AutoSquelch squelch;
    std::mt19937 rng(7);
    settle(squelch, 64, -95.0, rng, 60);
    CHECK(!squelch.open());
    // A 100 Hz CW filter over 62.5 Hz bins is two bins, which says nothing
    // about a distribution. Guessing there would mute the narrowest filters,
    // which are exactly the ones used on the weakest signals.
    const auto row = noise_row(2, -95.0, rng);
    squelch.update(row.data(), row.size());
    CHECK(squelch.open());
}
