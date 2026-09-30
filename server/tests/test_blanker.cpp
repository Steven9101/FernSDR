#include "../src/dsp/agc.h"
#include "test_util.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::cfloat;
using fernsdr::NoiseBlanker;

namespace {

constexpr double kRate = 384000.0;
constexpr double kTwoPi = 6.283185307179586;

/**
 * A band holding one carrier, buried in noise, with ignition-style impulses
 * on top: short, periodic, and tens of times the noise.
 */
std::vector<cfloat> make_band(size_t count, double tone_hz, float tone, float noise_level,
                              float impulse, size_t impulse_period, size_t impulse_width,
                              std::mt19937& rng) {
    std::normal_distribution<float> noise(0.0f, noise_level);
    std::vector<cfloat> band(count);
    for (size_t i = 0; i < count; i++) {
        const double phase = kTwoPi * tone_hz * static_cast<double>(i) / kRate;
        band[i] = cfloat(static_cast<float>(tone * std::cos(phase)) + noise(rng),
                         static_cast<float>(tone * std::sin(phase)) + noise(rng));
    }
    if (impulse_period > 0) {
        for (size_t i = impulse_period; i < count; i += impulse_period) {
            for (size_t j = i; j < std::min(count, i + impulse_width); j++) {
                band[j] += cfloat(impulse, impulse);
            }
        }
    }
    return band;
}

/**
 * How much of the carrier survives, against everything else in the band, at
 * the carrier's own frequency. This is the number that matters: a blanker
 * that removes the impulse but mangles the carrier has not helped.
 */
double carrier_to_rest_db(const std::vector<cfloat>& band, double tone_hz) {
    double re = 0.0, im = 0.0, total = 0.0;
    for (size_t i = 0; i < band.size(); i++) {
        const double phase = -kTwoPi * tone_hz * static_cast<double>(i) / kRate;
        re += band[i].real() * std::cos(phase) - band[i].imag() * std::sin(phase);
        im += band[i].real() * std::sin(phase) + band[i].imag() * std::cos(phase);
        total += std::norm(band[i]);
    }
    const double n = static_cast<double>(band.size());
    const double carrier = (re * re + im * im) / (n * n);
    const double rest = std::max(1e-30, total / n - carrier);
    return 10.0 * std::log10(carrier / rest);
}

}  // namespace

TEST_CASE(blanker_lifts_a_carrier_out_of_impulse_noise) {
    std::mt19937 rng(1);
    const double tone = 40000.0;
    auto band = make_band(200000, tone, 0.02f, 0.01f, 3.0f, 6528, 6, rng);
    const double before = carrier_to_rest_db(band, tone);

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.7f);
    blanker.process(band.data(), band.size());
    const double after = carrier_to_rest_db(band, tone);

    // The impulses carry far more energy than the carrier, so removing them
    // is worth tens of dB. This is the whole reason a blanker exists.
    // Measured: -15.6 dB to +3.0, so 18.6 dB, and the same at every strength
    // because these impulses are far enough above the noise that the loosest
    // setting still catches them.
    CHECK(after > before + 15.0);
}

TEST_CASE(blanker_leaves_a_clean_band_alone) {
    std::mt19937 rng(2);
    const double tone = 40000.0;
    auto clean = make_band(200000, tone, 0.02f, 0.01f, 0.0f, 0, 0, rng);
    const double before = carrier_to_rest_db(clean, tone);

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.7f);
    blanker.process(clean.data(), clean.size());
    const double after = carrier_to_rest_db(clean, tone);

    // Nothing to remove, so nothing should be removed. A blanker that eats a
    // dB of a quiet band is one an operator learns to leave switched off.
    CHECK(std::fabs(after - before) < 0.5);
    CHECK(blanker.blanked_fraction() < 0.01f);
}

TEST_CASE(blanker_does_not_gate_a_strong_signal) {
    std::mt19937 rng(3);
    const double tone = 40000.0;
    // A local station forty times the noise. It is not an impulse and must
    // not be treated as one: gating it turns a clear signal into a buzz.
    auto band = make_band(200000, tone, 0.4f, 0.01f, 0.0f, 0, 0, rng);

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(1.0f);
    blanker.process(band.data(), band.size());
    CHECK(blanker.blanked_fraction() < 0.01f);
}

TEST_CASE(blanker_off_is_off) {
    std::mt19937 rng(4);
    const double tone = 40000.0;
    auto band = make_band(50000, tone, 0.02f, 0.01f, 3.0f, 6528, 6, rng);
    const auto original = band;

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.0f);
    blanker.process(band.data(), band.size());
    for (size_t i = 0; i < band.size(); i++) {
        CHECK(band[i] == original[i]);
    }
}

TEST_CASE(blanker_handles_power_line_bursts) {
    std::mt19937 rng(5);
    const double tone = 40000.0;
    // Twice mains: 100 Hz, and wider than an ignition spark.
    auto band = make_band(200000, tone, 0.02f, 0.01f, 2.0f, 3840, 40, rng);
    const double before = carrier_to_rest_db(band, tone);

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.7f);
    blanker.process(band.data(), band.size());
    // Measured: -22.8 dB to +2.9, so 25.6 dB. Before the reference was
    // stopped from climbing during a burst this read +2.6 dB at this
    // strength: the leading edge went and the rest of the burst stayed.
    CHECK(carrier_to_rest_db(band, tone) > before + 20.0);
}

TEST_CASE(blanker_recovers_after_a_burst_of_impulses) {
    std::mt19937 rng(6);
    const double tone = 40000.0;
    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.7f);

    // A storm crash: dense impulses for a moment.
    auto storm = make_band(40000, tone, 0.02f, 0.01f, 4.0f, 200, 8, rng);
    blanker.process(storm.data(), storm.size());

    // Then the band as it was. The reference has to come back down, or the
    // receiver stays muted for seconds after the noise has passed.
    auto after = make_band(200000, tone, 0.02f, 0.01f, 0.0f, 0, 0, rng);
    const double clean = carrier_to_rest_db(after, tone);
    blanker.process(after.data(), after.size());
    CHECK(std::fabs(carrier_to_rest_db(after, tone) - clean) < 1.0);
}

TEST_CASE(blanker_fills_the_gap_rather_than_leaving_a_hole) {
    // A gate leaves sidebands beside the signal, and those are the audible
    // part of blanking. Zeroing the gap put them 16.6 dB above where a clean
    // band sits; continuing the signal across it recovers about a third.
    std::mt19937 rng(7);
    const double tone = 40000.0;
    auto band = make_band(200000, tone, 0.02f, 0.01f, 2.0f, 3840, 40, rng);

    NoiseBlanker blanker;
    blanker.configure(kRate);
    blanker.set_strength(0.7f);
    blanker.process(band.data(), band.size());

    // Energy at multiples of the burst rate either side of the carrier: that
    // is where a periodic gate puts what it takes out.
    auto power_at = [&](double hz) {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < band.size(); i++) {
            const double p = -kTwoPi * hz * static_cast<double>(i) / kRate;
            re += band[i].real() * std::cos(p) - band[i].imag() * std::sin(p);
            im += band[i].real() * std::sin(p) + band[i].imag() * std::cos(p);
        }
        const double n = static_cast<double>(band.size());
        return (re * re + im * im) / (n * n);
    };
    double sidebands = 0.0;
    for (int k = 1; k <= 20; k++) sidebands += power_at(tone + 100.0 * k) + power_at(tone - 100.0 * k);
    const double dbc = 10.0 * std::log10(sidebands / power_at(tone));

    // Measured: -23.0 dBc when the gap was zeroed, -28.3 when it is filled.
    CHECK(dbc < -26.0);
}

TEST_CASE(blanker_fill_reads_nothing_past_the_end_of_the_block) {
    // A run ending one fitting window before the end of the block must be
    // filled from the block alone: what lies in memory after it cannot move
    // the result. The block is followed by a sentinel sample, set to two
    // different values; every impulse position near the end must fill alike.
    constexpr size_t kCount = 2048;
    bool alike = true;
    for (size_t impulse = 1700; impulse < 1800 && alike; impulse++) {
        std::vector<cfloat> filled[2];
        for (int run = 0; run < 2; run++) {
            std::vector<cfloat> band(kCount + 1);
            for (size_t i = 0; i < kCount; i++) {
                const double phase = kTwoPi * 20000.0 * static_cast<double>(i) / kRate;
                band[i] = cfloat(static_cast<float>(0.01 * std::cos(phase)), static_cast<float>(0.01 * std::sin(phase)));
            }
            band[impulse] = cfloat(1.0f, 1.0f);
            band[kCount] = run == 0 ? cfloat(1000.0f, 0.0f) : cfloat(0.0f, -1000.0f);
            NoiseBlanker blanker;
            blanker.configure(kRate);
            blanker.set_strength(0.7f);
            blanker.process(band.data(), kCount);
            filled[run].assign(band.begin(), band.begin() + kCount);
        }
        alike = filled[0] == filled[1];
    }
    CHECK(alike);
}
