#include "../src/dsp/mdct.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

using fernsdr::Mdct;

namespace {

std::vector<float> random_signal(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& s : v) s = dist(rng);
    return v;
}

// out[k] = sum_n in[n] cos(pi/M (n+0.5)(k+0.5))
std::vector<float> naive_dct4(const std::vector<float>& in) {
    const size_t m = in.size();
    std::vector<float> out(m);
    for (size_t k = 0; k < m; k++) {
        double acc = 0.0;
        for (size_t n = 0; n < m; n++) {
            acc += in[n] * std::cos(M_PI / m * (n + 0.5) * (k + 0.5));
        }
        out[k] = static_cast<float>(acc);
    }
    return out;
}

// X[k] = sum_{n=0}^{2M-1} x[n] cos(pi/M (n + 0.5 + M/2)(k + 0.5))
std::vector<float> naive_mdct(const std::vector<float>& x) {
    const size_t m = x.size() / 2;
    std::vector<float> out(m);
    for (size_t k = 0; k < m; k++) {
        double acc = 0.0;
        for (size_t n = 0; n < 2 * m; n++) {
            acc += x[n] * std::cos(M_PI / m * (n + 0.5 + m / 2.0) * (k + 0.5));
        }
        out[k] = static_cast<float>(acc);
    }
    return out;
}

// y[n] = (2/M) * sum_k X[k] cos(pi/M (n + 0.5 + M/2)(k + 0.5))
std::vector<float> naive_imdct(const std::vector<float>& coeffs) {
    const size_t m = coeffs.size();
    std::vector<float> out(2 * m);
    for (size_t n = 0; n < 2 * m; n++) {
        double acc = 0.0;
        for (size_t k = 0; k < m; k++) {
            acc += coeffs[k] * std::cos(M_PI / m * (n + 0.5 + m / 2.0) * (k + 0.5));
        }
        out[n] = static_cast<float>(acc * 2.0 / m);
    }
    return out;
}

}  // namespace

TEST_CASE(mdct_dct4_stage_matches_direct_sum) {
    // Exercised through the public MDCT by feeding a window whose fold is the
    // identity: quarters c and d zero, b zero, so fold == [0.., a].
    const size_t m = 64;
    Mdct mdct(m);
    auto a = random_signal(m / 2, 3);

    std::vector<float> window(2 * m, 0.0f);
    for (size_t i = 0; i < m / 2; i++) window[i] = a[i];

    std::vector<float> fold(m, 0.0f);
    for (size_t i = 0; i < m / 2; i++) fold[m / 2 + i] = a[i];

    std::vector<float> got(m);
    mdct.forward(window.data(), got.data());
    auto want = naive_dct4(fold);
    for (size_t k = 0; k < m; k++) CHECK_NEAR(got[k], want[k], 1e-3);
}

TEST_CASE(mdct_forward_matches_direct_sum) {
    for (size_t m : {8u, 32u, 128u}) {
        Mdct mdct(m);
        auto x = random_signal(2 * m, static_cast<uint32_t>(m));
        std::vector<float> got(m);
        mdct.forward(x.data(), got.data());
        auto want = naive_mdct(x);
        for (size_t k = 0; k < m; k++) CHECK_NEAR(got[k], want[k], 1e-2);
    }
}

TEST_CASE(mdct_inverse_matches_direct_sum) {
    for (size_t m : {8u, 32u, 128u}) {
        Mdct mdct(m);
        auto coeffs = random_signal(m, static_cast<uint32_t>(m) + 100);
        std::vector<float> got(2 * m);
        mdct.inverse(coeffs.data(), got.data());
        auto want = naive_imdct(coeffs);
        for (size_t n = 0; n < 2 * m; n++) CHECK_NEAR(got[n], want[n], 1e-3);
    }
}

TEST_CASE(mdct_window_and_overlap_add_reconstruct_exactly) {
    // The property the codec actually depends on: streaming a signal through
    // window -> MDCT -> IMDCT -> window -> overlap-add returns it unchanged.
    const size_t m = 128;
    Mdct mdct(m);
    auto window = fernsdr::make_sine_window(2 * m);

    const size_t frames = 12;
    auto signal = random_signal(m * (frames + 1), 42);

    std::vector<float> reconstructed(signal.size(), 0.0f);
    std::vector<float> buf(2 * m), coeffs(m), out(2 * m);

    for (size_t f = 0; f + 1 < frames; f++) {
        const size_t start = f * m;
        for (size_t i = 0; i < 2 * m; i++) buf[i] = signal[start + i] * window[i];
        mdct.forward(buf.data(), coeffs.data());
        mdct.inverse(coeffs.data(), out.data());
        for (size_t i = 0; i < 2 * m; i++) reconstructed[start + i] += out[i] * window[i];
    }

    // Only samples covered by two complete frames are fully reconstructed.
    for (size_t i = m; i < (frames - 1) * m; i++) CHECK_NEAR(reconstructed[i], signal[i], 2e-4);
}

TEST_CASE(mdct_sine_window_satisfies_princen_bradley) {
    auto w = fernsdr::make_sine_window(256);
    for (size_t i = 0; i < 128; i++) {
        CHECK_NEAR(w[i] * w[i] + w[i + 128] * w[i + 128], 1.0, 1e-6);
    }
}
