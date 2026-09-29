#include "../src/core/decoder_tap.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

using fernsdr::cfloat;
using fernsdr::Channelizer;
using fernsdr::DecoderTap;

namespace {

constexpr double kRate = 192000.0;
constexpr size_t kFft = 8192;
// The band's spectrum starts here; the dial is placed inside it.
constexpr double kOrigin = 14000000.0;
constexpr double kDial = 14074000.0;

// Feeds `blocks` blocks of a complex tone at RF `tone_hz` and gives every
// block to the tap with the wall time it "arrived".
template <typename Arrival>
std::vector<fernsdr::TapFrame> run(DecoderTap& tap, Channelizer& channelizer, double tone_hz, size_t blocks,
                                    Arrival arrival) {
    std::vector<cfloat> input(channelizer.block_size());
    size_t n = 0;
    std::vector<fernsdr::TapFrame> frames;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < input.size(); i++, n++) {
            const double a = 2.0 * M_PI * (tone_hz - kOrigin) * static_cast<double>(n) / kRate;
            input[i] = cfloat(static_cast<float>(0.5 * std::cos(a)), static_cast<float>(0.5 * std::sin(a)));
        }
        channelizer.process(input.data());
        tap.process(channelizer, arrival(b));
        for (auto& frame : tap.take_frames()) frames.push_back(std::move(frame));
    }
    return frames;
}

double amplitude_at(const std::vector<cfloat>& x, double freq, double rate) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < x.size(); i++) {
        const double a = -2.0 * M_PI * freq * static_cast<double>(i) / rate;
        re += x[i].real() * std::cos(a) - x[i].imag() * std::sin(a);
        im += x[i].real() * std::sin(a) + x[i].imag() * std::cos(a);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

std::vector<cfloat> joined(const std::vector<fernsdr::TapFrame>& frames, size_t skip) {
    std::vector<cfloat> out;
    for (size_t i = skip; i < frames.size(); i++) out.insert(out.end(), frames[i].samples.begin(), frames[i].samples.end());
    return out;
}

const int64_t kT0 = 1'790'000'000'000'000;  // microseconds
const double kBlockUs = (kFft / 2) / kRate * 1e6;

}  // namespace

TEST_CASE(decoder_tap_puts_the_dial_audio_where_the_contract_says) {
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    CHECK(tap.rate() >= 4000.0);
    CHECK(tap.rate() <= 12000.0);
    // A signal at 1500 Hz audio: 500 Hz below the channel's centre.
    const auto frames = run(tap, channelizer, kDial + 1500.0, 80, [](size_t b) { return kT0 + static_cast<int64_t>((b + 1) * kBlockUs); });
    const auto inside = joined(frames, 2);
    CHECK(amplitude_at(inside, -500.0, tap.rate()) > 0.4);

    Channelizer other(kRate, kFft);
    DecoderTap tap2(0, other, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    // 6 kHz audio lies outside the 0 to 4 kHz the channel covers.
    const auto outside = joined(run(tap2, other, kDial + 6000.0, 80, [](size_t b) { return kT0 + static_cast<int64_t>((b + 1) * kBlockUs); }), 2);
    double power = 0.0;
    for (const auto& s : outside) power += std::norm(s);
    CHECK(std::sqrt(power / static_cast<double>(outside.size())) < 0.005);
}

TEST_CASE(decoder_tap_frames_are_contiguous_and_the_first_says_the_clock_was_set) {
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(3, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    const auto frames = run(tap, channelizer, kDial + 1000.0, 60, [](size_t b) { return kT0 + static_cast<int64_t>((b + 1) * kBlockUs); });
    CHECK(frames.size() >= 3u);
    CHECK_EQ(frames[0].flags, DecoderTap::kReanchored);
    for (size_t i = 0; i < frames.size(); i++) {
        CHECK_EQ(frames[i].channel, 3);
        CHECK_EQ(frames[i].samples.size(), tap.frame_samples());
        if (i > 0) {
            CHECK_EQ(frames[i].flags, 0);
            CHECK_EQ(frames[i].index, frames[i - 1].index + frames[i - 1].samples.size());
            const double step_us = static_cast<double>(frames[i - 1].samples.size()) * 1e6 / tap.rate();
            CHECK(std::fabs(static_cast<double>(frames[i].utc_us - frames[i - 1].utc_us) - step_us) < 2.0);
        }
    }
}

TEST_CASE(decoder_tap_times_samples_afresh_after_a_stop_hours_long) {
    // A band off the air for the night and on again: the first frame after
    // says the clock was set, carries only new samples, and is stamped with
    // the time they arrived, not hours earlier.
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    // Less than a frame, so one is half built when the band stops.
    const auto before = run(tap, channelizer, kDial + 1000.0, 7, [](size_t b) { return kT0 + static_cast<int64_t>((b + 1) * kBlockUs); });
    CHECK(before.empty());
    tap.reanchor();
    const int64_t later = kT0 + 12LL * 3600 * 1'000'000;
    const auto after = run(tap, channelizer, kDial + 1000.0, 30, [&](size_t b) { return later + static_cast<int64_t>((b + 1) * kBlockUs); });
    CHECK(!after.empty());
    if (after.empty()) return;
    CHECK_EQ(after[0].flags, DecoderTap::kReanchored);
    CHECK(after[0].utc_us >= later - static_cast<int64_t>(2 * kBlockUs));
    CHECK(after[0].utc_us <= later + static_cast<int64_t>(2 * kBlockUs));
    CHECK_EQ(after[0].samples.size(), tap.frame_samples());
}

TEST_CASE(decoder_tap_times_samples_by_their_earliest_arrival) {
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> late(0, 40000);  // up to 40 ms of buffering and scheduling
    const auto frames = run(tap, channelizer, kDial + 1000.0, 400, [&](size_t b) {
        return kT0 + static_cast<int64_t>((b + 1) * kBlockUs) + late(rng);
    });
    // Sample 0 was at kT0; the anchor ends within a few milliseconds of it,
    // not at the average lateness.
    CHECK(tap.anchor_us() >= kT0);
    CHECK(tap.anchor_us() - kT0 < 3000);
    // And a frame's time is its sample's, less the one block a channel lags.
    const int64_t expected = kT0 - static_cast<int64_t>(kBlockUs) +
                             static_cast<int64_t>(static_cast<double>(frames.back().index) * 1e6 / tap.rate());
    CHECK(std::llabs(frames.back().utc_us - expected) < 3000);
}

TEST_CASE(decoder_tap_follows_a_source_clock_that_runs_slow) {
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    // The source's "192 kHz" is really 50 ppm slower: each block takes
    // longer in wall time than its sample count says.
    const double real_block_us = kBlockUs * (1.0 + 50e-6);
    const size_t blocks = static_cast<size_t>(120e6 / kBlockUs);  // two minutes
    run(tap, channelizer, kDial + 1000.0, blocks, [&](size_t b) { return kT0 + static_cast<int64_t>((b + 1) * real_block_us); });
    // A minimum held from the start would be 6 ms behind by now; the anchor
    // follows within the last minute's drift.
    const double true_now_offset = static_cast<double>(blocks) * (real_block_us - kBlockUs);
    const double stamped_now = static_cast<double>(tap.anchor_us() - kT0);
    CHECK(true_now_offset - stamped_now < 3500.0);
}

TEST_CASE(decoder_tap_drops_the_oldest_frames_and_marks_the_gap) {
    Channelizer channelizer(kRate, kFft);
    DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
    std::vector<cfloat> input(channelizer.block_size(), cfloat(0.1f, 0.0f));
    // Nobody takes frames for long enough to overflow the queue.
    const size_t blocks = static_cast<size_t>(30e6 / kBlockUs);
    for (size_t b = 0; b < blocks; b++) {
        channelizer.process(input.data());
        tap.process(channelizer, kT0 + static_cast<int64_t>((b + 1) * kBlockUs));
    }
    const auto frames = tap.take_frames();
    CHECK_EQ(frames.size(), 64u);
    CHECK((frames[0].flags & DecoderTap::kGap) != 0);
    CHECK(frames[0].index > 0u);
    for (size_t i = 1; i < frames.size(); i++) CHECK_EQ(frames[i].index, frames[i - 1].index + frames[i - 1].samples.size());
}

TEST_CASE(decoder_tap_stamps_a_burst_at_the_time_it_went_into_the_band) {
    for (size_t fft : {size_t{4096}, size_t{65536}}) {
        Channelizer channelizer(kRate, fft);
        DecoderTap tap(0, channelizer, kOrigin, 1.0, kDial, 2000.0, 4000.0);
        const double block_us = (fft / 2) / kRate * 1e6;
        std::vector<cfloat> input(channelizer.block_size());
        const size_t burst = 20 * channelizer.block_size() + 321;
        size_t n = 0;
        std::vector<fernsdr::TapFrame> frames;
        for (size_t b = 0; b < 40; b++) {
            for (size_t i = 0; i < input.size(); i++, n++) {
                const double t = (static_cast<double>(n) - static_cast<double>(burst)) / kRate;
                const double envelope = std::exp(-t * t / (2 * 0.0005 * 0.0005));
                const double a = 2.0 * M_PI * (kDial + 1500.0 - kOrigin) * static_cast<double>(n) / kRate;
                input[i] = cfloat(static_cast<float>(envelope * std::cos(a)), static_cast<float>(envelope * std::sin(a)));
            }
            channelizer.process(input.data());
            // Each block arrives the moment its last sample was taken.
            tap.process(channelizer, kT0 + static_cast<int64_t>((b + 1) * block_us));
            for (auto& frame : tap.take_frames()) frames.push_back(std::move(frame));
        }
        int64_t peak_time = 0;
        double best = 0.0;
        for (const auto& frame : frames) {
            for (size_t i = 0; i < frame.samples.size(); i++) {
                if (std::norm(frame.samples[i]) > best) {
                    best = std::norm(frame.samples[i]);
                    peak_time = frame.utc_us + static_cast<int64_t>(static_cast<double>(i) * 1e6 / tap.rate());
                }
            }
        }
        const int64_t burst_time = kT0 + static_cast<int64_t>(static_cast<double>(burst) * 1e6 / kRate);
        CHECK(std::llabs(peak_time - burst_time) < 1000);
    }
}

#include "../src/core/band.h"
#include "../src/source/source.h"
#include "../src/util/config.h"

#include <chrono>
#include <thread>

TEST_CASE(a_band_feeds_its_decoder_channels_with_nobody_listening) {
    fernsdr::ConfigSection section("band:test");
    section.set("source", "test");
    section.set("sample_rate", "192000");
    section.set("center", "7100000");
    section.set("fft_size", "8192");
    section.set("realtime", "false");
    std::string error;
    fernsdr::Band band("test", "test", fernsdr::make_source(section, error), section);
    // The test source keeps a carrier 800 Hz below the centre: 1500 Hz audio
    // for a receiver set 2300 Hz below the centre.
    auto tap = band.add_decoder_tap(0, 7100000.0 - 2300.0, 2000.0, 4000.0);
    CHECK_EQ(band.listener_count(), 0);
    CHECK(band.start(error));
    std::vector<fernsdr::TapFrame> frames;
    for (int i = 0; i < 200 && frames.size() < 12; i++) {
        for (auto& frame : tap->take_frames()) frames.push_back(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    band.stop();
    CHECK(frames.size() >= 12u);
    CHECK_EQ(frames[0].flags, DecoderTap::kReanchored);
    const auto samples = joined(frames, 2);
    CHECK(amplitude_at(samples, -500.0, tap->rate()) > 0.05);
    band.remove_decoder_tap(tap);
    CHECK_EQ(band.decoder_tap_count(), 0u);
}
