#include "../src/dsp/fft.h"
#include "test_util.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::cfloat;
using fernsdr::Fft;

namespace {

// O(n^2) reference transform; slow but obviously correct.
std::vector<cfloat> naive_dft(const std::vector<cfloat>& in, bool inverse) {
    const size_t n = in.size();
    std::vector<cfloat> out(n);
    const double sign = inverse ? 2.0 * M_PI : -2.0 * M_PI;
    for (size_t k = 0; k < n; k++) {
        std::complex<double> acc(0.0, 0.0);
        for (size_t j = 0; j < n; j++) {
            double angle = sign * static_cast<double>(k) * static_cast<double>(j) / static_cast<double>(n);
            acc += std::complex<double>(in[j].real(), in[j].imag()) *
                   std::complex<double>(std::cos(angle), std::sin(angle));
        }
        out[k] = cfloat(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

std::vector<cfloat> random_signal(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<cfloat> v(n);
    // One draw at a time, imaginary part first as GCC on x86-64 did when this
    // was an argument list, whose order C++ leaves open.
    for (auto& s : v) {
        const float im = dist(rng);
        const float re = dist(rng);
        s = cfloat(re, im);
    }
    return v;
}

}  // namespace

TEST_CASE(fft_matches_naive_dft) {
    for (size_t n : {1u, 2u, 4u, 8u, 16u, 64u, 256u}) {
        auto in = random_signal(n, static_cast<uint32_t>(n));
        auto want = naive_dft(in, false);
        auto got = in;
        Fft(n).forward(got.data());
        for (size_t i = 0; i < n; i++) {
            CHECK_NEAR(got[i].real(), want[i].real(), 1e-3 * n);
            CHECK_NEAR(got[i].imag(), want[i].imag(), 1e-3 * n);
        }
    }
}

TEST_CASE(fft_roundtrip_is_identity) {
    for (size_t n : {8u, 32u, 512u, 4096u}) {
        auto in = random_signal(n, static_cast<uint32_t>(n) + 7);
        auto work = in;
        Fft f(n);
        f.forward(work.data());
        f.inverse(work.data());
        for (size_t i = 0; i < n; i++) {
            CHECK_NEAR(work[i].real(), in[i].real(), 1e-4);
            CHECK_NEAR(work[i].imag(), in[i].imag(), 1e-4);
        }
    }
}

TEST_CASE(fft_resolves_a_single_tone) {
    // A pure bin-centred tone must land entirely in one bin: this catches
    // twiddle sign or ordering mistakes that roundtrip tests hide.
    const size_t n = 1024;
    const size_t bin = 137;
    std::vector<cfloat> x(n);
    for (size_t i = 0; i < n; i++) {
        double a = 2.0 * M_PI * static_cast<double>(bin) * static_cast<double>(i) / static_cast<double>(n);
        x[i] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    Fft(n).forward(x.data());
    for (size_t i = 0; i < n; i++) {
        double mag = std::abs(x[i]);
        if (i == bin) CHECK_NEAR(mag, static_cast<double>(n), 1e-1);
        else CHECK_NEAR(mag, 0.0, 1e-1);
    }
}
