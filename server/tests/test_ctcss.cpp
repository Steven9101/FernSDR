#include "../src/dsp/ctcss.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

using fernsdr::CtcssDetector;

namespace {

constexpr double kPi = 3.14159265358979323846;

// What an NFM discriminator gives for a user with a tone: the tone at about
// a tenth of full voice, speech-like sound above the transmitter's 300 Hz
// cut, and noise. `seconds` of it at `rate`.
std::vector<float> nfm_audio(double rate, double seconds, double tone_hz, double tone_level, uint32_t seed,
                             double voice_level = 0.5, double noise_level = 0.05) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const size_t n = static_cast<size_t>(rate * seconds);
    std::vector<float> out(n);
    const double formants[] = {420.0, 730.0, 1090.0, 2440.0};
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / rate;
        double voice = 0.0;
        for (double f : formants) voice += std::sin(2.0 * kPi * f * t + f) * (0.5 + 0.5 * std::sin(2.0 * kPi * 3.1 * t + f));
        out[i] = static_cast<float>(tone_level * std::sin(2.0 * kPi * tone_hz * t) + voice_level * voice / 4.0 +
                                    noise_level * gauss(rng));
    }
    return out;
}

double run(double rate, const std::vector<float>& audio) {
    CtcssDetector detector;
    detector.configure(rate);
    // In the listener's block sizes, not in one call.
    for (size_t i = 0; i < audio.size(); i += 240) detector.process(audio.data() + i, std::min<size_t>(240, audio.size() - i));
    return detector.tone_hz();
}

}  // namespace

TEST_CASE(ctcss_names_each_standard_tone_at_every_audio_rate) {
    for (double rate : {8000.0, 12000.0, 24000.0, 48000.0}) {
        for (double tone : CtcssDetector::standard_tones()) {
            const double found = run(rate, nfm_audio(rate, 3.5, tone, 0.1, static_cast<uint32_t>(tone * 10)));
            CHECK_EQ(found, tone);
        }
    }
}

TEST_CASE(ctcss_separates_the_closest_pair_and_forgives_a_slightly_off_encoder) {
    CHECK_EQ(run(12000.0, nfm_audio(12000.0, 3.5, 165.5, 0.1, 1)), 165.5);
    CHECK_EQ(run(12000.0, nfm_audio(12000.0, 3.5, 167.9, 0.1, 2)), 167.9);
    // An encoder 0.8 Hz high is still 100.0; one halfway between two tones
    // is not guessed at.
    CHECK_EQ(run(12000.0, nfm_audio(12000.0, 3.5, 100.8, 0.1, 3)), 100.0);
    CHECK_EQ(CtcssDetector::nearest_standard(101.75), 0.0);
}

TEST_CASE(ctcss_reports_nothing_without_a_tone) {
    CHECK_EQ(run(12000.0, nfm_audio(12000.0, 4.0, 0.0, 0.0, 4)), 0.0);
    // Noise alone, as with the squelch open on an empty channel.
    CHECK_EQ(run(12000.0, nfm_audio(12000.0, 4.0, 0.0, 0.0, 5, 0.0, 0.3)), 0.0);
}

TEST_CASE(ctcss_lets_go_when_the_tone_stops_and_starts_over_on_configure) {
    CtcssDetector detector;
    detector.configure(12000.0);
    const auto with = nfm_audio(12000.0, 3.5, 88.5, 0.1, 6);
    detector.process(with.data(), with.size());
    CHECK_EQ(detector.tone_hz(), 88.5);
    const auto without = nfm_audio(12000.0, 2.5, 0.0, 0.0, 7);
    detector.process(without.data(), without.size());
    CHECK_EQ(detector.tone_hz(), 0.0);
    detector.process(with.data(), with.size());
    CHECK_EQ(detector.tone_hz(), 88.5);
    detector.configure(24000.0);
    CHECK_EQ(detector.tone_hz(), 0.0);
}

#include "../src/core/protocol.h"

TEST_CASE(binary_meter_carries_the_ctcss_tone_in_nfm_and_the_carrier_offset_in_sam) {
    using namespace fernsdr::proto;
    uint8_t out[kMeterBytes];
    const auto field = [&]() {
        return static_cast<int32_t>(static_cast<uint32_t>(out[8]) | static_cast<uint32_t>(out[9]) << 8 |
                                    static_cast<uint32_t>(out[10]) << 16 | static_cast<uint32_t>(out[11]) << 24);
    };
    Meter nfm;
    nfm.nfm = true;
    nfm.ctcss_hz = 88.5f;
    nfm.pll_offset = 12.0f;
    write_meter(out, nfm);
    CHECK_EQ(out[1] & 16, 16);
    CHECK_EQ(out[1] & 8, 0);
    CHECK_EQ(field(), 885);

    Meter sam;
    sam.sam = true;
    sam.pll_offset = -3.4f;
    sam.ctcss_hz = 88.5f;
    write_meter(out, sam);
    CHECK_EQ(out[1] & 16, 0);
    CHECK_EQ(field(), -34);
}

#include "../src/dsp/biquad.h"

TEST_CASE(tone_notch_takes_out_its_frequency_and_leaves_the_rest) {
    fernsdr::Biquad notch;
    notch.set_notch(12000.0, 88.5, 10.0);
    CHECK(notch.magnitude(12000.0, 88.5) < 1e-3);
    CHECK(notch.magnitude(12000.0, 88.5 + 20.0) > 0.9);
    CHECK(std::fabs(notch.magnitude(12000.0, 300.0) - 1.0) < 0.01);
    CHECK(std::fabs(notch.magnitude(12000.0, 1000.0) - 1.0) < 0.001);
    notch.set_notch(12000.0, 0.0, 10.0);
    CHECK_EQ(notch.magnitude(12000.0, 88.5), 1.0);
}

TEST_CASE(ctcss_notch_at_the_measured_frequency_removes_a_tone_from_an_encoder_off_the_standard) {
    const double rate = 12000.0;
    // An encoder a hertz high: named 100.0, notched where it really is.
    const auto audio = nfm_audio(rate, 4.0, 101.0, 0.1, 11, 0.5, 0.0);
    CtcssDetector detector;
    detector.configure(rate);
    detector.process(audio.data(), audio.size());
    CHECK_EQ(detector.tone_hz(), 100.0);
    CHECK(std::fabs(detector.measured_hz() - 101.0) < 0.2);

    fernsdr::Biquad notch;
    notch.set_notch(rate, detector.measured_hz(), 10.0);
    std::vector<float> filtered = audio;
    notch.process(filtered.data(), filtered.size());
    // Goertzel power at a frequency over the last second, past the notch's
    // settling.
    const auto power = [&](const std::vector<float>& x, double hz) {
        const double c = 2.0 * std::cos(2.0 * kPi * hz / rate);
        double s1 = 0, s2 = 0;
        for (size_t i = x.size() - static_cast<size_t>(rate); i < x.size(); i++) {
            const double s0 = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    };
    const double tone_drop_db = 10.0 * std::log10(power(audio, 101.0) / power(filtered, 101.0));
    const double voice_change_db = 10.0 * std::log10(power(filtered, 1090.0) / power(audio, 1090.0));
    CHECK(tone_drop_db > 30.0);
    CHECK(std::fabs(voice_change_db) < 0.5);
}

namespace {

// How long until the gate opens, in seconds, or -1 if it never does.
double gate_opens_after(double rate, double chosen, const std::vector<float>& audio) {
    CtcssDetector detector;
    detector.configure(rate);
    detector.set_squelch_tone(chosen);
    for (size_t i = 0; i < audio.size(); i += 240) {
        detector.process(audio.data() + i, std::min<size_t>(240, audio.size() - i));
        if (detector.squelch_open()) return static_cast<double>(i + 240) / rate;
    }
    return -1.0;
}

}  // namespace

TEST_CASE(tone_squelch_opens_fast_on_its_tone_and_never_on_a_neighbour) {
    const double rate = 12000.0;
    const auto& tones = CtcssDetector::standard_tones();
    for (size_t i = 0; i < tones.size(); i++) {
        const auto audio = nfm_audio(rate, 2.0, tones[i], 0.1, static_cast<uint32_t>(100 + i));
        const double opened = gate_opens_after(rate, tones[i], audio);
        CHECK(opened > 0.0 && opened < 0.7);
        if (i > 0) CHECK_EQ(gate_opens_after(rate, tones[i - 1], audio), -1.0);
        if (i + 1 < tones.size()) CHECK_EQ(gate_opens_after(rate, tones[i + 1], audio), -1.0);
    }
}

TEST_CASE(tone_squelch_stays_shut_on_voice_or_noise_and_closes_when_the_tone_stops) {
    const double rate = 12000.0;
    CHECK_EQ(gate_opens_after(rate, 88.5, nfm_audio(rate, 3.0, 0.0, 0.0, 200)), -1.0);
    CHECK_EQ(gate_opens_after(rate, 88.5, nfm_audio(rate, 3.0, 0.0, 0.0, 201, 0.0, 0.3)), -1.0);
    // Loud voice over the tone still opens it.
    CHECK(gate_opens_after(rate, 88.5, nfm_audio(rate, 2.0, 88.5, 0.1, 202, 1.0)) > 0.0);

    CtcssDetector detector;
    detector.configure(rate);
    detector.set_squelch_tone(88.5);
    CHECK(!detector.squelch_open());
    const auto with = nfm_audio(rate, 1.5, 88.5, 0.1, 203);
    detector.process(with.data(), with.size());
    CHECK(detector.squelch_open());
    const auto without = nfm_audio(rate, 0.6, 0.0, 0.0, 204);
    size_t closed_at = 0;
    for (size_t i = 0; i < without.size(); i += 240) {
        detector.process(without.data() + i, 240);
        if (!detector.squelch_open()) { closed_at = i + 240; break; }
    }
    CHECK(closed_at > 0 && static_cast<double>(closed_at) / rate < 0.6);
    // Off, or a frequency that is not a standard tone: always open.
    detector.set_squelch_tone(0.0);
    CHECK(detector.squelch_open());
    detector.set_squelch_tone(100.3);
    CHECK_EQ(detector.squelch_tone(), 0.0);
}
