// The gain control and the channel noise estimate it is built on.
#include "../src/core/settings.h"
#include "../src/dsp/agc.h"
#include "../src/dsp/channel_noise.h"
#include "../src/dsp/channelizer.h"
#include "../src/dsp/fft_split.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

using fernsdr::Agc;
using fernsdr::AgcProfile;
using fernsdr::cfloat;
using fernsdr::Channel;
using fernsdr::ChannelNoise;
using fernsdr::Channelizer;
using fernsdr::SignalKind;

namespace {

constexpr double kBandRate = 48000.0;
constexpr size_t kFft = 4096;     // 11.7 Hz bins, 42.7 ms blocks
constexpr size_t kIfft = 1024;    // 12 kHz channel output

double db(double power) { return 10.0 * std::log10(power); }

std::vector<cfloat> white(size_t n, double variance, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, static_cast<float>(std::sqrt(variance / 2.0)));
    std::vector<cfloat> x(n);
    // One draw at a time: C++ leaves the order of a call's arguments open,
    // and GCC on x86-64, which the figures here were measured with, drew the
    // imaginary part first. Other compilers and ARM drew the other way round
    // and tested different noise.
    for (auto& v : x) {
        const float im = g(rng);
        const float re = g(rng);
        v = cfloat(re, im);
    }
    return x;
}

// White noise with the negative frequencies `below_db` quieter than the
// positive ones, shaped in one long transform: two places in the band with
// different noise.
std::vector<cfloat> two_level(size_t n, double variance, double below_db, unsigned seed) {
    auto x = white(n, variance, seed);
    fernsdr::FftSplit fft(n);
    std::vector<float> re(n), im(n);
    for (size_t i = 0; i < n; i++) re[i] = x[i].real(), im[i] = x[i].imag();
    fft.forward(re.data(), im.data());
    const float scale = static_cast<float>(std::pow(10.0, below_db / 20.0));
    for (size_t k = n / 2; k < n; k++) re[k] *= scale, im[k] *= scale;
    fft.inverse(re.data(), im.data());
    for (size_t i = 0; i < n; i++) x[i] = cfloat(re[i], im[i]);
    return x;
}

// Runs `x` through a channelizer and one channel at `center_hz` with the given
// passband, feeding every block's spectrum to a ChannelNoise. Returns the
// channel's mean output power over the second half and the estimate for it,
// averaged over the same stretch: any one reading wanders by a few tenths of
// a dB, which is the noise's own.
struct Measured {
    double output_power = 0.0;
    double estimated_power = 0.0;
};

Measured through_channel(const std::vector<cfloat>& x, double center_hz, double low, double high,
                         ChannelNoise& noise) {
    Channelizer channelizer(kBandRate, kFft, SignalKind::Iq);
    Channel channel(channelizer, kIfft);
    channel.set_passband(center_hz, low, high);
    std::vector<cfloat> out(channel.output_per_block());
    const size_t block = channelizer.block_size();
    const size_t blocks = x.size() / block;
    double power = 0.0, estimate = 0.0;
    size_t count = 0, readings = 0;
    for (size_t b = 0; b < blocks; b++) {
        channelizer.process(x.data() + b * block);
        channel.pull(channelizer, out.data());
        noise.update(channelizer.current_block(), center_hz + 0.5 * (low + high), false);
        if (b < blocks / 2) continue;
        for (const cfloat& v : out) power += std::norm(v);
        count += out.size();
        estimate += noise.channel_power(channel.noise_bandwidth_hz());
        readings++;
    }
    return {power / static_cast<double>(count), estimate / static_cast<double>(readings)};
}

// Adds a station: band-limited noise between `low` and `high` hertz,
// `over_db` over the noise density already there.
void add_station(std::vector<cfloat>& x, double variance, double low, double high, double over_db, unsigned seed) {
    const size_t n = x.size();
    auto station = white(n, variance * std::pow(10.0, over_db / 10.0), seed);
    fernsdr::FftSplit fft(n);
    std::vector<float> re(n), im(n);
    for (size_t i = 0; i < n; i++) re[i] = station[i].real(), im[i] = station[i].imag();
    fft.forward(re.data(), im.data());
    for (size_t k = 0; k < n; k++) {
        const double hz = (k < n / 2 ? static_cast<double>(k) : static_cast<double>(k) - static_cast<double>(n)) *
                          kBandRate / static_cast<double>(n);
        if (hz < low || hz > high) re[k] = im[k] = 0.0f;
    }
    fft.inverse(re.data(), im.data());
    for (size_t i = 0; i < n; i++) x[i] += cfloat(re[i], im[i]);
}

std::vector<cfloat> tone(size_t n, double hz, double amplitude, double rate) {
    std::vector<cfloat> x(n);
    for (size_t i = 0; i < n; i++) x[i] = std::polar(static_cast<float>(amplitude), static_cast<float>(2 * M_PI * hz * i / rate));
    return x;
}

double rms(const std::vector<cfloat>& x, size_t start, size_t count) {
    double acc = 0.0;
    for (size_t i = start; i < start + count; i++) acc += std::norm(x[i]);
    return std::sqrt(acc / static_cast<double>(count));
}

// Feeds `x` to the AGC a block at a time, as a listener does.
void run(Agc& agc, std::vector<cfloat>& x, size_t block = 128) {
    for (size_t o = 0; o + block <= x.size(); o += block) agc.process(x.data() + o, block);
}

}  // namespace

// --- ChannelNoise ------------------------------------------------------------

TEST_CASE(channel_noise_predicts_what_the_channel_carries) {
    // The unit the gain control works in: the power per output sample of the
    // listener's own channel. Off by a factor here and the noise ends up at
    // the wrong distance under the target, which nothing else would catch.
    const auto x = white(static_cast<size_t>(kBandRate * 4), 1e-4, 11);
    for (const double center : {-15000.0, 0.0, 7000.0}) {
        for (const auto& [low, high] : {std::pair{300.0, 2700.0}, std::pair{-4500.0, 4500.0}, std::pair{450.0, 950.0}}) {
            ChannelNoise noise;
            const Measured m = through_channel(x, center, low, high, noise);
            // The mask's raised-cosine edges pass a little less than the
            // nominal width: 0.14 dB on a 2.4 kHz filter.
            CHECK_NEAR(db(m.estimated_power) - db(m.output_power), 0.0, 0.5);
        }
    }
}

TEST_CASE(channel_noise_reads_high_by_the_share_of_the_window_in_use) {
    // Signals do not reach the quietest bins, but they do push the 10th
    // percentile up the noise's distribution: with a share p of the window
    // occupied the estimate reads about 1 / (1 - p) high, 1 dB at a fifth and
    // 3 dB at half. On the safe side, since the noise is then held further
    // under the target, never closer.
    const size_t n = size_t{1} << 18;
    const auto clean = white(n, 1e-4, 12);
    const auto reading = [&](const std::vector<cfloat>& x) {
        ChannelNoise noise;
        return db(through_channel(x, 0.0, 300.0, 2700.0, noise).estimated_power);
    };
    const double noise = reading(clean);

    // Two carriers 50 dB over the noise in their bins: their skirts cost
    // almost nothing.
    auto carriers = clean;
    for (const double hz : {-2200.0, 5100.0}) {
        // A bin's noise is 1e-4 * K / 2; a tone of amplitude a puts about
        // (a K 2 / pi)^2 into its bin.
        const double amplitude = std::sqrt(1e-4 * 2048.0 * 1e5) * M_PI / (2.0 * kFft);
        const auto carrier = tone(n, hz, amplitude, kBandRate);
        for (size_t i = 0; i < n; i++) carriers[i] += carrier[i];
    }
    const double with_carriers = reading(carriers) - noise;

    // A fifth of the 12 kHz window: the station tuned in, 20 dB up.
    auto one = clean;
    add_station(one, 1e-4, 300.0, 2700.0, 20.0, 31);
    const double fifth = reading(one) - noise;

    // Half of it: two more stations, 35 and 28 dB up.
    auto three = one;
    add_station(three, 1e-4, -4500.0, -2100.0, 35.0, 32);
    add_station(three, 1e-4, 3100.0, 4300.0, 28.0, 33);
    const double half = reading(three) - noise;

    CHECK(with_carriers < 0.5);
    CHECK_NEAR(fifth, 10.0 * std::log10(1.0 / 0.8), 0.5);
    CHECK_NEAR(half, 10.0 * std::log10(1.0 / 0.5), 0.7);
    // Nothing reads low.
    CHECK(with_carriers > -0.3 && fifth > 0.0 && half > 0.0);
}

TEST_CASE(channel_noise_stays_on_its_side_of_the_band_edge) {
    // The positive half of the band 20 dB noisier than the negative half. A
    // channel just under the top edge must read the top edge's noise; a
    // window that wrapped past Nyquist would pull in the quiet bottom edge.
    const size_t n = size_t{1} << 18;
    const auto x = two_level(n, 1e-4, -20.0, 13);
    ChannelNoise noise;
    const Measured edge = through_channel(x, 22500.0, 300.0, 1200.0, noise);
    CHECK_NEAR(db(edge.estimated_power) - db(edge.output_power), 0.0, 0.7);
}

TEST_CASE(channel_noise_starts_over_after_a_long_retune) {
    // Tuned from the noisy half to the quiet one: the estimate must land on
    // the new noise at its next look, not glide there for a second while the
    // AGC holds the new channel's noise 20 dB too low.
    const size_t n = size_t{1} << 18;
    const auto x = two_level(n, 1e-4, -20.0, 14);
    Channelizer channelizer(kBandRate, kFft, SignalKind::Iq);
    ChannelNoise noise;
    const size_t block = channelizer.block_size();
    size_t b = 0;
    for (; b < 60; b++) {
        channelizer.process(x.data() + b * block);
        noise.update(channelizer.current_block(), 10000.0, false);
    }
    const double noisy = noise.bin_power();
    channelizer.process(x.data() + b * block);
    noise.update(channelizer.current_block(), -10000.0, false);
    CHECK_NEAR(db(noise.bin_power()) - db(noisy), -20.0, 1.5);
}

TEST_CASE(channel_noise_follows_a_drag) {
    // Dragged a little each block from the noisy half of the band to the
    // quiet one: no single step is a retune, but by the end the estimate must
    // be the quiet half's, not a second behind it.
    const size_t n = size_t{1} << 18;
    const auto x = two_level(n, 1e-4, -20.0, 16);
    Channelizer channelizer(kBandRate, kFft, SignalKind::Iq);
    ChannelNoise noise;
    const size_t block = channelizer.block_size();
    // The first block's transform is half the zeros the channelizer starts
    // with, 3 dB short: the noisy half is read once the transform is full.
    channelizer.process(x.data());
    double center = 10000.0, noisy = 0.0;
    for (size_t b = 1; b < 121; b++) {
        channelizer.process(x.data() + b * block);
        if (b >= 11 && center > -10000.0) center -= 200.0;
        noise.update(channelizer.current_block(), center, false);
        if (b == 10) noisy = noise.bin_power();
        if (center <= -10000.0) break;
    }
    // One look, so a dB or so of its own scatter; lagging behind would leave
    // it most of the 20 dB short.
    CHECK_NEAR(db(noise.bin_power()) - db(noisy), -20.0, 3.0);
}

TEST_CASE(channel_noise_reads_a_real_input_from_its_lower_half) {
    // A real front end fills only bins 0 to K/2; the window has to stay there.
    const size_t n = static_cast<size_t>(kBandRate * 4);
    std::mt19937 rng(15);
    std::normal_distribution<float> g(0.0f, 0.01f);
    std::vector<float> x(n);
    for (auto& v : x) v = g(rng);
    Channelizer channelizer(kBandRate, kFft, SignalKind::Real);
    Channel channel(channelizer, kIfft);
    for (const double center : {1500.0, 12000.0, 20500.0}) {
        channel.set_passband(center, 300.0, 2700.0);
        ChannelNoise noise;
        std::vector<cfloat> out(channel.output_per_block());
        const size_t block = channelizer.block_size();
        const size_t blocks = n / block;
        double power = 0.0;
        size_t count = 0;
        for (size_t b = 0; b < blocks; b++) {
            channelizer.process_real(x.data() + b * block);
            channel.pull(channelizer, out.data());
            noise.update(channelizer.current_block(), center + 1500.0, true);
            if (b < blocks / 2) continue;
            for (const cfloat& v : out) power += std::norm(v);
            count += out.size();
        }
        CHECK_NEAR(db(noise.channel_power(2400.0)) - db(power / static_cast<double>(count)), 0.0, 0.5);
    }
}

// --- Agc ---------------------------------------------------------------------

namespace {

constexpr double kRate = 12000.0;
constexpr double kTarget = Agc::kTarget;
constexpr double kNoiseCeiling = kTarget * Agc::kNoiseUnderTarget;

}  // namespace

TEST_CASE(agc_leaves_a_weak_signal_at_the_gain_that_holds_the_noise) {
    // Speech a few dB over the noise: the gain never moves, so the noise is
    // exactly as steady as with no gain control, and it sits 15 dB under the
    // target.
    const size_t n = static_cast<size_t>(kRate * 6);
    const double noise_power = 1e-6;
    auto x = white(n, noise_power, 21);
    for (size_t i = 0; i < n; i++) {
        const bool talking = (i / static_cast<size_t>(kRate * 0.3)) % 2 == 0;
        if (talking) x[i] += std::polar(0.0014f, static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    }
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(noise_power);
    double lowest = 1e9, highest = -1e9;
    for (size_t o = 0; o + 128 <= n; o += 128) {
        agc.process(x.data() + o, 128);
        if (o < n / 3) continue;
        lowest = std::min(lowest, static_cast<double>(agc.gain_db()));
        highest = std::max(highest, static_cast<double>(agc.gain_db()));
    }
    CHECK(highest - lowest < 0.5);
    CHECK_NEAR(highest, 20.0 * std::log10(kNoiseCeiling / std::sqrt(noise_power)), 0.5);
}

TEST_CASE(agc_levels_strong_signals_on_their_rms) {
    for (const double amplitude : {0.5, 0.01, 0.0005}) {
        auto x = tone(static_cast<size_t>(kRate * 3), 900.0, amplitude, kRate);
        Agc agc;
        agc.configure(kRate);
        agc.set_profile(AgcProfile::Slow);
        agc.set_noise_power(1e-12);
        run(agc, x);
        CHECK_NEAR(20.0 * std::log10(rms(x, x.size() / 2, x.size() / 4) / kTarget), 0.0, 0.5);
    }
}

TEST_CASE(agc_holds_its_gain_through_a_two_second_pause) {
    // A strong station, then two seconds of band noise: the gap between overs.
    // The gain must not climb and bring the noise up with it.
    const size_t n = static_cast<size_t>(kRate * 4);
    const double noise_power = 1e-8;
    auto x = white(n, noise_power, 22);
    // Rising through the over, so every stretch sets the gain and the hang
    // runs from the last one: a level that only held would let the hang
    // lapse mid-over, as it does in ka9q-radio, and that is not what this
    // measures.
    const size_t talk = static_cast<size_t>(kRate * 1.5);
    const auto level = [&](size_t i) { return 0.02 * std::pow(2.5, static_cast<double>(i) / static_cast<double>(talk)); };
    for (size_t i = 0; i < talk; i++) x[i] += std::polar(static_cast<float>(level(i)), static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(noise_power);
    run(agc, x);
    const double signal = rms(x, talk * 3 / 4, talk / 5);
    const double pause_early = rms(x, talk + static_cast<size_t>(kRate * 0.2), static_cast<size_t>(kRate * 0.2));
    const double pause_late = rms(x, talk + static_cast<size_t>(kRate * 1.7), static_cast<size_t>(kRate * 0.2));
    CHECK_NEAR(20.0 * std::log10(pause_late / pause_early), 0.0, 1.0);
    // The gain the end of the over set, still.
    CHECK_NEAR(20.0 * std::log10(kTarget / pause_late), 20.0 * std::log10(level(talk) / std::sqrt(noise_power)), 1.0);
    CHECK_NEAR(20.0 * std::log10(signal / kTarget), 0.0, 1.0);
}

TEST_CASE(agc_slow_recovers_at_twenty_db_a_second_once_the_hold_is_over) {
    const size_t n = static_cast<size_t>(kRate * 6);
    auto x = white(n, 1e-14, 23);
    const size_t talk = static_cast<size_t>(kRate * 0.5);
    for (size_t i = 0; i < talk; i++) {
        const double level = 0.05 * std::pow(2.0, static_cast<double>(i) / static_cast<double>(talk));
        x[i] += std::polar(static_cast<float>(level), static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    }
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(1e-14);
    std::vector<double> gain_at;
    for (size_t o = 0; o + 120 <= n; o += 120) {
        agc.process(x.data() + o, 120);
        gain_at.push_back(agc.gain_db());
    }
    const auto at = [&](double seconds) { return gain_at[static_cast<size_t>(seconds * kRate / 120)]; };
    // Held until 2.5 s after the last loud stretch...
    CHECK_NEAR(at(2.9) - at(0.6), 0.0, 0.1);
    // ...then climbing at 20 dB a second.
    CHECK_NEAR(at(4.5) - at(3.5), 20.0, 0.5);
}

TEST_CASE(agc_pulls_a_sudden_strong_signal_down_before_it_arrives) {
    // Quiet, then 40 dB louder somewhere in a 128-sample block. The decision
    // is made on the block before any of it leaves, so an onset well inside a
    // 2 ms slice stays near the target from its first sample. One in the last
    // few samples is only partly in its slice, the rest of it in the next
    // block; the sample limit keeps it from clipping.
    for (const size_t offset : {size_t{25}, size_t{100}, size_t{121}, size_t{125}, size_t{127}}) {
        const size_t n = static_cast<size_t>(kRate * 2);
        const size_t step = 128 * 90 + offset;
        auto x = tone(n, 800.0, 0.001, kRate);
        for (size_t i = step; i < n; i++) x[i] *= 100.0f;
        Agc agc;
        agc.configure(kRate);
        agc.set_profile(AgcProfile::Slow);
        agc.set_noise_power(1e-12);
        run(agc, x);
        double worst = 0.0;
        for (size_t i = step; i < step + static_cast<size_t>(kRate * 0.2); i++)
            worst = std::max(worst, static_cast<double>(std::abs(x[i])));
        CHECK(worst <= 4.0 * kTarget * 1.01);
        if (offset <= 100) CHECK(worst < 1.5 * std::sqrt(2.0) * kTarget);
    }
}

TEST_CASE(agc_does_not_let_a_peak_end_the_hold) {
    // An over rising towards its end, a plosive 10 dB up near the end, then a
    // pause. The plosive pulls the gain down for 80 ms; after that the gain
    // may come back to where the speech had it, but not climb through the
    // pause, which a plain 80 ms hold in place of the 2.5 s one would allow.
    const size_t n = static_cast<size_t>(kRate * 4);
    const double noise_power = 1e-8;
    auto x = white(n, noise_power, 24);
    const size_t talk = 128 * 187;  // just under 2 s, whole blocks
    for (size_t i = 0; i < talk; i++) {
        const double level = 0.01 * std::pow(2.5, static_cast<double>(i) / static_cast<double>(talk));
        x[i] += std::polar(static_cast<float>(level), static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    }
    const size_t plosive = 128 * 178;  // at the start of a block, 0.1 s before the end
    for (size_t i = plosive; i < plosive + 24; i++) x[i] *= 3.16f;
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(noise_power);
    std::vector<double> gain_after;  // gain at the end of each block
    for (size_t o = 0; o + 128 <= n; o += 128) {
        agc.process(x.data() + o, 128);
        gain_after.push_back(agc.gain_db());
    }
    const double speaking = gain_after[plosive / 128 - 1];
    CHECK(gain_after[plosive / 128] < speaking - 5.0);            // pulled down
    const auto at = [&](double seconds) { return gain_after[static_cast<size_t>(seconds * kRate / 128)]; };
    CHECK_NEAR(at(2.5), speaking, 0.5);                            // back to the speech's level
    CHECK_NEAR(at(3.8), speaking, 0.5);                            // and kept there in the pause
}

TEST_CASE(agc_keeps_the_ceiling_and_the_manual_offset) {
    auto quiet = tone(static_cast<size_t>(kRate * 2), 700.0, 1e-5, kRate);
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_max_gain_db(20.0f);
    run(agc, quiet);
    CHECK_NEAR(agc.gain_db(), 20.0, 0.01);

    auto loud = tone(static_cast<size_t>(kRate * 2), 700.0, 0.01, kRate);
    Agc offset;
    offset.configure(kRate);
    offset.set_profile(AgcProfile::Slow);
    offset.set_manual_gain_db(6.0f);
    run(offset, loud);
    CHECK_NEAR(20.0 * std::log10(rms(loud, loud.size() / 2, loud.size() / 4) / kTarget), 6.0, 0.5);
}


TEST_CASE(agc_brings_a_weaker_station_up_while_the_hold_runs) {
    // A strong station, then one 20 dB weaker but 40 dB over the noise takes
    // over: a reply on a QSO. The hold is for pauses; the reply comes up at
    // 60 dB/s to 3 dB under the target instead of waiting it out.
    const size_t n = static_cast<size_t>(kRate * 4);
    const double noise_power = 1e-8;
    auto x = white(n, noise_power, 25);
    // The first rises through its over, so every stretch sets the gain and
    // the hold runs from its end, as with speech.
    const size_t change = static_cast<size_t>(kRate * 2);
    for (size_t i = 0; i < n; i++) {
        const double first = 0.08 * std::pow(1.25, static_cast<double>(i) / static_cast<double>(change));
        const float amplitude = static_cast<float>(i < change ? first : 0.01);
        x[i] += std::polar(amplitude, static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    }
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(noise_power);
    run(agc, x);
    const auto level = [&](double from) {
        return 20.0 * std::log10(rms(x, static_cast<size_t>(from * kRate), static_cast<size_t>(kRate * 0.1)) / kTarget);
    };
    CHECK_NEAR(level(1.5), 0.0, 0.5);
    CHECK_NEAR(level(2.4), -3.0, 0.5);
    CHECK_NEAR(level(3.5), -3.0, 0.5);
}

TEST_CASE(agc_does_not_move_under_a_steady_tone) {
    // RTTY's mark or one of FT8's tones. A gain that rose to the target and
    // was pulled back by the next stretch's level moved at 25 Hz and put
    // sidebands on the tone. What is left is slow: the noise on the tone
    // makes one 20 ms stretch a little louder than the next, and the gain
    // follows that by a tenth or two of a dB, turning round a few times in
    // four seconds. Ten noise records, since one could be lucky.
    const size_t n = static_cast<size_t>(kRate * 5);
    const double noise_power = 1e-6;
    for (unsigned seed = 26; seed < 36; seed++) {
        auto x = white(n, noise_power, seed);
        for (size_t i = 0; i < n; i++) x[i] += std::polar(0.01f, static_cast<float>(2 * M_PI * 1500.0 * i / kRate));
        Agc agc;
        agc.configure(kRate);
        agc.set_profile(AgcProfile::Slow);
        agc.set_noise_power(noise_power);
        double lowest = 1e9, highest = -1e9, last = 0.0;
        int turns = 0, direction = 0;
        for (size_t o = 0; o + 128 <= n; o += 128) {
            agc.process(x.data() + o, 128);
            const double gain = agc.gain_db();
            if (o >= n / 5) {
                lowest = std::min(lowest, gain);
                highest = std::max(highest, gain);
                const int now = gain > last + 1e-4 ? 1 : (gain < last - 1e-4 ? -1 : 0);
                if (now != 0 && direction != 0 && now != direction) turns++;
                if (now != 0) direction = now;
            }
            last = gain;
        }
        CHECK(highest - lowest < 0.5);
        CHECK(turns <= 4);
    }
}

TEST_CASE(agc_stays_put_on_a_clean_tone_when_its_hold_runs_out) {
    // Nothing louder comes along, so every 0.3 s the fast profile's hold runs
    // out and the gain may go back up: not past what the tone itself allows,
    // or it rises for a stretch and the next pulls it back, a 0.2 dB step
    // each time on a tone whose level never changed.
    const size_t n = static_cast<size_t>(kRate * 3);
    std::vector<cfloat> x(n);
    for (size_t i = 0; i < n; i++) x[i] = std::polar(0.01f, static_cast<float>(2 * M_PI * 1500.0 * i / kRate));
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Fast);
    agc.set_noise_power(1e-6);
    double lowest = 1e9, highest = -1e9;
    for (size_t o = 0; o + 128 <= n; o += 128) {
        agc.process(x.data() + o, 128);
        if (o < n / 6) continue;
        lowest = std::min(lowest, static_cast<double>(agc.gain_db()));
        highest = std::max(highest, static_cast<double>(agc.gain_db()));
    }
    CHECK(highest - lowest < 0.01);
}

TEST_CASE(agc_follows_an_am_carrier_without_a_hold) {
    // A carrier modulated 80% at 100 Hz fades 12 dB over two seconds and
    // stays down. Its crests fill whole 2 ms slices 3.9 dB over its RMS, which
    // the speech rule takes for peaks; followed as a carrier, the level stays
    // put through the fade and the modulation keeps its depth.
    const size_t n = static_cast<size_t>(kRate * 7);
    const double noise_power = 1e-10;
    auto x = white(n, noise_power, 27);
    const auto fade = [](double t) {
        if (t < 2.0) return 1.0;
        return std::pow(10.0, -12.0 * std::min(t - 2.0, 2.0) / 2.0 / 20.0);
    };
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / kRate;
        x[i] += cfloat(static_cast<float>(0.05 * fade(t) * (1.0 + 0.8 * std::cos(2 * M_PI * 100.0 * t))), 0.0f);
    }
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_follow_carrier(true);
    agc.set_noise_power(noise_power);
    run(agc, x);
    const auto level = [&](double from, double seconds) {
        return 20.0 * std::log10(rms(x, static_cast<size_t>(from * kRate), static_cast<size_t>(seconds * kRate)) / kTarget);
    };
    const double before = level(1.0, 1.0);
    CHECK_NEAR(before, 0.0, 0.5);
    // Through the fade, never more than 2 dB off, and back on it after.
    for (double t = 2.0; t < 4.0; t += 0.1) CHECK(std::fabs(level(t, 0.1) - before) < 2.0);
    CHECK_NEAR(level(5.0, 1.0), before, 0.5);
    // Within a cycle of the modulation the gain stands still: the envelope's
    // trough to crest is 1.8 : 0.2, 19 dB, as it went in.
    double low = 1e9, high = 0.0;
    for (size_t i = static_cast<size_t>(kRate * 1.5); i < static_cast<size_t>(kRate * 1.6); i++) {
        low = std::min(low, static_cast<double>(std::abs(x[i])));
        high = std::max(high, static_cast<double>(std::abs(x[i])));
    }
    CHECK_NEAR(20.0 * std::log10(high / low), 19.1, 0.5);
}

TEST_CASE(agc_moving_between_speeds_keeps_the_gain) {
    const double noise_power = 1e-8;
    auto x = white(static_cast<size_t>(kRate * 4), noise_power, 28);
    const size_t talk = static_cast<size_t>(kRate);
    for (size_t i = 0; i < talk; i++) x[i] += std::polar(0.05f, static_cast<float>(2 * M_PI * 700.0 * i / kRate));
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Fast);
    agc.set_noise_power(noise_power);
    size_t o = 0;
    for (; o + 128 <= talk / 2; o += 128) agc.process(x.data() + o, 128);
    // Mid-signal, from fast to slow: the gain carries on where it was.
    const double before = agc.gain_db();
    agc.set_profile(AgcProfile::Slow);
    agc.process(x.data() + o, 128);
    o += 128;
    CHECK_NEAR(agc.gain_db(), before, 0.1);
    agc.set_profile(AgcProfile::Long);
    for (; o + 128 <= talk; o += 128) agc.process(x.data() + o, 128);
    // In the pause after it, Long would hold for five seconds. Moved to Fast
    // a second in, the hold left is cut to Fast's 0.3 s and the gain climbs.
    for (; o + 128 <= talk + static_cast<size_t>(kRate); o += 128) agc.process(x.data() + o, 128);
    const double held = agc.gain_db();
    agc.set_profile(AgcProfile::Fast);
    for (; o + 128 <= talk + static_cast<size_t>(kRate * 1.5); o += 128) agc.process(x.data() + o, 128);
    CHECK(agc.gain_db() > held + 3.0);
}

TEST_CASE(agc_coming_back_from_off_starts_settled) {
    auto x = tone(static_cast<size_t>(kRate * 2), 700.0, 0.01, kRate);
    Agc agc;
    agc.configure(kRate);
    agc.set_profile(AgcProfile::Slow);
    agc.set_noise_power(1e-14);
    run(agc, x);
    // Off while the listener retunes to a station 26 dB weaker.
    agc.set_profile(AgcProfile::Off);
    auto quiet = tone(static_cast<size_t>(kRate), 700.0, 0.0005, kRate);
    run(agc, quiet);
    // Back on, the first block is at the target, not at the gain from before
    // Off climbing back through the hold.
    agc.set_profile(AgcProfile::Slow);
    auto back = tone(128, 700.0, 0.0005, kRate);
    agc.process(back.data(), back.size());
    CHECK_NEAR(20.0 * std::log10(rms(back, 0, back.size()) / kTarget), 0.0, 0.5);
}

TEST_CASE(agc_names_its_profiles_and_takes_steady_for_slow) {
    AgcProfile profile = AgcProfile::Off;
    CHECK(fernsdr::agc_profile_from_name("steady", profile));
    CHECK(profile == AgcProfile::Slow);
    CHECK(fernsdr::agc_profile_from_name("auto", profile));
    CHECK(profile == AgcProfile::Auto);
    CHECK(fernsdr::agc_profile_from_name("med", profile));
    CHECK(profile == AgcProfile::Medium);
    CHECK(!fernsdr::agc_profile_from_name("bogus", profile));
    CHECK_EQ_STR(fernsdr::agc_profile_name(AgcProfile::Auto), "auto");
    CHECK_EQ_STR(fernsdr::agc_profile_name(AgcProfile::Slow), "slow");
}

TEST_CASE(agc_in_effect_is_slow_for_auto_and_off_for_nfm) {
    fernsdr::ChannelSettings channel;
    CHECK(channel.agc == AgcProfile::Auto);
    CHECK(fernsdr::agc_in_effect(channel) == AgcProfile::Slow);
    channel.agc = AgcProfile::Fast;
    CHECK(fernsdr::agc_in_effect(channel) == AgcProfile::Fast);
    channel.mode = fernsdr::Mode::Nfm;
    CHECK(fernsdr::agc_in_effect(channel) == AgcProfile::Off);
    channel.agc = AgcProfile::Auto;
    CHECK(fernsdr::agc_in_effect(channel) == AgcProfile::Off);
}

TEST_CASE(agc_does_not_take_static_crashes_for_a_station) {
    // Summer static: 30 ms crashes 12 dB over the noise in the pause after an
    // over, two and six a second. Each one is loud and under the target, as a
    // weaker station is, and one at a time they raised the gain the hold was
    // keeping, until the noise sat at the ceiling.
    for (const double per_second : {2.0, 6.0}) {
        const size_t n = static_cast<size_t>(kRate * 5);
        const double noise_power = 1e-8;
        auto x = white(n, noise_power, 29);
        const size_t talk = static_cast<size_t>(kRate * 2);
        for (size_t i = 0; i < talk; i++) {
            const double level = 0.02 * std::pow(1.25, static_cast<double>(i) / static_cast<double>(talk));
            x[i] += std::polar(static_cast<float>(level), static_cast<float>(2 * M_PI * 700.0 * i / kRate));
        }
        std::mt19937 rng(30);
        std::normal_distribution<float> crash(0.0f, static_cast<float>(std::sqrt(noise_power * std::pow(10.0, 1.2) / 2.0)));
        const size_t crash_length = static_cast<size_t>(kRate * 0.03);
        const size_t every = static_cast<size_t>(kRate / per_second);
        for (size_t start = talk + static_cast<size_t>(kRate * 0.25); start + crash_length < n; start += every) {
            for (size_t i = start; i < start + crash_length; i++) {
                const float im = crash(rng);
                const float re = crash(rng);
                x[i] += cfloat(re, im);
            }
        }
        Agc agc;
        agc.configure(kRate);
        agc.set_profile(AgcProfile::Slow);
        agc.set_noise_power(noise_power);
        std::vector<double> gain_after;
        for (size_t o = 0; o + 128 <= n; o += 128) {
            agc.process(x.data() + o, 128);
            gain_after.push_back(agc.gain_db());
        }
        const auto at = [&](double seconds) { return gain_after[static_cast<size_t>(seconds * kRate / 128)]; };
        // Held through the hold, crashes and all.
        CHECK_NEAR(at(4.4), at(2.0), 1.0);
    }
}
