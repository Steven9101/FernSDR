#include "../src/dsp/fft_split.h"
#include "../src/dsp/fft.h"
#include "test_util.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::Fft;
using fernsdr::FftSplit;
using fernsdr::RealFft;

namespace {

std::vector<float> random_reals(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& s : v) s = dist(rng);
    return v;
}

// In-place radix-2 transform in double: an independent reference whose own
// error, near 1e-15, disappears beside single precision.
void reference_transform(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<std::complex<double>> table(n / 2);
    for (size_t k = 0; k < n / 2; k++) table[k] = std::polar(1.0, -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n));
    for (size_t len = 2; len <= n; len <<= 1) {
        for (size_t start = 0; start < n; start += len) {
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<double> u = a[start + k];
                const std::complex<double> v = a[start + k + len / 2] * table[k * (n / len)];
                a[start + k] = u + v;
                a[start + k + len / 2] = u - v;
            }
        }
    }
}

// Rounding error in a float transform grows with the number of stages. The
// kernels measure 0.14 to 0.25 epsilons per stage RMS and up to 0.71 in the
// worst bin, except the single stage of n = 2 at 0.68 and 0.97. The bounds
// allow 1 and 4, a small multiple of that, and orders of magnitude below
// what a wrong twiddle, a missed bin or a swapped pair would leave.
void check_against_reference(const std::vector<std::complex<double>>& reference, const float* re,
                             const float* im, size_t stride_log) {
    const double epsilon = 5.96e-8;
    const double stages = std::max<double>(1.0, static_cast<double>(stride_log));
    double level = 0, sum = 0, worst = 0;
    for (size_t k = 0; k < reference.size(); k++) {
        level += std::norm(reference[k]);
        const double error = std::norm(reference[k] - std::complex<double>(re[k], im[k]));
        sum += error;
        worst = std::max(worst, error);
    }
    const double rms_level = std::sqrt(level / static_cast<double>(reference.size()));
    CHECK(std::sqrt(sum / static_cast<double>(reference.size())) <= 1.0 * epsilon * stages * rms_level);
    CHECK(std::sqrt(worst) <= 4.0 * epsilon * stages * rms_level);
}

}  // namespace

TEST_CASE(fft_split_matches_a_double_precision_transform_at_every_length) {
    for (size_t log = 0; log <= 20; log++) {
        const size_t n = size_t{1} << log;
        const auto re = random_reals(n, static_cast<uint32_t>(300 + log));
        const auto im = random_reals(n, static_cast<uint32_t>(400 + log));
        std::vector<std::complex<double>> forward(n), inverse(n);
        for (size_t i = 0; i < n; i++) {
            forward[i] = {re[i], im[i]};
            inverse[i] = {re[i], -im[i]};
        }
        reference_transform(forward);
        // The unscaled inverse is the conjugate of the forward transform of
        // the conjugate.
        reference_transform(inverse);
        for (auto& v : inverse) v = std::conj(v);

        const FftSplit plan(n);
        auto got_re = re, got_im = im;
        plan.forward(got_re.data(), got_im.data());
        check_against_reference(forward, got_re.data(), got_im.data(), log);
        got_re = re;
        got_im = im;
        plan.inverse_unscaled(got_re.data(), got_im.data());
        check_against_reference(inverse, got_re.data(), got_im.data(), log);
    }
}

TEST_CASE(real_fft_matches_a_double_precision_transform_at_every_length) {
    for (size_t log = 2; log <= 21; log++) {
        const size_t n = size_t{1} << log;
        const auto input = random_reals(n, static_cast<uint32_t>(500 + log));
        std::vector<std::complex<double>> full(n);
        for (size_t i = 0; i < n; i++) full[i] = {input[i], 0.0};
        reference_transform(full);
        full.resize(n / 2 + 1);

        const RealFft plan(n);
        std::vector<float> out_re(n / 2 + 1), out_im(n / 2 + 1);
        plan.forward(input.data(), out_re.data(), out_im.data());
        check_against_reference(full, out_re.data(), out_im.data(), log);
        // Real input has real DC and Nyquist bins, exactly.
        CHECK(out_im[0] == 0.0f);
        CHECK(out_im[n / 2] == 0.0f);
    }
}

TEST_CASE(fft_split_windowed_front_end_matches_a_windowed_copy) {
    // Reading the front end's pairs and windowing them in the first pass must
    // give the bins of the split, windowed copy it replaces, at every length
    // and so on every kind of plan. Not always bit for bit: with FMA the
    // compiler may fuse a window product into the butterfly after it, one
    // rounding fewer. A misplaced half or window index would miss by the
    // signal level itself.
    for (size_t log = 1; log <= 20; log++) {
        const size_t n = size_t{1} << log, half = n / 2;
        const auto re = random_reals(n, static_cast<uint32_t>(700 + log));
        const auto im = random_reals(n, static_cast<uint32_t>(800 + log));
        std::vector<float> window(n), older(2 * half), newer(2 * half);
        for (size_t e = 0; e < n; e++) {
            window[e] = static_cast<float>(std::sin(M_PI * (static_cast<double>(e) + 0.5) / static_cast<double>(n)));
            float* pair = e < half ? &older[2 * e] : &newer[2 * (e - half)];
            pair[0] = re[e];
            pair[1] = im[e];
        }
        std::vector<float> want_re(n), want_im(n), got_re(n), got_im(n);
        for (size_t e = 0; e < n; e++) {
            want_re[e] = re[e] * window[e];
            want_im[e] = im[e] * window[e];
        }
        const FftSplit plan(n);
        plan.forward(want_re.data(), want_im.data());
        plan.forward_windowed(older.data(), newer.data(), window.data(), got_re.data(), got_im.data());
        double level = 0, worst = 0;
        for (size_t k = 0; k < n; k++) {
            level += static_cast<double>(want_re[k]) * want_re[k] + static_cast<double>(want_im[k]) * want_im[k];
            const double dr = got_re[k] - want_re[k], di = got_im[k] - want_im[k];
            worst = std::max(worst, dr * dr + di * di);
        }
        const double rms_level = std::sqrt(level / static_cast<double>(n));
        CHECK(std::sqrt(worst) <= 4.0 * 5.96e-8 * static_cast<double>(log) * rms_level);
    }
}

TEST_CASE(fft_split_matches_the_interleaved_transform) {
    // The two implementations must agree exactly enough that either can be
    // used anywhere; the split one exists only to go faster.
    for (size_t n : {8u, 64u, 1024u, 8192u}) {
        auto re = random_reals(n, static_cast<uint32_t>(n));
        auto im = random_reals(n, static_cast<uint32_t>(n) + 1);

        std::vector<fernsdr::cfloat> interleaved(n);
        for (size_t i = 0; i < n; i++) interleaved[i] = fernsdr::cfloat(re[i], im[i]);
        Fft(n).forward(interleaved.data());

        FftSplit(n).forward(re.data(), im.data());

        for (size_t i = 0; i < n; i++) {
            CHECK_NEAR(re[i], interleaved[i].real(), 2e-3 * std::sqrt(static_cast<double>(n)));
            CHECK_NEAR(im[i], interleaved[i].imag(), 2e-3 * std::sqrt(static_cast<double>(n)));
        }
    }
}

TEST_CASE(fft_split_roundtrip_is_identity) {
    for (size_t n : {1u, 2u, 4u, 8u, 16u, 256u, 4096u}) {
        auto re = random_reals(n, static_cast<uint32_t>(n) + 5);
        auto im = random_reals(n, static_cast<uint32_t>(n) + 6);
        auto want_re = re;
        auto want_im = im;

        FftSplit fft(n);
        fft.forward(re.data(), im.data());
        fft.inverse(re.data(), im.data());

        for (size_t i = 0; i < n; i++) {
            CHECK_NEAR(re[i], want_re[i], 1e-4);
            CHECK_NEAR(im[i], want_im[i], 1e-4);
        }
    }
}

TEST_CASE(fft_split_small_fused_stages_match_both_reference_directions) {
    for (size_t n : {16u, 32u, 256u, 512u}) {
        for (bool inverse : {false, true}) {
            auto re = random_reals(n, 97), im = random_reals(n, 98);
            std::vector<fernsdr::cfloat> reference(n);
            for (size_t i = 0; i < n; i++) reference[i] = {re[i], im[i]};
            if (inverse) {
                Fft(n).inverse(reference.data());
                FftSplit(n).inverse(re.data(), im.data());
            } else {
                Fft(n).forward(reference.data());
                FftSplit(n).forward(re.data(), im.data());
            }
            const double tolerance = inverse ? 2e-6 : 2e-6 * std::sqrt(static_cast<double>(n));
            for (size_t i = 0; i < n; i++) {
                CHECK_NEAR(re[i], reference[i].real(), tolerance);
                CHECK_NEAR(im[i], reference[i].imag(), tolerance);
            }
        }
    }
}

TEST_CASE(fft_split_accepts_unaligned_arrays_without_crossing_their_bounds) {
    for (size_t n : {16u, 32u, 128u, 256u, 1024u, 16384u}) {
        auto signal_re = random_reals(n, 105), signal_im = random_reals(n, 106);
        std::vector<fernsdr::cfloat> reference(n);
        for (size_t i = 0; i < n; i++) reference[i] = {signal_re[i], signal_im[i]};
        Fft(n).forward(reference.data());
        for (size_t offset : {1u, 3u}) {
            std::vector<float> re(n + 8, 17.0f), im(n + 8, -19.0f);
            std::copy(signal_re.begin(), signal_re.end(), re.begin() + offset);
            std::copy(signal_im.begin(), signal_im.end(), im.begin() + offset);
            FftSplit(n).forward(re.data() + offset, im.data() + offset);
            double maximum_error = 0;
            for (size_t i = 0; i < n; i++) {
                maximum_error = std::max(maximum_error, static_cast<double>(std::max(
                    std::abs(re[offset + i] - reference[i].real()),
                    std::abs(im[offset + i] - reference[i].imag()))));
            }
            CHECK(maximum_error < 2e-6 * std::sqrt(static_cast<double>(n)));
            for (size_t i = 0; i < re.size(); i++) {
                if (i >= offset && i < offset + n) continue;
                CHECK_EQ(re[i], 17.0f);
                CHECK_EQ(im[i], -19.0f);
            }
        }
    }
}

TEST_CASE(fft_split_large_fused_transforms_roundtrip) {
    for (size_t n : {131072u, 524288u}) {
        auto re = random_reals(n, 51), im = random_reals(n, 52);
        const auto want_re = re, want_im = im;
        FftSplit fft(n);
        fft.forward(re.data(), im.data());
        fft.inverse(re.data(), im.data());
        float maximum_error = 0;
        for (size_t i = 0; i < n; i++) {
            maximum_error = std::max(maximum_error, std::max(std::abs(re[i] - want_re[i]), std::abs(im[i] - want_im[i])));
        }
        CHECK(maximum_error < 1e-4f);
    }
}

TEST_CASE(real_fft_matches_a_complex_transform_of_the_same_signal) {
    // A real front end's samples, transformed two ways.
    for (size_t n : {16u, 256u, 4096u}) {
        auto signal = random_reals(n, static_cast<uint32_t>(n) + 11);

        std::vector<fernsdr::cfloat> reference(n);
        for (size_t i = 0; i < n; i++) reference[i] = fernsdr::cfloat(signal[i], 0.0f);
        Fft(n).forward(reference.data());

        std::vector<float> re(n / 2 + 1), im(n / 2 + 1);
        RealFft(n).forward(signal.data(), re.data(), im.data());

        const double tolerance = 2e-3 * std::sqrt(static_cast<double>(n));
        for (size_t k = 0; k <= n / 2; k++) {
            CHECK_NEAR(re[k], reference[k].real(), tolerance);
            CHECK_NEAR(im[k], reference[k].imag(), tolerance);
        }
    }
}

TEST_CASE(real_fft_places_a_tone_in_the_right_bin_at_the_right_level) {
    const size_t n = 4096;
    const size_t bin = 137;
    std::vector<float> signal(n);
    for (size_t i = 0; i < n; i++) {
        signal[i] = static_cast<float>(std::cos(2.0 * M_PI * bin * i / n));
    }

    std::vector<float> re(n / 2 + 1), im(n / 2 + 1);
    RealFft(n).forward(signal.data(), re.data(), im.data());

    for (size_t k = 0; k <= n / 2; k++) {
        const double magnitude = std::hypot(re[k], im[k]);
        // A real cosine splits its energy between +f and -f, so the positive
        // bin holds half the amplitude times n.
        if (k == bin) CHECK_NEAR(magnitude, n / 2.0, n * 0.01);
        else CHECK_NEAR(magnitude, 0.0, n * 0.01);
    }
}

TEST_CASE(real_fft_handles_dc_and_nyquist) {
    const size_t n = 64;
    std::vector<float> dc(n, 0.5f);
    std::vector<float> re(n / 2 + 1), im(n / 2 + 1);
    RealFft fft(n);
    fft.forward(dc.data(), re.data(), im.data());
    CHECK_NEAR(re[0], 0.5 * n, 1e-3);
    CHECK_NEAR(im[0], 0.0, 1e-4);
    for (size_t k = 1; k <= n / 2; k++) CHECK_NEAR(std::hypot(re[k], im[k]), 0.0, 1e-3);

    // Alternating +/-1 is exactly the Nyquist frequency.
    std::vector<float> nyquist(n);
    for (size_t i = 0; i < n; i++) nyquist[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    fft.forward(nyquist.data(), re.data(), im.data());
    CHECK_NEAR(re[n / 2], static_cast<double>(n), 1e-2);
    CHECK_NEAR(im[n / 2], 0.0, 1e-4);
    CHECK_NEAR(re[0], 0.0, 1e-2);
}

TEST_CASE(real_fft_windowed_halves_match_a_full_windowed_transform) {
    for (size_t n : {4u, 64u, 65536u, 1048576u}) {
        const auto signal = random_reals(n, 101);
        std::vector<float> window(n), windowed(n);
        for (size_t i = 0; i < n; i++) {
            window[i] = static_cast<float>(std::sin(M_PI * (i + 0.5) / n));
            windowed[i] = signal[i] * window[i];
        }
        RealFft fft(n);
        std::vector<float> expected_re(n / 2 + 1), expected_im(n / 2 + 1);
        std::vector<float> re(n / 2 + 1), im(n / 2 + 1);
        fft.forward(windowed.data(), expected_re.data(), expected_im.data());
        for (bool analytic : {false, true}) {
            fft.forward_windowed_halves(signal.data(), signal.data() + n / 2, window.data(),
                                         re.data(), im.data(), analytic);
            double maximum_error = 0.0;
            for (size_t k = 0; k <= n / 2; k++) {
                const float gain = analytic && k > 0 && k < n / 2 ? 2.0f : 1.0f;
                maximum_error = std::max(maximum_error, static_cast<double>(std::max(
                    std::abs(re[k] - gain * expected_re[k]), std::abs(im[k] - gain * expected_im[k]))));
            }
            CHECK_NEAR(maximum_error, 0.0, 1e-6);
        }
    }
}

TEST_CASE(real_fft_sine_window_from_tables_matches_the_window_array) {
    // Long plans compute the channelizer's sine window from small tables
    // instead of reading it; the bins must match the array's to rounding, and
    // a wrong block offset or sample parity would miss by the signal itself.
    for (size_t log = 4; log <= 21; log++) {
        const size_t n = size_t{1} << log;
        const auto signal = random_reals(n, static_cast<uint32_t>(900 + log));
        std::vector<float> window(n);
        for (size_t i = 0; i < n; i++)
            window[i] = static_cast<float>(std::sin(M_PI * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));
        const RealFft fft(n);
        std::vector<float> want_re(n / 2 + 1), want_im(n / 2 + 1), got_re(n / 2 + 1), got_im(n / 2 + 1);
        for (bool analytic : {false, true}) {
            fft.forward_windowed_halves(signal.data(), signal.data() + n / 2, window.data(), want_re.data(),
                                        want_im.data(), analytic);
            fft.forward_sine_windowed_halves(signal.data(), signal.data() + n / 2, window.data(), got_re.data(),
                                             got_im.data(), analytic);
            double level = 0, worst = 0;
            for (size_t k = 0; k <= n / 2; k++) {
                level += static_cast<double>(want_re[k]) * want_re[k] + static_cast<double>(want_im[k]) * want_im[k];
                const double dr = got_re[k] - want_re[k], di = got_im[k] - want_im[k];
                worst = std::max(worst, dr * dr + di * di);
            }
            const double rms_level = std::sqrt(level / static_cast<double>(n / 2 + 1));
            CHECK(std::sqrt(worst) <= 4.0 * 5.96e-8 * static_cast<double>(log) * rms_level);
        }
    }
}

TEST_CASE(fft_split_large_forward_matches_the_independent_transform) {
    // A round trip alone also passes when both directions share the same
    // incorrect permutation. Check the forward bins against the separate
    // interleaved implementation with different stage counts and copy parities.
    for (size_t n : {16384u, 32768u, 65536u, 524288u}) {
        auto re = random_reals(n, 91), im = random_reals(n, 92);
        std::vector<fernsdr::cfloat> reference(n);
        for (size_t i = 0; i < n; i++) reference[i] = {re[i], im[i]};
        Fft(n).forward(reference.data());
        FftSplit(n).forward(re.data(), im.data());

        double error_power = 0.0, signal_power = 0.0, maximum_error = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double er = re[i] - reference[i].real();
            const double ei = im[i] - reference[i].imag();
            error_power += er * er + ei * ei;
            signal_power += std::norm(reference[i]);
            maximum_error = std::max(maximum_error, std::max(std::abs(er), std::abs(ei)));
        }
        CHECK(std::sqrt(error_power / signal_power) < 2e-6);
        CHECK(maximum_error < 5e-5 * std::sqrt(static_cast<double>(n)));
    }
}

TEST_CASE(real_fft_large_forward_preserves_aliased_input) {
    // The waterfall writes its real output over the windowed input. Packing
    // must finish reading that input before any output bin is published.
    for (size_t n : {65536u, 1048576u}) {
        const auto signal = random_reals(n, 93);
        std::vector<fernsdr::cfloat> reference(n);
        for (size_t i = 0; i < n; i++) reference[i] = {signal[i], 0.0f};
        Fft(n).forward(reference.data());
        RealFft fft(n);
        for (bool alias_imaginary : {false, true}) {
            auto input = signal;
            std::vector<float> other(n / 2 + 1);
            float* re = alias_imaginary ? other.data() : input.data();
            float* im = alias_imaginary ? input.data() : other.data();
            fft.forward(input.data(), re, im);
            double error_power = 0.0, signal_power = 0.0;
            for (size_t k = 0; k <= n / 2; k++) {
                const double er = re[k] - reference[k].real();
                const double ei = im[k] - reference[k].imag();
                error_power += er * er + ei * ei;
                signal_power += std::norm(reference[k]);
            }
            CHECK(std::sqrt(error_power / signal_power) < 2e-6);
        }
    }
}

TEST_CASE(real_fft_large_preserves_weak_tones_and_endpoint_bins) {
    const size_t n = 1048576;
    const size_t strong_bin = n / 4 - 1, weak_bin = n / 4 + 1;
    std::vector<float> signal(n), re(n / 2 + 1), im(n / 2 + 1);
    for (size_t i = 0; i < n; i++) {
        const double phase = 2.0 * M_PI * i / n;
        signal[i] = static_cast<float>(0.125 + (i % 2 ? -0.0625 : 0.0625) +
            0.5 * std::cos(phase * strong_bin) + 0.000005 * std::sin(phase * weak_bin));
    }
    RealFft(n).forward(signal.data(), re.data(), im.data());
    CHECK_NEAR(re[0], 0.125 * n, 0.02);
    CHECK_NEAR(im[0], 0.0, 0.0);
    CHECK_NEAR(re[n / 2], 0.0625 * n, 0.02);
    CHECK_NEAR(im[n / 2], 0.0, 0.0);
    CHECK_NEAR(re[strong_bin], 0.25 * n, 0.1);
    CHECK_NEAR(im[strong_bin], 0.0, 0.02);
    CHECK_NEAR(re[weak_bin], 0.0, 0.02);
    CHECK_NEAR(im[weak_bin], -0.0000025 * n, 0.03);
    CHECK_NEAR(std::hypot(re[n / 4], im[n / 4]), 0.0, 0.02);
}
