#include "../src/dsp/noise_floor.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

using fernsdr::NoiseFloor;

namespace {

/** A band of noise at `level` dBFS, with `signals` strong carriers in it. */
std::vector<float> make_line(size_t bins, float level, size_t signals, float signal_level,
                             std::mt19937& rng) {
    std::normal_distribution<float> spread(level, 2.0f);
    std::vector<float> line(bins);
    for (size_t i = 0; i < bins; i++) line[i] = spread(rng);
    for (size_t s = 0; s < signals; s++) {
        const size_t start = (bins / (signals + 1)) * (s + 1);
        // A real signal is a few bins wide, not one.
        for (size_t i = start; i < std::min(bins, start + bins / 200); i++) line[i] = signal_level;
    }
    return line;
}

}  // namespace

TEST_CASE(noise_floor_finds_an_empty_band) {
    std::mt19937 rng(1);
    NoiseFloor floor;
    for (int i = 0; i < 200; i++) {
        const auto line = make_line(4096, -95.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
    }
    // The 25th percentile of a normal spread sits below its mean by about
    // 0.67 standard deviations, which is 1.3dB here.
    CHECK(std::fabs(floor.dbfs() - -96.3f) < 1.0f);
}

TEST_CASE(noise_floor_ignores_the_signals_on_the_band) {
    std::mt19937 rng(2);
    NoiseFloor empty, busy;
    for (int i = 0; i < 200; i++) {
        const auto quiet = make_line(4096, -95.0f, 0, 0.0f, rng);
        empty.update(quiet.data(), quiet.size(), 0.04);
        // A quarter of the band occupied by signals 50dB out of the noise:
        // a mean would be dragged up by tens of dB.
        const auto loud = make_line(4096, -95.0f, 50, -45.0f, rng);
        busy.update(loud.data(), loud.size(), 0.04);
    }
    CHECK(std::fabs(busy.dbfs() - empty.dbfs()) < 1.0f);
}

TEST_CASE(noise_floor_follows_a_real_change) {
    std::mt19937 rng(3);
    NoiseFloor floor;
    for (int i = 0; i < 400; i++) {
        const auto line = make_line(2048, -100.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
    }
    const float before = floor.dbfs();
    CHECK(std::fabs(before - -101.3f) < 1.0f);

    // The preamp comes on: 20dB more noise. Five seconds of lines at 25/s.
    for (int i = 0; i < 125; i++) {
        const auto line = make_line(2048, -80.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
    }
    // One time constant is 63% of the way; the settle target is five seconds.
    CHECK(floor.dbfs() > before + 12.0f);

    for (int i = 0; i < 500; i++) {
        const auto line = make_line(2048, -80.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
    }
    CHECK(std::fabs(floor.dbfs() - -81.3f) < 1.0f);
}

TEST_CASE(noise_floor_does_not_flicker) {
    std::mt19937 rng(4);
    NoiseFloor floor;
    for (int i = 0; i < 400; i++) {
        const auto line = make_line(4096, -95.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
    }
    // Settled. Over the next second the reading must be steady enough to put
    // on screen and to hang a squelch threshold on.
    float low = 1e9f, high = -1e9f;
    for (int i = 0; i < 25; i++) {
        const auto line = make_line(4096, -95.0f, 0, 0.0f, rng);
        floor.update(line.data(), line.size(), 0.04);
        low = std::min(low, floor.dbfs());
        high = std::max(high, floor.dbfs());
    }
    CHECK(high - low < 0.5f);
}

TEST_CASE(noise_floor_settles_at_the_same_rate_whatever_the_line_rate) {
    std::mt19937 rng(5);
    NoiseFloor fast, slow;
    // Two seconds of lines: one band at 25/s, one at 5/s.
    for (int i = 0; i < 50; i++) {
        const auto line = make_line(2048, -70.0f, 0, 0.0f, rng);
        fast.update(line.data(), line.size(), 0.04);
    }
    for (int i = 0; i < 10; i++) {
        const auto line = make_line(2048, -70.0f, 0, 0.0f, rng);
        slow.update(line.data(), line.size(), 0.20);
    }
    CHECK(std::fabs(fast.dbfs() - slow.dbfs()) < 1.0f);
}

TEST_CASE(noise_floor_ignores_padding_bins) {
    std::mt19937 rng(6);
    NoiseFloor floor;
    for (int i = 0; i < 200; i++) {
        auto line = make_line(2048, -95.0f, 0, 0.0f, rng);
        // A notched DC spike and a gap at the edges, written as the padding
        // value. Counted as noise, these drag the floor to the padding.
        for (size_t j = 0; j < 200; j++) line[j] = -160.0f;
        line[1024] = -160.0f;
        floor.update(line.data(), line.size(), 0.04);
    }
    CHECK(std::fabs(floor.dbfs() - -96.3f) < 1.5f);
}

TEST_CASE(noise_floor_reports_nothing_before_the_first_line) {
    NoiseFloor floor;
    CHECK(!floor.ready());
    CHECK(floor.dbfs() < -150.0f);
}
