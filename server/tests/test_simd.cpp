#include "../src/dsp/simd.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

namespace simd = fernsdr::simd;

TEST_CASE(simd_atan2_is_within_its_bound_everywhere) {
    double worst = 0.0;
    for (int i = 0; i < 20000; i++) {
        const double angle = -M_PI + 2.0 * M_PI * (i + 0.5) / 20000.0;
        for (const double radius : {1e-6, 1.0, 1e6}) {
            const float y = static_cast<float>(radius * std::sin(angle));
            const float x = static_cast<float>(radius * std::cos(angle));
            worst = std::max(worst, static_cast<double>(std::fabs(simd::fast_atan2(y, x) - std::atan2(y, x))));
        }
    }
    CHECK(worst < 1.5e-5);
    CHECK_NEAR(simd::fast_atan2(0.0f, 0.0f), 0.0, 1e-9);
    CHECK_NEAR(simd::fast_atan2(0.0f, -1.0f), M_PI, 1e-6);
    CHECK_NEAR(simd::fast_atan2(1.0f, 0.0f), M_PI / 2, 1e-6);
    CHECK_NEAR(simd::fast_atan2(-1.0f, 0.0f), -M_PI / 2, 1e-6);
}

TEST_CASE(simd_phase_steps_match_atan2_of_each_step) {
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    for (const size_t n : {size_t(1), size_t(7), size_t(255), size_t(256), size_t(257), size_t(1000)}) {
        std::vector<fernsdr::cfloat> in(n);
        for (auto& s : in) s = fernsdr::cfloat(noise(rng), noise(rng));
        const fernsdr::cfloat previous(0.3f, -0.8f);
        std::vector<float> out(n);
        simd::phase_steps(in.data(), n, previous, out.data());
        fernsdr::cfloat before = previous;
        double worst = 0.0;
        for (size_t i = 0; i < n; i++) {
            const fernsdr::cfloat product = in[i] * std::conj(before);
            before = in[i];
            worst = std::max(worst, std::fabs(std::remainder(static_cast<double>(out[i]) - std::atan2(product.imag(), product.real()), 2 * M_PI)));
        }
        CHECK(worst < 3e-5);
    }
}

TEST_CASE(simd_dot_matches_a_double_sum) {
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> value(-1.0f, 1.0f);
    for (const size_t n : {size_t(0), size_t(3), size_t(16), size_t(17), size_t(315), size_t(1001)}) {
        std::vector<float> a(n), b(n);
        double want = 0.0;
        for (size_t i = 0; i < n; i++) {
            a[i] = value(rng);
            b[i] = value(rng);
            want += static_cast<double>(a[i]) * b[i];
        }
        CHECK_NEAR(simd::dot(a.data(), b.data(), n), want, 1e-4);
    }
    CHECK(simd::instruction_set() != nullptr);
}

TEST_CASE(simd_dot2_is_two_dots_in_one_pass) {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> value(-1.0f, 1.0f);
    for (const size_t n : {size_t(0), size_t(3), size_t(8), size_t(15), size_t(87), size_t(1001)}) {
        std::vector<float> a(n), b0(n), b1(n);
        double want0 = 0.0, want1 = 0.0;
        for (size_t i = 0; i < n; i++) {
            a[i] = value(rng);
            b0[i] = value(rng);
            b1[i] = value(rng);
            want0 += static_cast<double>(a[i]) * b0[i];
            want1 += static_cast<double>(a[i]) * b1[i];
        }
        float got0 = 99.0f, got1 = 99.0f;
        simd::dot2(a.data(), b0.data(), b1.data(), n, &got0, &got1);
        CHECK_NEAR(got0, want0, 1e-4);
        CHECK_NEAR(got1, want1, 1e-4);
    }
}

TEST_CASE(simd_power_to_db_matches_log10_from_the_noise_to_full_scale) {
    std::vector<float> power;
    for (int e = -40; e <= 20; e++) {
        for (const float m : {1.0f, 1.2345f, 1.41f, 1.42f, 1.9999f, 3.3f, 7.7f}) power.push_back(m * std::pow(10.0f, static_cast<float>(e)));
    }
    power.push_back(0.0f);
    std::vector<float> out(power.size());
    simd::power_to_db(power.data(), power.size(), 0.25f, 3.0f, out.data());
    double worst = 0.0;
    for (size_t i = 0; i < power.size(); i++) {
        const double want = 10.0 * std::log10(static_cast<double>(power[i]) * 0.25 + 1e-30) - 3.0;
        worst = std::max(worst, std::fabs(out[i] - want));
    }
    CHECK(worst < 1e-4);
    // Lengths that are not a whole number of vectors.
    std::vector<float> odd(13, 1.0f), odd_out(13);
    simd::power_to_db(odd.data(), odd.size(), 1.0f, 0.0f, odd_out.data());
    for (const float db : odd_out) CHECK_NEAR(db, 0.0, 1e-4);
}
