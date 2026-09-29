#include "../src/dsp/fir_decimator.h"
#include "test_util.h"

#include <cmath>
#include <complex>
#include <vector>

using fernsdr::BandDecimator;
using fernsdr::FirDecimator;

namespace {

// Level of the output at `hz`, in dB relative to a full-scale sine, measured
// over the second half so the filter has settled.
double level_db(double input_rate, size_t factor, double hz, size_t block = 0, double deemphasis_us = 0.0) {
    FirDecimator decimator;
    decimator.configure(input_rate, factor, 15000.0, 18500.0, 70.0, deemphasis_us);
    const size_t count = 256000;
    std::vector<float> in(count);
    for (size_t i = 0; i < count; i++) in[i] = static_cast<float>(std::sin(2.0 * M_PI * hz * i / input_rate));
    std::vector<float> out(count);
    size_t produced = 0;
    if (block == 0) {
        produced = decimator.process(in.data(), count, out.data());
    } else {
        for (size_t at = 0; at < count; at += block) {
            produced += decimator.process(in.data() + at, std::min(block, count - at), out.data() + produced);
        }
    }
    const double rate = input_rate / factor;
    double i_sum = 0.0, q_sum = 0.0;
    const size_t first = produced / 2;
    for (size_t n = first; n < produced; n++) {
        // The output frequency after folding.
        const double phase = 2.0 * M_PI * hz * static_cast<double>(n) / rate;
        i_sum += out[n] * std::cos(phase);
        q_sum += out[n] * std::sin(phase);
    }
    double power = 0.0;
    for (size_t n = first; n < produced; n++) power += static_cast<double>(out[n]) * out[n];
    power /= static_cast<double>(produced - first);
    return 10.0 * std::log10(std::max(power * 2.0, 1e-20));
}

}  // namespace

TEST_CASE(fir_decimator_keeps_broadcast_audio_and_removes_the_pilot) {
    // 256 kHz channel to 36.6 kHz audio, as for a 2.048 Msps band.
    CHECK_NEAR(level_db(256000.0, 7, 1000.0), 0.0, 0.05);
    CHECK_NEAR(level_db(256000.0, 7, 10000.0), 0.0, 0.05);
    CHECK_NEAR(level_db(256000.0, 7, 14500.0), 0.0, 0.1);
    // The stereo pilot, the stereo subcarrier and RDS, whatever they fold to.
    CHECK(level_db(256000.0, 7, 19000.0) < -68.0);
    CHECK(level_db(256000.0, 7, 38000.0) < -68.0);
    CHECK(level_db(256000.0, 7, 57000.0) < -68.0);
    // What would fold into the kept audio from above the output's rate.
    CHECK(level_db(256000.0, 7, 36571.0 - 5000.0) < -68.0);
}

TEST_CASE(fir_decimator_joins_blocks_without_a_seam) {
    const double whole = level_db(256000.0, 7, 3000.0);
    CHECK_NEAR(level_db(256000.0, 7, 3000.0, 1024), whole, 1e-6);
    CHECK_NEAR(level_db(256000.0, 7, 3000.0, 333), whole, 1e-6);
    FirDecimator decimator;
    decimator.configure(256000.0, 7, 15000.0, 18500.0, 70.0);
    std::vector<float> in(7000, 1.0f), a(7000), b(7000);
    size_t produced = 0;
    for (size_t at = 0; at < in.size(); at += 100) produced += decimator.process(in.data() + at, 100, a.data() + produced);
    CHECK_EQ(produced, 1000u);
    // In place.
    decimator.reset();
    std::vector<float> same(in);
    CHECK_EQ(decimator.process(same.data(), same.size(), same.data()), 1000u);
    decimator.reset();
    decimator.process(in.data(), in.size(), b.data());
    for (size_t i = 0; i < 1000; i++) CHECK_NEAR(same[i], b[i], 1e-7);
    CHECK(decimator.taps() < 600u);
}

TEST_CASE(fir_decimator_folds_in_the_de_emphasis_of_an_fm_receiver) {
    // The analogue one-pole response, relative to 1 kHz, for both standards.
    for (const double tau : {50.0, 75.0}) {
        const auto analogue = [tau](double hz) {
            const double w = 2.0 * M_PI * hz * tau * 1e-6;
            return -10.0 * std::log10(1.0 + w * w);
        };
        const double reference = level_db(256000.0, 7, 1000.0, 0, tau);
        for (const double hz : {3000.0, 6000.0, 12000.0}) {
            CHECK_NEAR(level_db(256000.0, 7, hz, 0, tau) - reference, analogue(hz) - analogue(1000.0), 0.15);
        }
        CHECK(level_db(256000.0, 7, 19000.0, 0, tau) < -68.0);
        CHECK_NEAR(level_db(256000.0, 7, 3000.0, 777, tau), level_db(256000.0, 7, 3000.0, 0, tau), 1e-6);
    }
}

namespace {

// A tone `offset` from 57 kHz through a BandDecimator for RDS, 256 kHz to
// 16 kHz: its level, and the frequency it came out at, from the rotation
// between outputs.
struct Shifted {
    double level_db;
    double hz;
};

Shifted band_tone(double offset, size_t block) {
    BandDecimator band;
    band.configure(256000.0, 16, 57000.0, 2600.0, 16000.0 - 2600.0, 60.0);
    const size_t count = 256000;
    std::vector<float> in(count);
    for (size_t i = 0; i < count; i++) in[i] = static_cast<float>(std::cos(2.0 * M_PI * (57000.0 + offset) * i / 256000.0));
    std::vector<std::complex<float>> out(count / 16 + 16);
    size_t produced = 0;
    for (size_t at = 0; at < count; at += block) {
        produced += band.process(in.data() + at, std::min(block, count - at), out.data() + produced);
    }
    double power = 0.0, turn = 0.0;
    const size_t first = produced / 2;
    for (size_t n = first; n < produced; n++) {
        power += std::norm(out[n]);
        if (n > first) turn += std::arg(out[n] * std::conj(out[n - 1]));
    }
    power /= static_cast<double>(produced - first);
    const double hz = turn / static_cast<double>(produced - first - 1) * band.output_rate() / (2.0 * M_PI);
    // A real cosine of amplitude one is two halves; the kept one is 1/2.
    return {10.0 * std::log10(std::max(power * 4.0, 1e-20)), hz};
}

}  // namespace

TEST_CASE(band_decimator_brings_a_subcarrier_to_baseband) {
    const Shifted centre = band_tone(0.0, 4096);
    CHECK_NEAR(centre.level_db, 0.0, 0.05);
    const Shifted above = band_tone(1000.0, 4096);
    CHECK_NEAR(above.level_db, 0.0, 0.05);
    CHECK_NEAR(above.hz, 1000.0, 1.0);
    const Shifted below = band_tone(-2000.0, 4096);
    CHECK_NEAR(below.hz, -2000.0, 1.0);
    // What would fold onto the kept 2.6 kHz: 60 dB down.
    CHECK(band_tone(16000.0 - 1500.0, 4096).level_db < -59.0);
    CHECK(band_tone(-(16000.0 - 1500.0), 4096).level_db < -59.0);
    // The stereo pilot and the programme, far below: gone.
    CHECK(band_tone(19000.0 - 57000.0, 4096).level_db < -59.0);
    // Blocks of any size join without a seam, the rotation included.
    const Shifted odd = band_tone(1000.0, 999);
    CHECK_NEAR(odd.level_db, above.level_db, 1e-3);
    CHECK_NEAR(odd.hz, above.hz, 1e-3);
}
