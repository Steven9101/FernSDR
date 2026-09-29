#include "../src/dsp/biquad.h"
#include "../src/dsp/input_conditioner.h"
#include "test_util.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::Biquad;
using fernsdr::cfloat;
using fernsdr::InputConditioner;

namespace {

constexpr double kRate = 240000.0;
constexpr size_t kBlock = 2400;  // 10 ms, as a band's channelizer takes it
constexpr double kTwoPi = 6.283185307179586;

/**
 * A tone at `hz`, with noise, as the front end would deliver it: the Q arm
 * `amplitude` times as strong as I and `phase` radians away from quadrature,
 * and a DC offset on each arm.
 */
struct FrontEnd {
    double hz = 15000.0;
    double tone = 0.2;
    double noise = 0.0;
    double amplitude = 1.0;
    double phase = 0.0;
    cfloat dc{0.0f, 0.0f};
    size_t sample = 0;
    std::mt19937 rng{7};

    void fill(std::vector<cfloat>& block) {
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (cfloat& value : block) {
            const double t = kTwoPi * hz * static_cast<double>(sample++) / kRate;
            const double i0 = tone * std::cos(t) + noise * gauss(rng);
            const double q0 = tone * std::sin(t) + noise * gauss(rng);
            const double q = amplitude * (q0 * std::cos(phase) + i0 * std::sin(phase));
            value = cfloat(static_cast<float>(i0) + dc.real(), static_cast<float>(q) + dc.imag());
        }
    }
};

/** The amplitude of the component at `hz` in `block`, whose first sample is number `first`. */
double component(const std::vector<cfloat>& block, double hz, size_t first) {
    std::complex<double> sum(0.0, 0.0);
    for (size_t k = 0; k < block.size(); k++) {
        const double t = -kTwoPi * hz * static_cast<double>(first + k) / kRate;
        sum += std::complex<double>(block[k].real(), block[k].imag()) * std::complex<double>(std::cos(t), std::sin(t));
    }
    return std::abs(sum) / static_cast<double>(block.size());
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST_CASE(input_dc_removal_takes_out_the_offset_and_leaves_the_signal) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_dc_remove(true);
    FrontEnd front;
    front.hz = 20.0;  // a carrier 20 Hz from the centre must survive the tracker
    front.tone = 0.1;
    front.dc = cfloat(0.05f, -0.03f);
    std::vector<cfloat> block(kBlock);
    // Two seconds, then a whole second measured: 20 Hz needs whole cycles.
    std::vector<cfloat> last;
    size_t first = 0;
    for (int n = 0; n < 300; n++) {
        const size_t start = front.sample;
        front.fill(block);
        conditioner.process(block.data(), block.size());
        if (n >= 200) {
            if (last.empty()) first = start;
            last.insert(last.end(), block.begin(), block.end());
        }
    }
    const double offset = std::abs(std::complex<double>(0.05, -0.03));
    CHECK(component(last, 0.0, first) < offset * 0.01);  // at least 40 dB down
    CHECK_NEAR(db(component(last, 20.0, first) / 0.1), 0.0, 0.1);
    CHECK_NEAR(conditioner.dc_offset_dbfs(), 20.0 * std::log10(offset), 0.5);
}

TEST_CASE(input_dc_removal_starts_at_once_and_forgets_when_switched_off) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_dc_remove(true);
    FrontEnd front;
    front.dc = cfloat(0.1f, 0.1f);
    std::vector<cfloat> block(kBlock);
    front.fill(block);
    conditioner.process(block.data(), block.size());
    // The first block is corrected by its own mean: no spike while it settles.
    CHECK(component(block, 0.0, 0) < 0.001);
    conditioner.set_dc_remove(false);
    front.fill(block);
    conditioner.process(block.data(), block.size());
    CHECK_NEAR(component(block, 0.0, 0), std::abs(std::complex<double>(0.1, 0.1)), 0.002);
    CHECK_EQ(conditioner.dc_offset_dbfs(), -160.0f);
}

TEST_CASE(input_dc_removal_on_a_real_input) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_dc_remove(true);
    std::vector<float> block(kBlock);
    size_t sample = 0;
    double mean = 0.0, tone = 0.0;
    for (int n = 0; n < 100; n++) {
        for (float& value : block) {
            value = 0.2f + 0.1f * static_cast<float>(std::cos(kTwoPi * 3000.0 * static_cast<double>(sample++) / kRate));
        }
        conditioner.process_real(block.data(), block.size());
    }
    for (size_t k = 0; k < block.size(); k++) {
        mean += block[k];
        tone += block[k] * std::cos(kTwoPi * 3000.0 * static_cast<double>(sample - block.size() + k) / kRate);
    }
    CHECK(std::fabs(mean / block.size()) < 0.002);
    CHECK_NEAR(2.0 * tone / block.size(), 0.1, 0.001);
}

TEST_CASE(input_balance_restores_image_rejection) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    FrontEnd front;
    front.hz = 15000.0;
    front.tone = 0.2;
    front.noise = 0.02;
    front.amplitude = 1.08;           // 0.7 dB
    front.phase = 4.0 * kTwoPi / 360;  // 4 degrees
    std::vector<cfloat> block(kBlock);

    // Uncorrected: the mirror image sits about 26 dB under the tone.
    front.fill(block);
    const double before = db(component(block, 15000.0, 0) / component(block, -15000.0, 0));
    CHECK(before > 24.0 && before < 28.0);

    conditioner.set_balance(true);
    double after = 0.0;
    for (int n = 0; n < 600; n++) {  // six seconds
        const size_t first = front.sample;
        front.fill(block);
        conditioner.process(block.data(), block.size());
        if (n == 599) after = db(component(block, 15000.0, first) / component(block, -15000.0, first));
    }
    CHECK(after - before >= 20.0);
    // And it says what it found, close to what was put in.
    CHECK_NEAR(conditioner.gain_error_db(), db(1.08), 0.05);
    CHECK_NEAR(conditioner.phase_error_degrees(), 4.0, 0.2);
    CHECK_NEAR(conditioner.image_rejection_db(), before, 1.0);
}

TEST_CASE(input_balance_leaves_a_balanced_input_alone) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_balance(true);
    FrontEnd front;
    front.noise = 0.05;
    std::vector<cfloat> block(kBlock);
    double image = 0.0, tone = 0.0;
    for (int n = 0; n < 300; n++) {
        const size_t first = front.sample;
        front.fill(block);
        conditioner.process(block.data(), block.size());
        if (n == 299) {
            tone = component(block, 15000.0, first);
            image = component(block, -15000.0, first);
        }
    }
    CHECK_NEAR(db(tone / 0.2), 0.0, 0.1);
    CHECK(db(tone / image) > 40.0);
}

TEST_CASE(input_balance_starts_again_when_the_arms_are_swapped) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_balance(true);
    FrontEnd front;
    front.noise = 0.02;
    front.amplitude = 1.08;
    front.phase = 4.0 * kTwoPi / 360;
    std::vector<cfloat> block(kBlock);
    for (int n = 0; n < 300; n++) {
        front.fill(block);
        conditioner.process(block.data(), block.size());
    }
    // Swapped, the tone sits at -15 kHz and its image at +15 kHz. The very
    // first block must not come out worse than no correction would.
    conditioner.set_swap(true);
    const size_t first = front.sample;
    front.fill(block);
    std::vector<cfloat> raw = block;
    for (cfloat& value : raw) value = cfloat(value.imag(), value.real());
    const double uncorrected = db(component(raw, -15000.0, first) / component(raw, 15000.0, first));
    conditioner.process(block.data(), block.size());
    const double corrected = db(component(block, -15000.0, first) / component(block, 15000.0, first));
    CHECK(corrected >= uncorrected - 0.5);
}

TEST_CASE(input_corrections_handle_blocks_of_any_length) {
    // The lanes take samples two at a time and four floats at a time; every
    // other length leaves a tail for the scalar loop.
    for (size_t length : {size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{7}, size_t{63}, size_t{65}, size_t{1023}, size_t{1025}}) {
        InputConditioner conditioner;
        conditioner.configure(kRate);
        conditioner.set_dc_remove(true);
        conditioner.set_balance(true);
        FrontEnd front;
        front.noise = 0.02;
        front.amplitude = 1.08;
        front.phase = 4.0 * kTwoPi / 360;
        front.dc = cfloat(0.05f, -0.03f);
        std::vector<cfloat> block(length);
        const size_t blocks = static_cast<size_t>(4.0 * kRate) / length;
        std::vector<cfloat> tail;
        size_t first = 0;
        for (size_t n = 0; n < blocks; n++) {
            const size_t start = front.sample;
            front.fill(block);
            conditioner.process(block.data(), block.size());
            if (front.sample + static_cast<size_t>(kRate / 10) >= blocks * length) {
                if (tail.empty()) first = start;
                tail.insert(tail.end(), block.begin(), block.end());
            }
        }
        CHECK(component(tail, 0.0, first) < 0.001);
        CHECK(db(component(tail, 15000.0, first) / component(tail, -15000.0, first)) > 40.0);
    }
}

TEST_CASE(input_swap_mirrors_the_spectrum) {
    InputConditioner conditioner;
    conditioner.configure(kRate);
    conditioner.set_swap(true);
    FrontEnd front;
    std::vector<cfloat> block(kBlock);
    front.fill(block);
    conditioner.process(block.data(), block.size());
    CHECK_NEAR(component(block, -15000.0, 0), 0.2, 0.001);
    CHECK(component(block, 15000.0, 0) < 0.001);
}

TEST_CASE(audio_highpass_has_its_corner_at_the_cutoff_and_falls_12_db_an_octave) {
    Biquad filter;
    filter.set_highpass(12000.0, 300.0);
    CHECK(filter.active());
    CHECK_NEAR(db(filter.magnitude(12000.0, 300.0)), -3.01, 0.05);
    CHECK(db(filter.magnitude(12000.0, 75.0)) < -23.0);    // two octaves down
    CHECK(db(filter.magnitude(12000.0, 1200.0)) > -0.3);   // two octaves up
    CHECK(db(filter.magnitude(12000.0, 3000.0)) > -0.05);

    // The samples do what the coefficients say: a 150 Hz tone, once settled.
    std::vector<float> audio(12000);
    for (size_t k = 0; k < audio.size(); k++) audio[k] = static_cast<float>(std::sin(kTwoPi * 150.0 * k / 12000.0));
    filter.process(audio.data(), audio.size());
    double peak = 0.0;
    for (size_t k = 6000; k < audio.size(); k++) peak = std::max(peak, static_cast<double>(std::fabs(audio[k])));
    CHECK_NEAR(db(peak), db(filter.magnitude(12000.0, 150.0)), 0.1);

    // Hum taken out entirely: DC in, nothing out once settled.
    std::vector<float> constant(12000, 0.5f);
    filter.process(constant.data(), constant.size());
    CHECK(std::fabs(constant.back()) < 1e-4);
}

TEST_CASE(audio_highpass_off_passes_everything) {
    Biquad filter;
    filter.set_highpass(12000.0, 0.0);
    CHECK(!filter.active());
    std::vector<float> audio = {0.5f, -0.25f, 0.125f};
    filter.process(audio.data(), audio.size());
    CHECK_EQ(audio[0], 0.5f);
    CHECK_EQ(audio[2], 0.125f);
    filter.set_highpass(12000.0, 7000.0);  // above Nyquist: not a filter
    CHECK(!filter.active());
}

TEST_CASE(audio_highpass_changes_while_playing_without_a_click) {
    // A listener switches the filter on, moves its cutoff and switches it off
    // while audio plays. A click is one sample far from the last; a clean tone
    // never moves more than 2 A sin(pi f / rate) from one to the next. Hum and
    // the bottom of a voice, near the cutoffs, are where a click shows most.
    constexpr double rate = 12000.0;
    constexpr double amplitude = 0.5;
    for (double tone : {60.0, 150.0, 300.0, 1000.0}) {
        Biquad filter;
        double last = 0.0, worst = 0.0;
        std::vector<float> block(240);
        size_t sample = 0;
        for (int n = 0; n < 70; n++) {
            if (n == 10) filter.set_highpass(rate, 100.0);   // switched on
            if (n == 20) filter.set_highpass(rate, 1000.0);  // to the top of the range
            if (n == 30) filter.set_highpass(rate, 50.0);    // and down again
            if (n == 40) filter.set_highpass(rate, 300.0);
            if (n == 50) filter.set_highpass(rate, 0.0);     // switched off
            if (n == 60) filter.set_highpass(rate, 300.0);   // the same again: nothing to fade
            for (float& value : block) {
                value = static_cast<float>(amplitude * std::sin(kTwoPi * tone * static_cast<double>(sample++) / rate));
            }
            filter.process(block.data(), block.size());
            for (float value : block) {
                worst = std::max(worst, static_cast<double>(std::fabs(value - last)));
                last = value;
            }
        }
        const double clean = 2.0 * amplitude * std::sin(kTwoPi / 2.0 * tone / rate);
        CHECK(worst < clean * 1.1);
    }

    // Once the fade is over, the new cutoff is all there is.
    Biquad moved;
    moved.set_highpass(rate, 100.0);
    std::vector<float> audio(2400, 0.25f);
    moved.process(audio.data(), audio.size());
    moved.set_highpass(rate, 400.0);
    std::vector<float> tone(6000);
    for (size_t k = 0; k < tone.size(); k++) tone[k] = static_cast<float>(std::sin(kTwoPi * 400.0 * k / rate));
    moved.process(tone.data(), tone.size());
    double peak = 0.0;
    for (size_t k = 3000; k < tone.size(); k++) peak = std::max(peak, static_cast<double>(std::fabs(tone[k])));
    CHECK_NEAR(db(peak), -3.01, 0.05);
}
