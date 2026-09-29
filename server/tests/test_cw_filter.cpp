#include "../src/dsp/cw_filter.h"
#include "../src/dsp/agc.h"
#include "test_util.h"
#include <algorithm>
#include <limits>

using namespace fernsdr;

namespace {
constexpr double tau = 6.2831853071795864769;
double response(double rate, double low, double high, double tone, bool agc_enabled = false) {
    CwFilter filter;
    filter.configure(rate, low, high, true);
    Agc agc;
    agc.configure(rate);
    agc.set_profile(AgcProfile::Fast);
    agc.set_max_gain_db(60);
    double power = 0;
    size_t collected = 0;
    const size_t count = static_cast<size_t>(rate * (agc_enabled ? 4 : 0.6));
    for (size_t start = 0; start < count; start += 137) {
        cfloat samples[137];
        const size_t size = std::min<size_t>(137, count - start);
        for (size_t i = 0; i < size; i++) samples[i] = std::polar(0.25f, static_cast<float>(
            std::remainder(tau * tone * (start + i) / rate, tau)));
        // RF retunes and unrelated controls must not reset this state.
        filter.configure(rate, low, high, true);
        filter.process(samples, size);
        if (agc_enabled) agc.process(samples, size);
        for (size_t i = 0; i < size; i++) if (start + i >= count / 2) {
            power += std::norm(samples[i]);
            collected++;
        }
    }
    return std::sqrt(power / collected);
}
}

TEST_CASE(cw_selector_resolves_narrow_filters_independently_of_fft_bins) {
    for (double rate : {8000.0, 12000.0, 15625.0, 48000.0}) {
        for (double width : {50.0, 100.0, 250.0, 500.0, 1000.0}) {
            for (double sign : {-1.0, 1.0}) {
                const double center = sign * 713.25;
                const double low = center - width / 2, high = center + width / 2;
                const double middle = response(rate, low, high, center);
                CHECK_NEAR(middle, 0.25, 0.003);
                CHECK_NEAR(response(rate, low, high, high) / middle, std::sqrt(0.5), 0.02);
                CHECK(response(rate, low, high, center + width * 2) / middle < 0.0003);
            }
        }
    }
}

TEST_CASE(cw_selector_rejects_the_mirrored_carrier_after_agc_recovers) {
    for (const double sign : {-1.0, 1.0}) {
        const double center = sign * 700;
        const double rejected = response(12000, center - 125, center + 125, -center, true);
        CHECK(rejected < 0.0005);
        CHECK(response(12000, center - 50, center + 50, center + 150, true) < 0.005);
    }
}

TEST_CASE(cw_selector_changes_are_bounded_and_eventually_reach_the_latest_filter) {
    CwFilter filter;
    filter.configure(12000, 450, 950, true);
    double phase = 0;
    double maximum = 0;
    double tail_power = 0;
    for (size_t block = 0; block < 500; block++) {
        if (block < 150) {
            const double width = block % 2 ? 50 : 500;
            filter.configure(12000, 700 - width / 2, 700 + width / 2, block % 3 != 0);
        } else filter.configure(12000, 1150, 1250, true);
        cfloat samples[128];
        for (auto& sample : samples) {
            sample = std::polar(0.25f, static_cast<float>(phase));
            phase = std::remainder(phase + tau * 700 / 12000, tau);
        }
        if (block == 75) samples[13] = {std::numeric_limits<float>::quiet_NaN(), 0};
        filter.process(samples, 128);
        for (auto sample : samples) {
            CHECK(std::isfinite(sample.real()) && std::isfinite(sample.imag()));
            maximum = std::max(maximum, static_cast<double>(std::abs(sample)));
            if (block > 400) tail_power += std::norm(sample);
        }
    }
    CHECK(maximum < 0.5);
    CHECK(std::sqrt(tail_power / (99 * 128)) < 1e-5);
    filter.reset();
    filter.configure(12000, 0, 0, false);
    cfloat input[] = {{0.2f, -0.1f}, {-0.3f, 0.4f}};
    filter.process(input, 2);
    CHECK(input[0] == cfloat(0.2f, -0.1f));
    CHECK(input[1] == cfloat(-0.3f, 0.4f));
}

TEST_CASE(cw_selector_coalesces_changes_that_arrive_during_a_crossfade) {
    CwFilter actual, reference;
    actual.configure(12000, 650, 750, true);
    reference.configure(12000, 650, 750, true);
    double phase = 0;
    for (int block = 0; block < 500; block++) {
        if (block == 50) {
            actual.configure(12000, 450, 950, true);
            reference.configure(12000, 450, 950, true);
        }
        if (block == 51) actual.configure(12000, 575, 825, true);
        if (block == 52) actual.configure(12000, 450, 950, true);
        cfloat a[16], b[16];
        for (size_t i = 0; i < 16; i++) {
            a[i] = b[i] = std::polar(0.25f, static_cast<float>(phase));
            phase = std::remainder(phase + tau * 700 / 12000, tau);
        }
        actual.process(a, 16);
        reference.process(b, 16);
        for (size_t i = 0; i < 16; i++) CHECK(std::abs(a[i] - b[i]) < 1e-6);
    }
}
