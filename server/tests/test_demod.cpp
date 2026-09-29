#include "../src/dsp/agc.h"
#include "../src/dsp/audio_post.h"
#include "../src/dsp/demod.h"
#include "test_util.h"

#include <cmath>
#include <functional>
#include <random>
#include <vector>

using fernsdr::AudioPost;
using fernsdr::cfloat;
using fernsdr::Demodulator;
using fernsdr::Mode;

namespace {

constexpr double kRate = 12000.0;

// Amplitude of a real signal at `freq`, by correlation over the given range.
double amplitude_at(const std::vector<float>& x, double freq, size_t start, size_t count) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double a = 2.0 * M_PI * freq * static_cast<double>(i + start) / kRate;
        re += x[i + start] * std::cos(a);
        im += x[i + start] * std::sin(a);
    }
    return 2.0 * std::sqrt(re * re + im * im) / count;
}

double rms(const std::vector<float>& x, size_t start = 0) {
    double acc = 0.0;
    for (size_t i = start; i < x.size(); i++) acc += static_cast<double>(x[i]) * x[i];
    return std::sqrt(acc / (x.size() - start));
}

}  // namespace

TEST_CASE(demod_ssb_takes_the_real_part_at_full_amplitude) {
    const size_t n = 4096;
    std::vector<cfloat> in(n);
    for (size_t i = 0; i < n; i++) {
        const double a = 2.0 * M_PI * 1000.0 * i / kRate;
        in[i] = cfloat(static_cast<float>(0.4 * std::cos(a)), static_cast<float>(0.4 * std::sin(a)));
    }
    Demodulator d;
    d.configure(Mode::Usb, kRate);
    std::vector<float> out(n);
    d.process(in.data(), n, out.data());
    CHECK_NEAR(amplitude_at(out, 1000.0, 0, n), 0.4, 0.01);
    // S-meter reads the complex channel power: |0.4|^2 -> -7.96 dBFS.
    CHECK_NEAR(d.level_dbfs(), 20.0 * std::log10(0.4), 0.1);
}

TEST_CASE(demod_am_recovers_the_envelope) {
    const size_t n = 24000;
    std::vector<cfloat> in(n);
    for (size_t i = 0; i < n; i++) {
        const double m = 1.0 + 0.5 * std::cos(2.0 * M_PI * 400.0 * i / kRate);
        in[i] = cfloat(static_cast<float>(0.5 * m), 0.0f);
    }
    Demodulator d;
    d.configure(Mode::Am, kRate);
    std::vector<float> out(n);
    d.process(in.data(), n, out.data());
    // Carrier amplitude 0.5, modulation depth 0.5 -> audio amplitude 0.25.
    CHECK_NEAR(amplitude_at(out, 400.0, 4000, 16000), 0.25, 0.02);
}

TEST_CASE(demod_sam_locks_to_an_offset_carrier) {
    // A carrier 40 Hz off tune, which is exactly what SAM is for: envelope
    // detection would still work here, but SAM must lock and stay clean.
    const size_t n = 48000;
    std::vector<cfloat> in(n);
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / kRate;
        const double m = 1.0 + 0.5 * std::cos(2.0 * M_PI * 400.0 * t);
        const double carrier = 2.0 * M_PI * 40.0 * t;
        in[i] = cfloat(static_cast<float>(0.5 * m * std::cos(carrier)),
                       static_cast<float>(0.5 * m * std::sin(carrier)));
    }
    Demodulator d;
    d.configure(Mode::Sam, kRate);
    std::vector<float> out(n);
    d.process(in.data(), n, out.data());

    CHECK(d.pll_locked());
    CHECK_NEAR(d.pll_offset_hz(), 40.0, 3.0);
    CHECK_NEAR(amplitude_at(out, 400.0, 24000, 20000), 0.25, 0.03);
}

TEST_CASE(demod_sam_tracks_carriers_on_both_sides_of_the_capture_range) {
    const size_t count = 48000;
    std::vector<cfloat> input(count);
    std::vector<float> output(count);
    for (double offset : {-200.0, -80.0, 80.0, 200.0}) {
        for (size_t i = 0; i < count; i++) {
            const double t = static_cast<double>(i) / kRate;
            const double envelope = 0.2 * (1.0 + 0.8 * std::cos(2.0 * M_PI * 600.0 * t));
            input[i] = cfloat(static_cast<float>(envelope * std::cos(2.0 * M_PI * offset * t)),
                              static_cast<float>(envelope * std::sin(2.0 * M_PI * offset * t)));
        }
        Demodulator demod;
        demod.configure(Mode::Sam, kRate);
        // Uneven packet sizes must not change loop acquisition or audio.
        for (size_t start = 0; start < count; start += 137) {
            demod.process(input.data() + start, std::min(size_t{137}, count - start), output.data() + start);
        }
        CHECK(demod.pll_locked());
        CHECK_NEAR(demod.pll_offset_hz(), offset, 1.0);
        CHECK_NEAR(amplitude_at(output, 600.0, 24000, 24000), 0.16, 0.01);
        CHECK(amplitude_at(output, std::abs(offset), 24000, 24000) < 0.005);
    }
}

TEST_CASE(demod_sam_catches_a_carrier_at_the_edge_of_its_capture_range_at_every_audio_rate) {
    // The loop is steered once a run of samples. A run too long for the
    // rate let a carrier near 250 Hz off turn half a cycle within it, its
    // error averaged to nothing, and at 8 kHz the loop never caught it.
    for (const double rate : {8000.0, 12000.0, 24000.0}) {
        for (const double offset : {-245.0, 245.0}) {
            const size_t count = static_cast<size_t>(rate * 4.0);
            std::vector<cfloat> input(count);
            std::vector<float> output(count);
            for (size_t i = 0; i < count; i++) {
                const double t = static_cast<double>(i) / rate;
                const double envelope = 0.2 * (1.0 + 0.5 * std::cos(2.0 * M_PI * 600.0 * t));
                input[i] = std::polar(static_cast<float>(envelope), static_cast<float>(2.0 * M_PI * offset * t));
            }
            Demodulator demod;
            demod.configure(Mode::Sam, rate);
            for (size_t start = 0; start < count; start += 240) {
                demod.process(input.data() + start, std::min(size_t{240}, count - start), output.data() + start);
            }
            CHECK(demod.pll_locked());
            CHECK_NEAR(demod.pll_offset_hz(), offset, 1.0);
        }
    }
}

TEST_CASE(demod_fm_removes_tuning_offset_without_losing_audio) {
    const size_t count = 24000;
    std::vector<cfloat> input(count);
    std::vector<float> output(count);
    double phase = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double t = static_cast<double>(i) / kRate;
        phase += 2.0 * M_PI * (700.0 + 1200.0 * std::cos(2.0 * M_PI * 400.0 * t)) / kRate;
        input[i] = cfloat(static_cast<float>(0.3 * std::cos(phase)), static_cast<float>(0.3 * std::sin(phase)));
    }
    Demodulator demod;
    demod.configure(Mode::Nfm, kRate);
    demod.process(input.data(), count, output.data());
    double mean = 0.0;
    for (size_t i = count / 2; i < count; i++) mean += output[i];
    mean /= count / 2;
    CHECK(std::abs(mean) < 1e-4);
    CHECK_NEAR(amplitude_at(output, 400.0, count / 2, count / 2), 0.07948, 0.002);
}

TEST_CASE(demod_fm_recovers_a_tone) {
    const size_t n = 24000;
    const double deviation = 2000.0;
    const double tone = 500.0;
    std::vector<cfloat> in(n);
    double phase = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / kRate;
        const double instantaneous = deviation * std::cos(2.0 * M_PI * tone * t);
        phase += 2.0 * M_PI * instantaneous / kRate;
        in[i] = cfloat(static_cast<float>(0.5 * std::cos(phase)), static_cast<float>(0.5 * std::sin(phase)));
    }
    Demodulator d;
    d.configure(Mode::Nfm, kRate);
    std::vector<float> out(n);
    d.process(in.data(), n, out.data());
    // Non-zero, bounded, and concentrated at the modulating frequency.
    const double at_tone = amplitude_at(out, tone, 4000, 16000);
    CHECK_NEAR(at_tone, 0.12090, 0.002);
    CHECK(at_tone > 5.0 * amplitude_at(out, 1700.0, 4000, 16000));
}

TEST_CASE(demod_fm_deemphasis_choice_sets_the_treble_rolloff) {
    // The same deviation at 300 Hz and at 3 kHz: the discriminator is flat, so
    // whatever separates the two is the de-emphasis, one pole at 1 / (2 pi tau).
    const auto level = [](double tau_us, double tone) {
        const size_t n = 24000;
        std::vector<cfloat> in(n);
        double phase = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double instantaneous = 1000.0 * std::cos(2.0 * M_PI * tone * static_cast<double>(i) / kRate);
            phase += 2.0 * M_PI * instantaneous / kRate;
            in[i] = cfloat(static_cast<float>(0.5 * std::cos(phase)), static_cast<float>(0.5 * std::sin(phase)));
        }
        Demodulator d;
        d.set_deemphasis(tau_us);
        d.configure(Mode::Nfm, kRate);
        std::vector<float> out(n);
        d.process(in.data(), n, out.data());
        return amplitude_at(out, tone, 6000, 12000);
    };
    // What a one-pole with that time constant does at `hz`, sampled at kRate.
    const auto pole = [](double tau_us, double hz) {
        if (tau_us <= 0.0) return 1.0;
        const double alpha = 1.0 - std::exp(-1.0 / (kRate * tau_us * 1e-6));
        const double w = 2.0 * M_PI * hz / kRate;
        const double re = 1.0 - (1.0 - alpha) * std::cos(w), im = (1.0 - alpha) * std::sin(w);
        return alpha / std::sqrt(re * re + im * im);
    };
    for (double tau : {0.0, 300.0, 750.0}) {
        const double measured = 20.0 * std::log10(level(tau, 3000.0) / level(tau, 300.0));
        const double expected = 20.0 * std::log10(pole(tau, 3000.0) / pole(tau, 300.0));
        CHECK_NEAR(measured, expected, 0.3);
    }
    // Off is flat; 750 us, the land-mobile standard, rolls off a good deal more than 300 us.
    CHECK_NEAR(20.0 * std::log10(level(0.0, 3000.0) / level(0.0, 300.0)), 0.0, 0.3);
    CHECK(level(750.0, 3000.0) / level(750.0, 300.0) < 0.8 * level(300.0, 3000.0) / level(300.0, 300.0));
}

TEST_CASE(demod_mode_names_round_trip) {
    for (const char* name : {"usb", "lsb", "cw", "cwl", "am", "sam", "nfm", "dsb"}) {
        Mode mode;
        CHECK(fernsdr::mode_from_name(name, mode));
        CHECK(std::string(fernsdr::mode_name(mode)) == name);
    }
    Mode unused;
    CHECK(!fernsdr::mode_from_name("not-a-mode", unused));
}

TEST_CASE(demod_cw_passband_sits_on_the_sidetone_pitch) {
    auto upper = fernsdr::cw_passband(Mode::Cw, 700.0, 400.0);
    CHECK_NEAR(upper.low, 500.0, 1e-9);
    CHECK_NEAR(upper.high, 900.0, 1e-9);
    // Lower-side CW is the mirror image, so the operator hears the same pitch.
    auto lower = fernsdr::cw_passband(Mode::CwL, 700.0, 400.0);
    CHECK_NEAR(lower.low, -900.0, 1e-9);
    CHECK_NEAR(lower.high, -500.0, 1e-9);
}

// --- AGC --------------------------------------------------------------------

namespace {

// Builds a complex baseband tone at `freq` whose amplitude follows `envelope`.
std::vector<cfloat> complex_tone(size_t n, double freq, const std::function<double(size_t)>& envelope) {
    std::vector<cfloat> v(n);
    for (size_t i = 0; i < n; i++) {
        const double a = 2.0 * M_PI * freq * static_cast<double>(i) / kRate;
        const double amplitude = envelope(i);
        v[i] = cfloat(static_cast<float>(amplitude * std::cos(a)),
                      static_cast<float>(amplitude * std::sin(a)));
    }
    return v;
}

double peak_magnitude(const std::vector<cfloat>& x, size_t start, size_t count) {
    double peak = 0.0;
    for (size_t i = start; i < std::min(x.size(), start + count); i++) {
        peak = std::max(peak, static_cast<double>(std::abs(x[i])));
    }
    return peak;
}

double rms_magnitude(const std::vector<cfloat>& x, size_t start, size_t count) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = start; i < std::min(x.size(), start + count); i++) {
        acc += static_cast<double>(std::norm(x[i]));
        n++;
    }
    return n ? std::sqrt(acc / n) : 0.0;
}

}  // namespace

TEST_CASE(agc_brings_widely_different_levels_to_a_similar_output) {
    auto run_at = [](float amplitude) {
        fernsdr::Agc agc;
        agc.configure(kRate);
        agc.set_profile(fernsdr::AgcProfile::Medium);
        auto x = complex_tone(static_cast<size_t>(kRate * 3), 800.0, [&](size_t) { return amplitude; });
        agc.process(x.data(), x.size());
        return rms_magnitude(x, x.size() / 2, x.size() / 2);
    };

    const double loud = run_at(0.5f);
    const double quiet = run_at(0.002f);
    // 48 dB of input range collapses to a couple of dB of output range.
    CHECK(std::fabs(20.0 * std::log10(loud / quiet)) < 3.0);
}

TEST_CASE(agc_does_not_overshoot_on_a_sudden_strong_signal) {
    // The property look-ahead exists for, and the one that separates an AGC
    // that sounds like a receiver from one that pops on every loud station.
    //
    // A quiet signal steps up by 40 dB. Without look-ahead the gain is still
    // high when the loud signal arrives and the output blows past its target
    // for as long as the attack takes. With it, the gain is already down.
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Slow);

    const size_t n = static_cast<size_t>(kRate * 2);
    const size_t step = n / 2;
    auto x = complex_tone(n, 800.0, [&](size_t i) { return i < step ? 0.004 : 0.4; });
    agc.process(x.data(), x.size());

    // Steady-state level well before the step.
    const double settled = rms_magnitude(x, step - 4000, 3000);
    // The worst excursion anywhere across the transition.
    const double overshoot = peak_magnitude(x, step - 200, 3000);

    CHECK(settled > 0.05);
    // A tone's peak is sqrt(2) above its RMS; anything beyond that is the AGC
    // failing to keep up.
    CHECK(overshoot < settled * 1.6);
}

TEST_CASE(agc_gain_does_not_ripple_at_audio_rate) {
    // Detecting on the real audio instead of the complex envelope makes the
    // gain wobble at twice the tone frequency, which is distortion. The
    // envelope of a steady tone must come out steady.
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Medium);

    auto x = complex_tone(static_cast<size_t>(kRate), 700.0, [](size_t) { return 0.05; });
    agc.process(x.data(), x.size());

    const size_t start = x.size() / 2;
    double minimum = 1e9, maximum = 0.0;
    for (size_t i = start; i < x.size(); i++) {
        const double magnitude = std::abs(x[i]);
        minimum = std::min(minimum, magnitude);
        maximum = std::max(maximum, magnitude);
    }
    // Under a fifth of a dB across half a second. Detecting on the real
    // audio instead would give several dB of ripple at twice the tone frequency.
    CHECK(20.0 * std::log10(maximum / std::max(minimum, 1e-12)) < 0.2);
}

TEST_CASE(agc_holds_gain_through_a_gap_rather_than_pumping) {
    // Between syllables the gain must stay put, or the noise floor surges up
    // and back down and the receiver sounds like it is breathing.
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Slow);

    const size_t n = static_cast<size_t>(kRate * 3);
    // Speech-like: 200 ms of signal, 150 ms of quiet noise, repeating.
    auto x = complex_tone(n, 900.0, [&](size_t i) {
        const size_t phase = i % static_cast<size_t>(kRate * 0.35);
        return phase < static_cast<size_t>(kRate * 0.2) ? 0.2 : 0.0015;
    });
    agc.process(x.data(), x.size());

    // Noise level during two different gaps, late enough to have settled.
    const size_t period = static_cast<size_t>(kRate * 0.35);
    const size_t gap_start = period * 6 + static_cast<size_t>(kRate * 0.25);
    const double gap = rms_magnitude(x, gap_start, static_cast<size_t>(kRate * 0.05));
    const double signal = rms_magnitude(x, period * 6 + static_cast<size_t>(kRate * 0.1),
                                        static_cast<size_t>(kRate * 0.05));

    // The noise in the gap must stay far below the signal: the AGC held its
    // gain instead of winding up during the pause.
    CHECK(20.0 * std::log10(signal / std::max(gap, 1e-12)) > 25.0);
}

TEST_CASE(agc_off_applies_manual_gain_exactly) {
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Off);
    agc.set_manual_gain_db(6.0f);
    std::vector<cfloat> x(64, cfloat(0.1f, 0.0f));
    agc.process(x.data(), x.size());
    for (const auto& v : x) CHECK_NEAR(v.real(), 0.1 * std::pow(10.0, 6.0 / 20.0), 1e-4);
}

TEST_CASE(agc_respects_the_maximum_gain) {
    // Which is also the threshold: a dead band must come up as quiet noise,
    // not as full-scale hiss.
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Fast);
    agc.set_max_gain_db(20.0f);

    auto x = complex_tone(static_cast<size_t>(kRate), 500.0, [](size_t) { return 1e-6; });
    agc.process(x.data(), x.size());
    CHECK(agc.gain_db() <= 20.5f);
    CHECK(rms_magnitude(x, x.size() / 2, x.size() / 2) < 1e-4);
}

TEST_CASE(agc_reports_the_input_level_not_its_own_target) {
    // An S-meter fed from after the AGC reads the gain control, not the
    // antenna, and would show every station at the same strength.
    for (double amplitude : {0.5, 0.05, 0.005}) {
        fernsdr::Agc agc;
        agc.configure(kRate);
        agc.set_profile(fernsdr::AgcProfile::Medium);
        auto x = complex_tone(4096, 800.0, [&](size_t) { return amplitude; });
        agc.process(x.data(), x.size());
        CHECK_NEAR(agc.input_dbfs(), 20.0 * std::log10(amplitude), 0.2);
    }
}

// --- noise blanker ----------------------------------------------------------
//
// The blanker runs on a band's raw wideband input, before any channel
// filtering. That placement is the whole design: an impulse is broadband and
// microseconds long, and a 2.4 kHz channel filter smears it across
// milliseconds until it is indistinguishable from a speech peak. These tests
// therefore feed it wideband noise, which is what it actually sees.

namespace {

constexpr double kWideRate = 1536000.0;

// Wideband noise, as a quiet band looks to the front end.
std::vector<cfloat> wideband_noise(size_t n, float sigma, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> gaussian(0.0f, sigma);
    std::vector<cfloat> v(n);
    // One draw at a time, imaginary part first as GCC on x86-64 did when this
    // was an argument list, whose order C++ leaves open.
    for (auto& s : v) {
        const float im = gaussian(rng);
        const float re = gaussian(rng);
        s = cfloat(re, im);
    }
    return v;
}

}  // namespace

TEST_CASE(noise_blanker_removes_impulses_and_leaves_the_noise_floor) {
    const size_t n = static_cast<size_t>(kWideRate * 0.5);
    auto clean = wideband_noise(n, 0.02f, 5);

    auto noisy = clean;
    // An impulse every millisecond, about 40 dB above the noise: power-line
    // arcing looks like this.
    const size_t period = static_cast<size_t>(kWideRate * 0.001);
    for (size_t i = period; i < n; i += period) {
        noisy[i] += cfloat(2.0f, 1.0f);
        noisy[i + 1] += cfloat(1.0f, -0.6f);
    }

    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    blanker.set_strength(0.7f);
    auto blanked = noisy;
    blanker.process(blanked.data(), blanked.size());

    const size_t start = n / 2;
    const size_t count = n / 2;
    // The impulses are gone.
    CHECK(peak_magnitude(blanked, start, count) < peak_magnitude(noisy, start, count) * 0.1);
    // And the noise floor is essentially untouched: the blanker removed the
    // impulses, not the band.
    const double before = rms_magnitude(clean, start, count);
    const double after = rms_magnitude(blanked, start, count);
    CHECK(after > before * 0.9);
    CHECK(after < before * 1.1);
}

TEST_CASE(noise_blanker_leaves_a_clean_band_alone) {
    // Nothing to blank means nothing blanked. A blanker that nibbles at an
    // undisturbed band is worse than none at all.
    const size_t n = static_cast<size_t>(kWideRate * 0.5);
    auto x = wideband_noise(n, 0.02f, 9);
    auto original = x;

    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    blanker.set_strength(1.0f);  // the most aggressive setting on offer
    blanker.process(x.data(), x.size());

    const double before = rms_magnitude(original, n / 2, n / 2);
    const double after = rms_magnitude(x, n / 2, n / 2);
    CHECK(after > before * 0.995);
    CHECK(blanker.blanked_fraction() < 0.005);
}

TEST_CASE(noise_blanker_leaves_a_strong_carrier_alone) {
    // A loud station is not an impulse. The reference follows the band's own
    // level, so a strong steady signal simply raises it.
    const size_t n = static_cast<size_t>(kWideRate * 0.5);
    auto x = wideband_noise(n, 0.02f, 11);
    for (size_t i = 0; i < n; i++) {
        const double a = 2.0 * M_PI * 50000.0 * static_cast<double>(i) / kWideRate;
        x[i] += cfloat(static_cast<float>(0.4 * std::cos(a)), static_cast<float>(0.4 * std::sin(a)));
    }
    auto original = x;

    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    blanker.set_strength(1.0f);
    blanker.process(x.data(), x.size());

    CHECK_NEAR(rms_magnitude(x, n / 2, n / 2), rms_magnitude(original, n / 2, n / 2), 0.01);
    CHECK(blanker.blanked_fraction() < 0.005);
}

TEST_CASE(noise_blanker_handles_real_front_ends) {
    const size_t n = static_cast<size_t>(kWideRate * 0.5);
    std::mt19937 rng(13);
    std::normal_distribution<float> gaussian(0.0f, 0.02f);
    std::vector<float> x(n);
    for (auto& v : x) v = gaussian(rng);
    auto clean = x;

    const size_t period = static_cast<size_t>(kWideRate * 0.001);
    for (size_t i = period; i < n; i += period) x[i] += 2.0f;

    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    blanker.set_strength(0.7f);
    blanker.process_real(x.data(), x.size());

    double peak = 0.0, reference_rms = 0.0, blanked_rms = 0.0;
    for (size_t i = n / 2; i < n; i++) {
        peak = std::max<double>(peak, std::fabs(x[i]));
        reference_rms += static_cast<double>(clean[i]) * clean[i];
        blanked_rms += static_cast<double>(x[i]) * x[i];
    }
    CHECK(peak < 0.5);
    CHECK(std::sqrt(blanked_rms) > std::sqrt(reference_rms) * 0.9);
}

TEST_CASE(noise_blanker_recovers_if_its_reference_starts_low) {
    // A trap worth a test of its own. If blanked samples are excluded from the
    // reference update, a reference that starts too low blanks everything,
    // which stops it updating, and the band is silenced permanently. Starting
    // from silence and stepping straight into a loud band must recover.
    const size_t n = static_cast<size_t>(kWideRate * 0.2);
    std::vector<cfloat> x(n, cfloat(0.0f, 0.0f));
    auto loud = wideband_noise(n, 0.2f, 17);
    for (size_t i = n / 10; i < n; i++) x[i] = loud[i];

    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    blanker.set_strength(1.0f);
    blanker.process(x.data(), x.size());

    // Well after the step, the band is passing normally again.
    CHECK(rms_magnitude(x, n * 3 / 4, n / 4) > rms_magnitude(loud, n * 3 / 4, n / 4) * 0.9);
}

TEST_CASE(noise_blanker_is_a_true_bypass_when_disabled) {
    fernsdr::NoiseBlanker blanker;
    blanker.configure(kWideRate);
    CHECK(!blanker.active());
    auto x = wideband_noise(512, 0.1f, 3);
    auto original = x;
    blanker.process(x.data(), x.size());
    for (size_t i = 0; i < x.size(); i++) CHECK_EQ(x[i] == original[i], 1);
}

TEST_CASE(audio_post_is_a_true_bypass_when_nothing_is_enabled) {
    // The default path must not be delayed or altered at all.
    AudioPost post;
    post.configure(kRate);
    CHECK(!post.active());

    std::vector<float> x(fernsdr::kPostHop);
    for (size_t i = 0; i < x.size(); i++) x[i] = static_cast<float>(std::sin(i * 0.3));
    auto original = x;
    post.process(x.data());
    for (size_t i = 0; i < x.size(); i++) CHECK_EQ(x[i] == original[i], 1);
}

TEST_CASE(audio_post_manual_notch_removes_a_carrier_and_keeps_the_rest) {
    AudioPost post;
    post.configure(kRate);
    post.set_notches({{1500.0, 150.0}});
    CHECK(post.active());

    const size_t n = fernsdr::kPostHop * 80;
    std::vector<float> x(n);
    for (size_t i = 0; i < n; i++) {
        x[i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 1500.0 * i / kRate) +
                                  0.3 * std::sin(2.0 * M_PI * 800.0 * i / kRate));
    }
    std::vector<float> y = x;
    for (size_t off = 0; off + fernsdr::kPostHop <= n; off += fernsdr::kPostHop) post.process(y.data() + off);

    const size_t start = fernsdr::kPostHop * 10;
    const size_t count = fernsdr::kPostHop * 60;
    const double heterodyne = amplitude_at(y, 1500.0, start, count);
    const double wanted = amplitude_at(y, 800.0, start, count);

    CHECK(heterodyne < 0.3 * 0.1);   // notched by better than 20 dB
    CHECK(wanted > 0.3 * 0.8);       // the signal either side is untouched
}

TEST_CASE(audio_post_noise_reduction_keeps_signal_and_removes_noise) {
    // The real use case: an intermittent signal (speech, CW elements) in a
    // steady noise floor.  The minimum-statistics estimator learns the floor
    // from the gaps, so the wanted signal must come through essentially
    // untouched while the gaps go quiet.
    AudioPost post;
    post.configure(kRate);
    post.set_noise_reduction(1.0f);

    const size_t n = fernsdr::kPostHop * 600;
    const size_t burst_len = 3000;  // 250 ms on, 250 ms off
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.05f);

    std::vector<float> mixed(n);
    for (size_t i = 0; i < n; i++) {
        const bool keyed = ((i / burst_len) % 2) == 0;
        const double tone = keyed ? 0.25 * std::sin(2.0 * M_PI * 900.0 * i / kRate) : 0.0;
        mixed[i] = static_cast<float>(tone) + noise(rng);
    }

    std::vector<float> processed = mixed;
    for (size_t off = 0; off + fernsdr::kPostHop <= n; off += fernsdr::kPostHop) post.process(processed.data() + off);

    // Well after the estimator has settled: one burst and the gap after it.
    const size_t burst = burst_len * 20;
    const size_t gap = burst_len * 21;
    auto window_rms = [&](const std::vector<float>& v, size_t start) {
        double acc = 0.0;
        for (size_t i = start + 300; i < start + 2700; i++) acc += static_cast<double>(v[i]) * v[i];
        return std::sqrt(acc / 2400.0);
    };

    const double tone_before = amplitude_at(mixed, 900.0, burst + 300, 2400);
    const double tone_after = amplitude_at(processed, 900.0, burst + 300, 2400);
    CHECK(tone_after > tone_before * 0.9);  // signal survives within ~1 dB

    const double noise_reduction_db = 20.0 * std::log10(window_rms(mixed, gap) / window_rms(processed, gap));
    CHECK(noise_reduction_db > 8.0);
}

TEST_CASE(audio_post_noise_reduction_partial_strength_is_gentler) {
    auto measure = [](float strength) {
        AudioPost post;
        post.configure(kRate);
        post.set_noise_reduction(strength);
        const size_t n = fernsdr::kPostHop * 300;
        std::mt19937 rng(9);
        std::normal_distribution<float> noise(0.0f, 0.05f);
        std::vector<float> x(n);
        for (auto& v : x) v = noise(rng);
        for (size_t off = 0; off + fernsdr::kPostHop <= n; off += fernsdr::kPostHop) post.process(x.data() + off);
        return rms(x, n / 2);
    };
    // More strength must mean more suppression, monotonically.
    CHECK(measure(1.0f) < measure(0.5f));
    CHECK(measure(0.5f) < measure(0.0f) * 1.001);
}

// Re-applying the AGC profile the receiver already has must not disturb the
// audio. `Listener::apply` calls set_profile on every channel change - a
// filter, a noise-reduction slider, a frequency drag sending one message per
// frame - and rebuilding wiped the look-ahead delay line, punching a hole the
// length of the look-ahead into the stream. That was heard as a click on every
// adjustment and a continuous crackle while dragging.
TEST_CASE(agc_reapplying_the_same_profile_does_not_break_the_audio) {
    fernsdr::Agc agc;
    agc.configure(kRate);
    agc.set_profile(fernsdr::AgcProfile::Slow);

    // An analytic tone: constant magnitude, so a zeroed delay sample shows up
    // directly as a dropout instead of hiding in a zero crossing.
    const size_t kBlock = 512;
    size_t index = 0;
    auto tone = [&]() {
        std::vector<cfloat> x(kBlock);
        for (size_t i = 0; i < kBlock; i++, index++) {
            const double t = 2.0 * M_PI * 800.0 * static_cast<double>(index) / kRate;
            x[i] = cfloat(static_cast<float>(0.2 * std::cos(t)), static_cast<float>(0.2 * std::sin(t)));
        }
        agc.process(x.data(), kBlock);
        return x;
    };

    for (size_t b = 0; b < 40; b++) tone();  // let the gain settle

    const auto reference = tone();
    double floor_db = 0.0;
    for (const auto& s : reference) floor_db = std::max(floor_db, static_cast<double>(std::abs(s)));
    const double quiet = floor_db * 0.25;

    agc.set_profile(fernsdr::AgcProfile::Slow);  // the call that used to click
    const auto after = tone();

    size_t dropped = 0;
    for (const auto& s : after) {
        if (static_cast<double>(std::abs(s)) < quiet) dropped++;
    }
    CHECK(dropped == 0);
}
