// End-to-end: a signal generated at a known frequency must arrive at the
// client as audio at the right pitch and level, and as a waterfall line with
// the carrier in the right bin.  This is the test that would catch a sign
// error, an off-by-one in the band plan, or a codec/decoder divergence that
// every unit test passes individually.
#include "../src/codec/nac.h"
#include "../src/codec/waterfall_codec.h"
#include "../src/core/band.h"
#include "../src/core/listener.h"
#include "../src/core/protocol.h"
#include "rds_signal.h"
#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <memory>
#include <random>
#include <limits>
#include <vector>
#include <unistd.h>

using fernsdr::Band;
using fernsdr::Listener;

TEST_CASE(enabling_history_retains_the_configured_path_when_initially_off) {
    char directory[] = "/tmp/fernsdr-history-path-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/custom.wfa";
    fernsdr::Config config;
    std::string error;
    CHECK(config.parse("[band:test]\nsource=test\nsample_rate=192k\nhistory=off\nhistory_path=" + path + "\n", error));
    const auto& section = config.section("band:test");
    auto source = fernsdr::make_source(section, error);
    CHECK(source != nullptr);
    if (source) {
        Band band("test", "History", std::move(source), section);
        CHECK(::access(path.c_str(), F_OK) != 0);
        CHECK(band.set_history("public", 1, 64, 1, error));
        CHECK(::access(path.c_str(), F_OK) == 0);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

namespace {

constexpr double kBandCenter = 7100000.0;
constexpr double kBandRate = 192000.0;
// The test source puts a steady unmodulated carrier here, relative to centre.
constexpr double kCarrierOffset = -800.0;

fernsdr::Config make_config(bool realtime = false) {
    fernsdr::Config config;
    std::string error;
    const bool ok = config.parse(
        "[band:test]\n"
        "source = test\n"
        "sample_rate = 192k\n"
        "center = 7.1M\n"
        "noise = 0.001\n"
        "realtime = " + std::string(realtime ? "true" : "false") + "\n",
        error);
    (void)ok;
    return config;
}

std::unique_ptr<Band> make_band(const fernsdr::Config& config) {
    const auto& section = config.section("band:test");
    std::string error;
    auto source = fernsdr::make_source(section, error);
    if (!source) return nullptr;
    return std::make_unique<Band>("test", "Test Band", std::move(source), section);
}

double amplitude_at(const std::vector<float>& x, double freq, double rate, size_t start, size_t count) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double a = 2.0 * M_PI * freq * static_cast<double>(i + start) / rate;
        re += x[i + start] * std::cos(a);
        im += x[i + start] * std::sin(a);
    }
    return 2.0 * std::sqrt(re * re + im * im) / count;
}

struct Decoded {
    std::vector<float> audio;
    std::vector<std::vector<float>> waterfall_lines;
    std::vector<double> waterfall_low;
    std::vector<double> waterfall_high;
    int audio_frames = 0;
    size_t audio_bytes = 0;
    size_t waterfall_bytes = 0;
};

double read_f64(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | p[i];
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Acts as the client: decodes exactly what would go down the WebSocket.
Decoded decode_stream(const std::vector<std::vector<uint8_t>>& messages, int audio_rate) {
    Decoded out;
    fernsdr::nac::Decoder audio_decoder(audio_rate);
    fernsdr::wfc::LineDecoder line_decoder;
    std::vector<float> frame(fernsdr::nac::kFrameHop);

    for (const auto& message : messages) {
        if (message.empty()) continue;
        if (message[0] == fernsdr::proto::kStreamAudio) {
            out.audio_bytes += message.size();
            audio_decoder.decode(message.data() + fernsdr::proto::kAudioHeaderBytes,
                                 message.size() - fernsdr::proto::kAudioHeaderBytes, frame.data(), (message[1] & 2) != 0);
            out.audio.insert(out.audio.end(), frame.begin(), frame.end());
            out.audio_frames++;
        } else if (message[0] == fernsdr::proto::kStreamWaterfall) {
            out.waterfall_bytes += message.size();
            const double low = read_f64(message.data() + 4);
            const double high = read_f64(message.data() + 12);
            const size_t width = static_cast<size_t>(message[20]) | (static_cast<size_t>(message[21]) << 8);
            std::vector<float> line(width);
            if (line_decoder.decode(message.data() + fernsdr::proto::kWaterfallHeaderBytes,
                                    message.size() - fernsdr::proto::kWaterfallHeaderBytes, width,
                                    line.data(), (message[1] & 1) != 0, (message[1] & 2) != 0)) {
                out.waterfall_lines.push_back(std::move(line));
                out.waterfall_low.push_back(low);
                out.waterfall_high.push_back(high);
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE(calibration_peak_bounds_frequencies_before_converting_bins) {
    auto band = make_band(make_config());
    CHECK(band != nullptr);
    if (!band) return;
    for (int i = 0; i < 40; i++) CHECK(band->process_one_block());
    const float peak = band->peak_dbfs(kBandCenter, kBandRate);
    CHECK(peak > -100);
    CHECK_NEAR(band->peak_dbfs(kBandCenter, 1e300), peak, 0);
    for (double hz : {1e300, -1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_EQ(band->peak_dbfs(hz, 1000), -160);
    }
    for (double width : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_EQ(band->peak_dbfs(kBandCenter, width), -160);
    }
}

TEST_CASE(integration_narrow_cw_rejects_a_neighbour_through_agc_and_codec) {
    class ToneSource final : public fernsdr::Source {
    public:
        explicit ToneSource(double offset) : step_(2 * M_PI * offset / kBandRate) {}
        bool start(std::string&) override { return true; }
        void stop() override {}
        fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
        bool read(fernsdr::cfloat* out, size_t count) override {
            for (size_t i = 0; i < count; i++) {
                out[i] = std::polar(0.25f, static_cast<float>(phase_));
                phase_ = std::remainder(phase_ + step_, 2 * M_PI);
            }
            return true;
        }
        double sample_rate() const override { return kBandRate; }
        double center_hz() const override { return kBandCenter; }
        fernsdr::SourceStats stats() const override { return {}; }
        const char* kind_name() const override { return "test-tone"; }
    private:
        double phase_ = 0, step_;
    };
    for (double sign : {-1.0, 1.0}) for (bool excluded : {false, true}) {
        fernsdr::ConfigSection section("band:test");
        const double pitch = sign * 700;
        auto source = std::make_unique<ToneSource>(pitch + (excluded ? sign * 150 : 0));
        Band band("test", "test", std::move(source), section);
        auto listener = std::make_shared<Listener>(1, band);
        auto channel = listener->channel();
        channel.frequency_hz = kBandCenter;
        channel.mode = sign > 0 ? fernsdr::Mode::Cw : fernsdr::Mode::CwL;
        channel.bandwidth_low = pitch - 50;
        channel.bandwidth_high = pitch + 50;
        channel.agc = fernsdr::AgcProfile::Fast;
        listener->set_channel(channel);
        auto view = listener->viewport();
        view.enabled = false;
        listener->set_viewport(view);
        band.add_listener(listener);
        std::vector<std::vector<uint8_t>> messages;
        for (size_t block = 0; block < 400; block++) {
            CHECK(band.process_one_block());
            if (block < 200) messages.clear();
            listener->drain(messages);
        }
        const auto decoded = decode_stream(messages, listener->actual_audio_rate());
        // About a second, at whatever rate the narrow passband was given.
        CHECK(static_cast<double>(decoded.audio.size()) > 0.9 * listener->actual_audio_rate());
        double power = 0;
        for (float sample : decoded.audio) power += sample * sample;
        const double rms = std::sqrt(power / decoded.audio.size());
        // A tone's RMS at the gain control's target is its peak, and the
        // demodulator keeps the real part.
        if (excluded) CHECK_NEAR(rms, 0, 0.005);
        else CHECK_NEAR(rms, fernsdr::Agc::kTarget / std::sqrt(2.0), 0.01);
    }
}

TEST_CASE(integration_agc_holds_an_empty_channel_under_its_target) {
    // Band noise and nothing else, through the band, a listener's channel,
    // the AGC and the codec. The AGC has to be told how much noise the
    // channel carries, from the band's transform around it, in the units the
    // channel's samples come in; wrong by a factor anywhere on the way and the
    // noise lands at the wrong level here, and nowhere else would say so.
    class NoiseSource final : public fernsdr::Source {
    public:
        bool start(std::string&) override { return true; }
        void stop() override {}
        fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
        bool read(fernsdr::cfloat* out, size_t count) override {
            for (size_t i = 0; i < count; i++) out[i] = fernsdr::cfloat(noise_(rng_), noise_(rng_));
            return true;
        }
        double sample_rate() const override { return kBandRate; }
        double center_hz() const override { return kBandCenter; }
        fernsdr::SourceStats stats() const override { return {}; }
        const char* kind_name() const override { return "test-noise"; }
    private:
        std::mt19937 rng_{41};
        std::normal_distribution<float> noise_{0.0f, 0.003f};
    };
    const auto audio_rms = [](fernsdr::AgcProfile profile) {
        fernsdr::ConfigSection section("band:test");
        Band band("test", "test", std::make_unique<NoiseSource>(), section);
        auto listener = std::make_shared<Listener>(1, band);
        auto channel = listener->channel();
        channel.frequency_hz = kBandCenter + 12000.0;
        channel.mode = fernsdr::Mode::Usb;
        channel.bandwidth_low = 300.0;
        channel.bandwidth_high = 2700.0;
        channel.agc = profile;
        listener->set_channel(channel);
        auto view = listener->viewport();
        view.enabled = false;
        listener->set_viewport(view);
        band.add_listener(listener);
        std::vector<std::vector<uint8_t>> messages;
        for (size_t block = 0; block < 600; block++) {
            CHECK(band.process_one_block());
            if (block < 300) messages.clear();
            listener->drain(messages);
        }
        const auto decoded = decode_stream(messages, listener->actual_audio_rate());
        double power = 0.0;
        for (float sample : decoded.audio) power += static_cast<double>(sample) * sample;
        return std::sqrt(power / std::max<size_t>(decoded.audio.size(), 1));
    };
    // The noise sits 15 dB under the target, whichever the profile; the
    // demodulator keeps the real part, half the power of complex noise. The
    // peak-following profiles before these brought an empty channel up until
    // its noise peaks reached the target, about 10 dB louder.
    const double expected = fernsdr::Agc::kTarget * fernsdr::Agc::kNoiseUnderTarget / std::sqrt(2.0);
    for (const auto profile : {fernsdr::AgcProfile::Fast, fernsdr::AgcProfile::Auto, fernsdr::AgcProfile::Long}) {
        CHECK_NEAR(20.0 * std::log10(audio_rms(profile) / expected), 0.0, 1.5);
    }
}

TEST_CASE(integration_carrier_arrives_as_audio_at_the_expected_pitch) {
    auto config = make_config();
    auto band = make_band(config);
    CHECK(band != nullptr);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);

    // Tune USB 1000 Hz below the carrier, so it lands at 1000 Hz of audio.
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter + kCarrierOffset - 1000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.bandwidth_low = 300.0;
    channel.bandwidth_high = 2800.0;
    channel.agc = fernsdr::AgcProfile::Off;  // keep amplitudes meaningful
    channel.manual_gain_db = 0.0f;
    channel.audio_bitrate = 48000;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;  // audio only for this case
    listener->set_viewport(viewport);

    band->add_listener(listener);

    std::string error;
    CHECK(band->process_one_block());
    for (int i = 0; i < 120; i++) CHECK(band->process_one_block());

    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    CHECK(!messages.empty());

    const int rate = listener->actual_audio_rate();
    CHECK(rate > 8000 && rate <= 16000);

    auto decoded = decode_stream(messages, rate);
    CHECK(decoded.audio_frames > 50);

    // Skip the priming frames, then look for the tone.
    const size_t start = fernsdr::nac::kFrameHop * 20;
    const size_t count = decoded.audio.size() - start - fernsdr::nac::kFrameHop;
    CHECK(count > 1000);

    const double at_tone = amplitude_at(decoded.audio, 1000.0, rate, start, count);
    // The source's carrier amplitude is 0.20; SSB takes the real part, so the
    // audio amplitude should match it closely.
    CHECK(at_tone > 0.12);
    CHECK(at_tone < 0.30);

    // And nothing significant anywhere else in the passband.
    CHECK(amplitude_at(decoded.audio, 2000.0, rate, start, count) < at_tone * 0.2);
    CHECK(amplitude_at(decoded.audio, 500.0, rate, start, count) < at_tone * 0.2);
}

TEST_CASE(integration_listener_joining_an_idle_band_hears_the_carrier_cleanly) {
    // An idle band leaves its channelizer untransformed and only counts the
    // blocks. A listener arriving after an odd number of them, on an odd or
    // an even centre bin, must hear what it would have heard had the band
    // been busy all along.
    auto config = make_config();
    auto band = make_band(config);
    CHECK(band != nullptr);
    if (!band) return;
    const double bin_hz = band->channelizer().bin_hz();
    for (const double nudge : {0.0, bin_hz}) {
        for (int i = 0; i < 37; i++) CHECK(band->process_one_block());

        auto listener = std::make_shared<Listener>(static_cast<uint64_t>(10 + nudge), *band);
        fernsdr::ChannelSettings channel;
        channel.frequency_hz = kBandCenter + kCarrierOffset - 1000.0 - nudge;
        channel.mode = fernsdr::Mode::Usb;
        channel.bandwidth_low = 300.0;
        channel.bandwidth_high = 2800.0;
        channel.agc = fernsdr::AgcProfile::Off;
        channel.manual_gain_db = 0.0f;
        channel.audio_bitrate = 48000;
        listener->set_channel(channel);
        fernsdr::ViewportSettings viewport;
        viewport.enabled = false;
        listener->set_viewport(viewport);
        band->add_listener(listener);
        for (int i = 0; i < 120; i++) CHECK(band->process_one_block());
        band->remove_listener(listener->id());

        std::vector<std::vector<uint8_t>> messages;
        listener->drain(messages);
        const int rate = listener->actual_audio_rate();
        auto decoded = decode_stream(messages, rate);
        const size_t start = fernsdr::nac::kFrameHop * 20;
        CHECK(decoded.audio.size() > start + 2000);
        if (decoded.audio.size() <= start + 2000) continue;
        const size_t count = decoded.audio.size() - start - fernsdr::nac::kFrameHop;
        const double tone = 1000.0 + nudge;
        const double at_tone = amplitude_at(decoded.audio, tone, rate, start, count);
        CHECK(at_tone > 0.12 && at_tone < 0.30);
        CHECK(amplitude_at(decoded.audio, tone + 700.0, rate, start, count) < at_tone * 0.2);
        CHECK(amplitude_at(decoded.audio, tone - 500.0, rate, start, count) < at_tone * 0.2);
    }
}

TEST_CASE(integration_first_listener_on_an_idle_band_gets_a_line_at_once) {
    // An idle band makes a few lines a second. Its first listener must get
    // its first waterfall row about as soon as one joining a busy band does,
    // not after the rest of an idle stretch.
    const auto blocks_to_first_row = [](bool busy) {
        auto band = make_band(make_config());
        CHECK(band != nullptr);
        if (!band) return -1;
        auto keeper = std::make_shared<Listener>(99, *band);
        if (busy) band->add_listener(keeper);
        for (int i = 0; i < 90; i++) CHECK(band->process_one_block());
        auto listener = std::make_shared<Listener>(3, *band);
        fernsdr::ChannelSettings channel;
        channel.audio_enabled = false;
        listener->set_channel(channel);
        band->add_listener(listener);
        int blocks = 0;
        bool row = false;
        while (!row && blocks < 200) {
            CHECK(band->process_one_block());
            blocks++;
            std::vector<std::vector<uint8_t>> frames;
            listener->drain(frames);
            for (const auto& frame : frames)
                row = row || (!frame.empty() && frame[0] == fernsdr::proto::kStreamWaterfall);
        }
        CHECK(row);
        return blocks;
    };
    const int busy = blocks_to_first_row(true), idle = blocks_to_first_row(false);
    auto band = make_band(make_config());
    if (!band) return;
    // At most one line of averages later (a line is 40 ms at 25 a second).
    const int line_blocks = static_cast<int>(std::ceil(1.0 / 25.0 / band->channelizer().block_seconds()));
    CHECK(busy > 0);
    CHECK(idle <= busy + line_blocks);
}

TEST_CASE(integration_waterfall_shows_the_carrier_at_the_right_frequency) {
    auto config = make_config();
    auto band = make_band(config);
    CHECK(band != nullptr);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);

    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter;
    channel.audio_enabled = false;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = true;
    viewport.low_hz = kBandCenter - 20000.0;
    viewport.high_hz = kBandCenter + 20000.0;
    viewport.width = 1024;
    viewport.lines_per_second = 20.0;
    listener->set_viewport(viewport);

    band->add_listener(listener);
    for (int i = 0; i < 200; i++) CHECK(band->process_one_block());

    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    auto decoded = decode_stream(messages, 12000);
    CHECK(decoded.waterfall_lines.size() > 5);
    if (decoded.waterfall_lines.empty()) return;

    // The span must travel with the line.
    CHECK_NEAR(decoded.waterfall_low.back(), kBandCenter - 20000.0, 1.0);
    CHECK_NEAR(decoded.waterfall_high.back(), kBandCenter + 20000.0, 1.0);

    const auto& line = decoded.waterfall_lines.back();
    size_t peak = 0;
    for (size_t i = 1; i < line.size(); i++) {
        if (line[i] > line[peak]) peak = i;
    }

    const double span = decoded.waterfall_high.back() - decoded.waterfall_low.back();
    const double peak_hz = decoded.waterfall_low.back() + span * (peak + 0.5) / line.size();
    // The strongest thing in +/-20 kHz of centre is the carrier at -800 Hz.
    CHECK_NEAR(peak_hz - kBandCenter, kCarrierOffset, span / line.size() * 2.0);
}

TEST_CASE(zoomed_waterfall_centres_bin_aligned_tones_at_their_physical_frequency) {
    for (const std::string kind : {"iq", "real"}) {
        fernsdr::ConfigSection section("band:test");
        section.set("source", "test");
        section.set("sample_rate", "204800");
        section.set("center", "7100000");
        section.set("signal", kind);
        section.set("spectrum_bins", "4096");
        section.set("noise", "0");
        section.set("realtime", "false");
        std::string error;
        Band band("test", "Tone alignment", fernsdr::make_source(section, error), section);
        const double tone_hz = band.center_hz() - 800;
        auto listener = std::make_shared<Listener>(1, band);
        auto channel = listener->channel();
        channel.audio_enabled = false;
        listener->set_channel(channel);
        auto view = listener->viewport();
        view.low_hz = tone_hz - 1000;
        view.high_hz = tone_hz + 1000;
        view.width = 1001;
        view.lines_per_second = 20;
        listener->set_viewport(view);
        band.add_listener(listener);
        for (int i = 0; i < 40; ++i) CHECK(band.process_one_block());
        std::vector<std::vector<uint8_t>> frames;
        listener->drain(frames);
        const auto decoded = decode_stream(frames, listener->actual_audio_rate());
        CHECK(!decoded.waterfall_lines.empty());
        if (decoded.waterfall_lines.empty()) continue;
        const auto& line = decoded.waterfall_lines.back();
        const float peak = *std::max_element(line.begin(), line.end());
        double centre = 0;
        size_t count = 0;
        for (size_t i = 0; i < line.size(); ++i) if (line[i] == peak) {
            centre += i + 0.5;
            ++count;
        }
        const double pixel_hz = (decoded.waterfall_high.back() - decoded.waterfall_low.back()) / line.size();
        const double displayed_hz = decoded.waterfall_low.back() + pixel_hz * centre / count;
        CHECK_NEAR(displayed_hz, tone_hz, pixel_hz);
    }
}

TEST_CASE(native_waterfall_falls_back_before_clipping_changes_a_visible_slope) {
    auto band = make_band(make_config());
    CHECK(band != nullptr);
    if (!band) return;
    CHECK(band->process_one_block());
    for (const float left : {-300.0f, -160.0f}) {
        Listener listener(1, *band);
        auto channel = listener.channel();
        channel.audio_enabled = false;
        listener.set_channel(channel);
        auto view = listener.viewport();
        view.low_hz = kBandCenter - 1;
        view.high_hz = kBandCenter + 1;
        view.width = 256;
        view.native_grid = view.adaptive_codec = view.zero_runs = true;
        listener.set_viewport(view);
        std::vector<float> source(16, -100);
        source[7] = left;
        source[8] = -40;
        fernsdr::SpectrumPyramid spectrum;
        spectrum.build(source.data(), source.size(), kBandCenter - 80, kBandCenter + 80);
        listener.process_block(band->channelizer(), &spectrum);
        std::vector<std::vector<uint8_t>> frames;
        listener.drain(frames);
        CHECK_EQ(frames.size(), 1);
        if (frames.empty()) continue;
        CHECK_EQ(frames[0][0], fernsdr::proto::kStreamWaterfall);
        CHECK_EQ((frames[0][1] & 4) != 0, left >= -200);
        const auto decoded = decode_stream(frames, 12000);
        CHECK_EQ(decoded.waterfall_lines.size(), 1);
        if (decoded.waterfall_lines.empty()) continue;
        const auto& levels = decoded.waterfall_lines[0];
        CHECK_EQ(levels.size(), left >= -200 ? 2 : 256);
        if (left < -200) {
            CHECK_NEAR(levels[128], -170, 1);
            CHECK_NEAR(decoded.waterfall_low[0], view.low_hz, 0);
            CHECK_NEAR(decoded.waterfall_high[0], view.high_hz, 0);
        }
    }
}

TEST_CASE(integration_per_user_bandwidth_stays_within_budget) {
    // The constraint the whole design exists to satisfy.  A user is allowed to
    // ask for a fast, wide waterfall; the server's job is to keep the combined
    // stream inside the budget anyway, by slowing the waterfall rather than by
    // touching the audio.
    auto config = make_config();
    auto band = make_band(config);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    listener->set_bitrate_budget(100000);

    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter + kCarrierOffset - 1000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.audio_bitrate = 48000;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = true;
    viewport.low_hz = band->low_hz();
    viewport.high_hz = band->high_hz();
    viewport.width = 1024;
    viewport.lines_per_second = 30.0;  // deliberately more than the budget allows
    listener->set_viewport(viewport);

    band->add_listener(listener);

    const size_t blocks = 600;
    for (size_t i = 0; i < blocks; i++) CHECK(band->process_one_block());

    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    auto decoded = decode_stream(messages, listener->actual_audio_rate());

    const double seconds = static_cast<double>(blocks) * band->channelizer().block_seconds();
    const double audio_kbps = decoded.audio_bytes * 8.0 / seconds / 1000.0;
    const double waterfall_kbps = decoded.waterfall_bytes * 8.0 / seconds / 1000.0;

    // Audio is untouched by the limiter.
    CHECK(audio_kbps > 40.0);
    CHECK(audio_kbps < 52.0);
    // And the total is held to the budget, with a little slack for the
    // limiter's start-up before it has measured a line.
    CHECK(audio_kbps + waterfall_kbps < 110.0);
    CHECK(decoded.waterfall_lines.size() > 5);

    // The reduction is reported rather than silently applied.
    const auto telemetry = listener->telemetry();
    CHECK(telemetry.waterfall_lines_per_second < 30.0);
    CHECK(telemetry.waterfall_lines_per_second > 0.0);
}

TEST_CASE(integration_generous_budget_delivers_the_requested_line_rate) {
    auto config = make_config();
    auto band = make_band(config);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    listener->set_bitrate_budget(400000);  // plenty

    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter;
    channel.audio_bitrate = 48000;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = true;
    viewport.low_hz = band->low_hz();
    viewport.high_hz = band->high_hz();
    viewport.width = 512;
    viewport.lines_per_second = 10.0;
    listener->set_viewport(viewport);
    band->add_listener(listener);

    const size_t blocks = 600;
    for (size_t i = 0; i < blocks; i++) CHECK(band->process_one_block());
    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    auto decoded = decode_stream(messages, listener->actual_audio_rate());

    const double seconds = static_cast<double>(blocks) * band->channelizer().block_seconds();
    const double lines_per_second = decoded.waterfall_lines.size() / seconds;
    CHECK(lines_per_second > 8.0);
    CHECK(lines_per_second < 11.0);
}

TEST_CASE(integration_control_broadcasts_reduce_waterfall_allowance_without_changing_audio) {
    auto config = make_config();
    auto band = make_band(config);
    CHECK(band != nullptr);
    if (!band) return;
    auto baseline = std::make_shared<Listener>(1, *band);
    auto with_control = std::make_shared<Listener>(2, *band);
    for (auto& listener : {baseline, with_control}) {
        listener->set_bitrate_budget(100000);
        fernsdr::ViewportSettings view;
        view.low_hz = band->low_hz();
        view.high_hz = band->high_hz();
        view.width = 1024;
        view.lines_per_second = 30;
        listener->set_viewport(view);
        band->add_listener(listener);
    }
    size_t audio_bytes[2]{}, waterfall_bytes[2]{};
    const double block_seconds = band->channelizer().block_seconds();
    const size_t control_bytes = static_cast<size_t>(std::lround(40000 * block_seconds / 8));
    for (int block = 0; block < 900; block++) {
        with_control->account_control_bytes(control_bytes);
        CHECK(band->process_one_block());
        int index = 0;
        for (auto& listener : {baseline, with_control}) {
            std::vector<std::vector<uint8_t>> messages;
            listener->drain(messages);
            for (const auto& message : messages) {
                if (block < 200) continue;
                if (message[0] == fernsdr::proto::kStreamAudio) audio_bytes[index] += message.size();
                if (message[0] == fernsdr::proto::kStreamWaterfall) waterfall_bytes[index] += message.size();
            }
            index++;
        }
    }
    CHECK(audio_bytes[0] > 0);
    CHECK_EQ(audio_bytes[0], audio_bytes[1]);
    CHECK(waterfall_bytes[1] > 0);
    CHECK(waterfall_bytes[1] < waterfall_bytes[0] / 2);
}

TEST_CASE(integration_retuning_mid_stream_moves_the_audio) {
    auto config = make_config();
    auto band = make_band(config);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter + kCarrierOffset - 1000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.agc = fernsdr::AgcProfile::Off;
    listener->set_channel(channel);
    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band->add_listener(listener);

    for (int i = 0; i < 100; i++) band->process_one_block();
    std::vector<std::vector<uint8_t>> first;
    listener->drain(first);

    // Retune so the same carrier lands at 2000 Hz instead of 1000 Hz.
    channel.frequency_hz = kBandCenter + kCarrierOffset - 2000.0;
    listener->set_channel(channel);
    for (int i = 0; i < 100; i++) band->process_one_block();
    std::vector<std::vector<uint8_t>> second;
    listener->drain(second);

    const int rate = listener->actual_audio_rate();
    auto before = decode_stream(first, rate);
    auto after = decode_stream(second, rate);

    const size_t start = fernsdr::nac::kFrameHop * 30;
    CHECK(before.audio.size() > start + 2000);
    CHECK(after.audio.size() > start + 2000);
    const size_t count = 2000;

    CHECK(amplitude_at(before.audio, 1000.0, rate, start, count) >
          amplitude_at(before.audio, 2000.0, rate, start, count) * 3.0);
    CHECK(amplitude_at(after.audio, 2000.0, rate, start, count) >
          amplitude_at(after.audio, 1000.0, rate, start, count) * 3.0);
}

TEST_CASE(integration_leaving_nac3_mid_packet_keeps_the_frame_clock) {
    // The switch lands at every phase of a four-frame packet in turn, so at
    // least some of them find a packet half built. Its frames already hold
    // sequence numbers, and dropping them would read as loss to the client.
    auto config = make_config();
    int partial_switches = 0;
    for (int phase = 0; phase < 8; phase++) {
        auto band = make_band(config);
        if (!band) return;
        auto listener = std::make_shared<Listener>(1, *band);
        fernsdr::ChannelSettings channel;
        channel.frequency_hz = kBandCenter + kCarrierOffset - 1000.0;
        channel.mode = fernsdr::Mode::Usb;
        channel.packet_audio = true;
        channel.audio_packet_frames = 4;
        listener->set_channel(channel);
        fernsdr::ViewportSettings viewport;
        viewport.enabled = false;
        listener->set_viewport(viewport);
        band->add_listener(listener);

        for (int i = 0; i < 40 + phase; i++) CHECK(band->process_one_block());
        std::vector<std::vector<uint8_t>> before;
        listener->drain(before);
        channel.packet_audio = false;
        listener->set_channel(channel);
        for (int i = 0; i < 20; i++) CHECK(band->process_one_block());
        std::vector<std::vector<uint8_t>> after;
        listener->drain(after);

        int expected = -1, packets = 0, plain = 0, short_packets = 0;
        auto walk = [&](const std::vector<std::vector<uint8_t>>& messages, bool switched) {
            for (const auto& message : messages) {
                if (message.size() < 5 || message[0] != fernsdr::proto::kStreamAudio) continue;
                const int sequence = message[2] | (message[3] << 8);
                const int count = fernsdr::proto::audio_frames(message.data(), message.size());
                if (expected >= 0) CHECK_EQ(sequence, expected);
                CHECK_EQ(message[1] & fernsdr::proto::kAudioFlagDiscontinuity, 0);
                expected = (sequence + count) & 0xffff;
                if (message[1] & fernsdr::proto::kAudioFlagPacket) {
                    packets++;
                    if (switched && count < 4) short_packets++;
                } else {
                    plain++;
                }
            }
        };
        walk(before, false);
        walk(after, true);
        CHECK(packets >= 4);
        CHECK(plain >= 5);
        partial_switches += short_packets;
    }
    CHECK(partial_switches > 0);
}

TEST_CASE(integration_am_signal_demodulates) {
    // The test source carries an AM signal 60 kHz below centre with a 1 kHz
    // tone at 60% depth.
    auto config = make_config();
    auto band = make_band(config);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = kBandCenter - 60000.0;
    channel.mode = fernsdr::Mode::Am;
    channel.bandwidth_low = -4000.0;
    channel.bandwidth_high = 4000.0;
    channel.agc = fernsdr::AgcProfile::Off;
    listener->set_channel(channel);
    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band->add_listener(listener);

    for (int i = 0; i < 200; i++) band->process_one_block();
    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);

    const int rate = listener->actual_audio_rate();
    auto decoded = decode_stream(messages, rate);
    const size_t start = fernsdr::nac::kFrameHop * 40;
    CHECK(decoded.audio.size() > start + 4000);
    const double tone = amplitude_at(decoded.audio, 1000.0, rate, start, 4000);
    // Carrier 0.30, depth 0.6 -> audio amplitude around 0.18.
    CHECK(tone > 0.08);
    CHECK(tone < 0.35);
}

TEST_CASE(integration_band_thread_starts_and_stops_cleanly) {
    auto config = make_config(/*realtime=*/true);
    auto band = make_band(config);
    CHECK(band != nullptr);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    band->add_listener(listener);

    std::string error;
    CHECK(band->start(error));
    CHECK(band->running());
    CHECK_EQ(band->listener_count(), 1);

    // Let it produce a little, then take it down.
    for (int i = 0; i < 40 && listener->queued_bytes() == 0; i++) {
        struct timespec ts {0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    CHECK(listener->queued_bytes() > 0);

    band->remove_listener(1);
    CHECK_EQ(band->listener_count(), 0);
    band->stop();
    CHECK(!band->running());

    auto info = band->info();
    CHECK(info.id == "test");
    CHECK_NEAR(info.center_hz, kBandCenter, 1.0);
    CHECK(info.source_kind == std::string("test"));
}

// --- real-input front ends --------------------------------------------------

namespace {

fernsdr::Config make_real_config() {
    fernsdr::Config config;
    std::string error;
    config.parse(
        "[band:real]\n"
        "source = test\n"
        "signal = real\n"
        "format = s16\n"
        "sample_rate = 2048k\n"
        "noise = 0.001\n"
        "realtime = false\n",
        error);
    return config;
}

std::unique_ptr<Band> make_real_band(const fernsdr::Config& config) {
    const auto& section = config.section("band:real");
    std::string error;
    auto source = fernsdr::make_source(section, error);
    if (!source) return nullptr;
    return std::make_unique<Band>("real", "Real Band", std::move(source), section);
}

}  // namespace

TEST_CASE(integration_real_band_covers_dc_to_nyquist) {
    auto config = make_real_config();
    auto band = make_real_band(config);
    CHECK(band != nullptr);
    if (!band) return;

    // A direct-sampling front end starts at DC and reaches half the sample
    // rate; nothing above Nyquist is real spectrum.
    CHECK_NEAR(band->low_hz(), 0.0, 1.0);
    CHECK(band->high_hz() > 900000.0);
    CHECK(band->high_hz() <= 1024000.0);
    CHECK(band->info().signal == "real");
}

namespace {

std::unique_ptr<Band> make_iq_band(const std::string& extra, fernsdr::Config& config) {
    std::string error;
    config.parse("[band:hf]\nsource = test\nsample_rate = 10M\nrealtime = false\n" + extra, error);
    const auto& section = config.section("band:hf");
    if (!fernsdr::check_band_settings(section, *fernsdr::make_source(section, error), error)) return nullptr;
    auto source = fernsdr::make_source(section, error);
    if (!source) return nullptr;
    return std::make_unique<Band>("hf", "HF", std::move(source), section);
}

}  // namespace

// Below 0 Hz an IQ input shows nothing but the mirror of what lies just
// above it: an SDRplay at 4 MHz with 10 Msps covers -1 to 9 MHz on paper.
TEST_CASE(integration_an_iq_band_stops_at_zero_hertz) {
    {
        fernsdr::Config config;
        auto band = make_iq_band("center = 4M\nusable_fraction = 1\n", config);
        CHECK(band != nullptr);
        if (band) {
            CHECK_NEAR(band->low_hz(), 0.0, 1.0);
            CHECK_NEAR(band->high_hz(), 9e6, 1.0);
        }
    }
    {
        // What the panel writes for shortwave on a 10 Msps radio.
        fernsdr::Config config;
        auto band = make_iq_band("center = 5025k\nlow = 25000\nhigh = 9700k\n", config);
        CHECK(band != nullptr);
        if (band) {
            CHECK_NEAR(band->low_hz(), 25000.0, 1.0);
            CHECK_NEAR(band->high_hz(), 9.7e6, 1.0);
        }
    }
    {
        // A negative low is not.
        fernsdr::Config config;
        CHECK(make_iq_band("center = 4M\nlow = -500k\nhigh = 8M\n", config) == nullptr);
    }
    {
        // Behind an upconverter, 0 Hz on the dial is still the floor.
        fernsdr::Config config;
        auto band = make_iq_band("center = 128M\nfrequency_offset = -125M\nusable_fraction = 1\n", config);
        CHECK(band != nullptr);
        if (band) {
            CHECK_NEAR(band->low_hz(), 0.0, 1.0);
            CHECK_NEAR(band->high_hz(), 8e6, 1.0);
        }
    }
    {
        // The tester's SDRplay band, as written by hand: all of 0 to 10 MHz.
        fernsdr::Config config;
        auto band = make_iq_band("center = 5M\n", config);
        CHECK(band != nullptr);
        if (band) {
            CHECK_NEAR(band->low_hz(), 0.0, 1.0);
            CHECK_NEAR(band->high_hz(), 10e6, 1.0);
        }
    }
    {
        // Well above 0 Hz nothing changes.
        fernsdr::Config config;
        auto band = make_iq_band("center = 14200k\n", config);
        CHECK(band != nullptr);
        if (band) CHECK_NEAR(band->low_hz(), 14.2e6 - 5e6, 1.0);
    }
}

TEST_CASE(integration_real_band_demodulates_a_signal) {
    auto config = make_real_config();
    auto band = make_real_band(config);
    if (!band) return;

    // The synthetic source places its signals around a quarter of the sample
    // rate; the steady carrier sits 800 Hz below that.
    const double carrier = 2048000.0 * 0.25 - 800.0;

    auto listener = std::make_shared<Listener>(1, *band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = carrier - 1000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.bandwidth_low = 300.0;
    channel.bandwidth_high = 2700.0;
    channel.agc = fernsdr::AgcProfile::Off;
    channel.manual_gain_db = 0.0f;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band->add_listener(listener);

    for (int i = 0; i < 240; i++) CHECK(band->process_one_block());

    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    const int rate = listener->actual_audio_rate();
    auto decoded = decode_stream(messages, rate);

    const size_t start = fernsdr::nac::kFrameHop * 30;
    CHECK(decoded.audio.size() > start + 4000);
    const double at_tone = amplitude_at(decoded.audio, 1000.0, rate, start, 4000);
    // The synthetic carrier's amplitude is 0.20, and a real front end's
    // analytic scaling must recover it in full rather than halving it.
    CHECK(at_tone > 0.12);
    CHECK(at_tone < 0.32);
}

TEST_CASE(integration_real_band_waterfall_spans_the_right_range) {
    auto config = make_real_config();
    auto band = make_real_band(config);
    if (!band) return;

    auto listener = std::make_shared<Listener>(1, *band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = band->center_hz();
    channel.audio_enabled = false;
    listener->set_channel(channel);

    fernsdr::ViewportSettings viewport;
    viewport.enabled = true;
    viewport.low_hz = band->low_hz();
    viewport.high_hz = band->high_hz();
    viewport.width = 512;
    viewport.lines_per_second = 20.0;
    listener->set_viewport(viewport);
    band->add_listener(listener);

    for (int i = 0; i < 300; i++) CHECK(band->process_one_block());
    std::vector<std::vector<uint8_t>> messages;
    listener->drain(messages);
    auto decoded = decode_stream(messages, 16000);
    CHECK(decoded.waterfall_lines.size() > 3);
    if (decoded.waterfall_lines.empty()) return;

    const auto& line = decoded.waterfall_lines.back();
    size_t peak = 0;
    for (size_t i = 1; i < line.size(); i++) {
        if (line[i] > line[peak]) peak = i;
    }
    const double span = decoded.waterfall_high.back() - decoded.waterfall_low.back();
    const double peak_hz = decoded.waterfall_low.back() + span * (peak + 0.5) / line.size();
    // The strongest synthetic signal is the AM carrier 60 kHz below centre.
    CHECK_NEAR(peak_hz, 2048000.0 * 0.25 - 60000.0, span / line.size() * 3.0);
}

TEST_CASE(band_spectrum_snapshot_keeps_the_strongest_level_in_each_cell) {
    auto band = make_band(make_config());
    if (!band) return;
    std::vector<float> levels;
    double low = 0.0, high = 0.0;
    CHECK(!band->spectrum_snapshot(64, levels, low, high));
    for (int i = 0; i < 200; i++) CHECK(band->process_one_block());
    CHECK(band->spectrum_snapshot(64, levels, low, high));
    CHECK_EQ(levels.size(), 64);
    CHECK(high > low);
    // The test source has a carrier; reduced to 64 cells it must still stand
    // well clear of the floor rather than be averaged into it.
    const float peak = *std::max_element(levels.begin(), levels.end());
    std::vector<float> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    CHECK(peak - sorted[sorted.size() / 2] > 20.0f);
}

TEST_CASE(band_keeps_its_recent_spectrum_for_the_admin_waterfall) {
    auto band = make_band(make_config());
    if (!band) return;
    std::vector<uint8_t> rows;
    int64_t age_ms = 0;
    CHECK_EQ(band->recent_spectrum(rows, age_ms), 0);
    CHECK_EQ(age_ms, -1);

    for (int i = 0; i < 200; i++) CHECK(band->process_one_block());
    // A line every interval of wall time, however fast the blocks went.
    const size_t first = band->recent_spectrum(rows, age_ms);
    CHECK(first >= 1);
    CHECK_EQ(rows.size(), first * Band::kRecentBins);
    CHECK(age_ms >= 0 && age_ms < 5000);
    // Quantised like the archive, with the carrier standing clear of the floor.
    std::vector<uint8_t> sorted = rows;
    std::sort(sorted.begin(), sorted.end());
    const float peak = fernsdr::WaterfallArchive::dequantise(sorted.back());
    const float median = fernsdr::WaterfallArchive::dequantise(sorted[sorted.size() / 2]);
    CHECK(peak - median > 20.0f);

    std::this_thread::sleep_for(std::chrono::milliseconds(Band::kRecentIntervalMs + 50));
    // Nobody listens, so the band makes a line every quarter interval of
    // input; run a whole interval's worth so one of them falls due.
    const double block_seconds = band->channelizer().block_seconds();
    const int blocks = static_cast<int>(Band::kRecentIntervalMs / 1000.0 / block_seconds) + 1;
    for (int i = 0; i < blocks; i++) CHECK(band->process_one_block());
    const size_t later = band->recent_spectrum(rows, age_ms);
    CHECK(later > first && later <= Band::kRecentLines);
    CHECK_EQ(rows.size(), later * Band::kRecentBins);
}

namespace {

// One carrier at a fixed offset from the centre in the samples, with a DC
// offset if asked: what a front end delivers for one clean signal.
class SteadyTone final : public fernsdr::Source {
public:
    SteadyTone(double offset_hz, fernsdr::cfloat dc = {0.0f, 0.0f}) : step_(2 * M_PI * offset_hz / kBandRate), dc_(dc) {}
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++) {
            out[i] = std::polar(0.25f, static_cast<float>(phase_)) + dc_;
            phase_ = std::remainder(phase_ + step_, 2 * M_PI);
        }
        return true;
    }
    double sample_rate() const override { return kBandRate; }
    double center_hz() const override { return kBandCenter; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-tone"; }

private:
    double phase_ = 0, step_;
    fernsdr::cfloat dc_;
};

/** Where the band's spectrum peaks, as the RF frequency of that bin's centre. */
double spectrum_peak_hz(Band& band) {
    std::vector<float> levels;
    double low = 0.0, high = 0.0;
    CHECK(band.spectrum_snapshot(1u << 20, levels, low, high));
    size_t peak = 0;
    for (size_t i = 1; i < levels.size(); i++) if (levels[i] > levels[peak]) peak = i;
    const double width = (high - low) / static_cast<double>(levels.size());
    return low + (static_cast<double>(peak) + 0.5) * width;
}

/** The frequency of the one tone in `audio`, from how fast its phase turns: far finer than an FFT bin. */
double tone_frequency(const std::vector<float>& audio, double rate, double guess, size_t skip) {
    const size_t piece = static_cast<size_t>(rate / 10);
    std::vector<double> phases;
    for (size_t start = skip; start + piece <= audio.size(); start += piece) {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < piece; i++) {
            const double a = -2.0 * M_PI * guess * static_cast<double>(start + i) / rate;
            re += audio[start + i] * std::cos(a);
            im += audio[start + i] * std::sin(a);
        }
        phases.push_back(std::atan2(im, re));
    }
    // Unwrapped, the phase runs in a straight line whose slope is the error.
    double turn = 0.0, slope_sum = 0.0;
    for (size_t k = 1; k < phases.size(); k++) {
        double step = phases[k] - phases[k - 1];
        while (step > M_PI) step -= 2 * M_PI;
        while (step < -M_PI) step += 2 * M_PI;
        turn += step;
    }
    if (phases.size() > 1) slope_sum = turn / static_cast<double>(phases.size() - 1);
    return guess + slope_sum / (2.0 * M_PI) * 10.0;
}

}  // namespace

TEST_CASE(band_ppm_puts_a_carrier_at_its_true_frequency_near_the_band_edge) {
    // A carrier 75 kHz from the centre, exactly on a spectrum bin, from a
    // receiver whose crystal runs 50 ppm fast. Stretching the axis about zero
    // moves it by 50 ppm of its own frequency: 358.75 Hz here, where shifting
    // the band by the error at its centre would be 3.75 Hz short.
    const double offset = 1600.0 * kBandRate / 4096.0;  // 75 kHz
    const double truth = (kBandCenter + offset) * (1.0 + 50e-6);
    for (bool corrected : {false, true}) {
        fernsdr::ConfigSection section("band:test");
        section.set("spectrum_bins", "4096");
        if (corrected) section.set("ppm", "50");
        Band band("test", "test", std::make_unique<SteadyTone>(offset), section);
        for (int i = 0; i < 60; i++) CHECK(band.process_one_block());
        const double shown = spectrum_peak_hz(band);
        if (corrected) CHECK_NEAR(shown, truth, 1.0);
        else CHECK_NEAR(shown, kBandCenter + offset, 1.0);
        CHECK_NEAR(band.info().ppm, corrected ? 50.0 : 0.0, 1e-9);
    }
}

TEST_CASE(band_ppm_tunes_a_listener_to_the_true_frequency) {
    const double offset = 1600.0 * kBandRate / 4096.0;
    const double truth = (kBandCenter + offset) * (1.0 + 50e-6);
    fernsdr::ConfigSection section("band:test");
    section.set("ppm", "50");
    Band band("test", "test", std::make_unique<SteadyTone>(offset), section);
    auto listener = std::make_shared<Listener>(1, band);
    fernsdr::ChannelSettings channel;
    // USB 1 kHz below where the carrier really is: a 1 kHz tone if, and only
    // if, the listener's slice sits where the corrected axis says.
    channel.frequency_hz = truth - 1000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.bandwidth_low = 300.0;
    channel.bandwidth_high = 2800.0;
    channel.agc = fernsdr::AgcProfile::Off;
    channel.audio_bitrate = 64000;
    listener->set_channel(channel);
    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band.add_listener(listener);
    std::vector<std::vector<uint8_t>> messages;
    // 1024 samples a block: about two and a half seconds.
    for (int i = 0; i < 480; i++) {
        CHECK(band.process_one_block());
        listener->drain(messages);
    }
    const auto decoded = decode_stream(messages, listener->actual_audio_rate());
    const double rate = listener->actual_audio_rate();
    CHECK(decoded.audio.size() > rate * 1.5);
    CHECK_NEAR(tone_frequency(decoded.audio, rate, 1000.0, static_cast<size_t>(rate / 2)), 1000.0, 1.0);
}

TEST_CASE(band_dc_removal_and_iq_swap_act_on_what_every_consumer_sees) {
    const double offset = 400.0 * kBandRate / 4096.0;  // 18.75 kHz
    for (int round = 0; round < 3; round++) {
        fernsdr::ConfigSection section("band:test");
        section.set("spectrum_bins", "4096");
        if (round == 1) section.set("dc_remove", "yes");
        if (round == 2) section.set("iq_swap", "yes");
        Band band("test", "test", std::make_unique<SteadyTone>(offset, fernsdr::cfloat(0.1f, -0.08f)), section);
        for (int i = 0; i < 100; i++) CHECK(band.process_one_block());
        const float centre = band.peak_dbfs(kBandCenter, 1.0);
        const float tone = band.peak_dbfs(kBandCenter + (round == 2 ? -offset : offset), 1.0);
        const float mirrored = band.peak_dbfs(kBandCenter + (round == 2 ? offset : -offset), 1.0);
        CHECK(tone - mirrored > 60.0f);
        if (round == 1) CHECK(tone - centre > 50.0f);  // the offset is gone
        else CHECK(tone - centre < 10.0f);             // 0.13 against 0.25: there, a few dB down
    }
}

TEST_CASE(listener_highpass_takes_out_hum_and_leaves_the_voice) {
    // An AM carrier with 100 Hz of hum and a 1 kHz tone on it.
    class HummingCarrier final : public fernsdr::Source {
    public:
        bool start(std::string&) override { return true; }
        void stop() override {}
        fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
        bool read(fernsdr::cfloat* out, size_t count) override {
            for (size_t i = 0; i < count; i++, n_++) {
                const double t = static_cast<double>(n_) / kBandRate;
                const double envelope = 0.25 * (1.0 + 0.3 * std::cos(2 * M_PI * 100.0 * t) + 0.3 * std::cos(2 * M_PI * 1000.0 * t));
                out[i] = std::polar(static_cast<float>(envelope), static_cast<float>(2 * M_PI * 5000.0 * t));
            }
            return true;
        }
        double sample_rate() const override { return kBandRate; }
        double center_hz() const override { return kBandCenter; }
        fernsdr::SourceStats stats() const override { return {}; }
        const char* kind_name() const override { return "test-am"; }

    private:
        uint64_t n_ = 0;
    };
    const auto levels = [](float highpass) {
        fernsdr::ConfigSection section("band:test");
        Band band("test", "test", std::make_unique<HummingCarrier>(), section);
        auto listener = std::make_shared<Listener>(1, band);
        fernsdr::ChannelSettings channel;
        channel.frequency_hz = kBandCenter + 5000.0;
        channel.mode = fernsdr::Mode::Am;
        channel.bandwidth_low = -3000.0;
        channel.bandwidth_high = 3000.0;
        channel.agc = fernsdr::AgcProfile::Off;
        channel.audio_bitrate = 64000;
        channel.highpass_hz = highpass;
        listener->set_channel(channel);
        fernsdr::ViewportSettings viewport;
        viewport.enabled = false;
        listener->set_viewport(viewport);
        band.add_listener(listener);
        std::vector<std::vector<uint8_t>> messages;
        for (int i = 0; i < 480; i++) {
            CHECK(band.process_one_block());
            listener->drain(messages);
        }
        const auto decoded = decode_stream(messages, listener->actual_audio_rate());
        const double rate = listener->actual_audio_rate();
        const size_t start = static_cast<size_t>(rate);
        const size_t count = static_cast<size_t>(rate);  // one second: whole cycles of both
        CHECK(decoded.audio.size() >= start + count);
        if (decoded.audio.size() < start + count) return std::pair<double, double>(0, 0);
        return std::make_pair(amplitude_at(decoded.audio, 100.0, rate, start, count),
                              amplitude_at(decoded.audio, 1000.0, rate, start, count));
    };
    const auto open = levels(0.0f);
    const auto filtered = levels(300.0f);
    // A second-order filter at 300 Hz: about 19 dB down at 100 Hz.
    CHECK(20.0 * std::log10(filtered.first / open.first) < -15.0);
    CHECK_NEAR(20.0 * std::log10(filtered.second / open.second), 0.0, 0.5);
}

TEST_CASE(band_finds_a_carrier_between_bins) {
    fernsdr::ConfigSection section("band:test");
    section.set("spectrum_bins", "4096");
    section.set("ppm", "-20");
    const double bin = kBandRate / 4096.0;
    for (double fraction : {0.0, 0.25, 0.5, -0.3}) {
        const double offset = (900.0 + fraction) * bin;
        Band band("test", "test", std::make_unique<SteadyTone>(offset), section);
        for (int i = 0; i < 60; i++) CHECK(band.process_one_block());
        const double truth = (kBandCenter + offset) * (1.0 - 20e-6);
        double found = 0.0;
        float level = 0.0f;
        CHECK(band.find_carrier(truth + 300.0, 2000.0, found, level));
        CHECK_NEAR(found, truth, 0.5);
        CHECK_NEAR(level, 20.0 * std::log10(0.25), 1.0);
    }
    Band band("test", "test", std::make_unique<SteadyTone>(0.0), section);
    double found = 0.0;
    float level = 0.0f;
    CHECK(!band.find_carrier(kBandCenter, 2000.0, found, level));  // no spectrum yet
    CHECK(band.process_one_block());
    for (int i = 0; i < 20; i++) CHECK(band.process_one_block());
    CHECK(!band.find_carrier(kBandCenter + 1e6, 2000.0, found, level));  // not on the band
    CHECK(!band.find_carrier(std::nan(""), 2000.0, found, level));
}

TEST_CASE(audio_rate_follows_the_passband) {
    using fernsdr::passband_audio_rate;
    // 64 Msps through a million-point channelizer, 61 Hz bins. SSB runs at
    // half the rate it had, exactly: a rounded 7813 would drift.
    CHECK_EQ(passband_audio_rate(64e6, 12000, 1u << 20, 300, 2700), 7812.5);
    // Narrow CW could take 3906.25 Hz, below the lowest the page plays.
    CHECK_EQ(passband_audio_rate(64e6, 12000, 1u << 20, 650, 750), 7812.5);
    CHECK_EQ(passband_audio_rate(64e6, 12000, 1u << 20, -4500, 4500), 15625.0);
    // 2.048 Msps, as an RTL-SDR delivers it: 62.5 Hz bins.
    CHECK_EQ(passband_audio_rate(2.048e6, 12000, 32768, 300, 2700), 8000.0);
    CHECK_EQ(passband_audio_rate(2.048e6, 12000, 32768, 650, 750), 4000.0);
    CHECK_EQ(passband_audio_rate(2.048e6, 12000, 32768, -4500, 4500), 16000.0);
    // 192 kHz: the next rate down, 6 kHz, cannot hold SSB and its skirt.
    CHECK_EQ(passband_audio_rate(192000, 12000, 2048, 300, 2700), 12000.0);
    CHECK_EQ(passband_audio_rate(192000, 12000, 2048, 650, 750), 6000.0);
    // The rate asked for stays the ceiling, wide passband or not.
    CHECK_EQ(passband_audio_rate(2.048e6, 8000, 32768, -4500, 4500), 8000.0);
}

TEST_CASE(the_lowest_rate_a_page_asks_for_is_one_it_plays) {
    using fernsdr::channel_audio_rate;
    using fernsdr::passband_audio_rate;
    // 4 kHz rounds to the nearest power of two of the band rate, and that
    // may lie below 4 kHz, which the page refuses: it stays silent.
    CHECK_EQ(channel_audio_rate(192000, 4000, 4096), 6000.0);
    CHECK_EQ(passband_audio_rate(192000, 4000, 4096, 650, 750), 6000.0);
    CHECK_EQ(channel_audio_rate(64e6, 4000, 1u << 20), 7812.5);
    CHECK_EQ(passband_audio_rate(64e6, 4000, 1u << 20, 650, 750), 7812.5);
    CHECK_EQ(passband_audio_rate(2.048e6, 4000, 32768, 650, 750), 4000.0);

    fernsdr::Config config;
    std::string error;
    CHECK(config.parse("[band:test]\nsource = test\nsample_rate = 192000\ncenter = 7.1M\nfft_size = 4096\n"
                       "noise = 0.001\nrealtime = false\n", error));
    const auto& section = config.section("band:test");
    Band band("test", "Rates", fernsdr::make_source(section, error), section);
    auto listener = std::make_shared<Listener>(1, band);
    auto channel = listener->channel();
    channel.frequency_hz = 7100000.0;
    channel.mode = fernsdr::Mode::Cw;
    channel.bandwidth_low = 650;
    channel.bandwidth_high = 750;
    channel.requested_audio_rate = 4000;
    listener->set_channel(channel);
    auto view = listener->viewport();
    view.enabled = false;
    listener->set_viewport(view);
    band.add_listener(listener);
    CHECK(band.process_one_block());
    CHECK(listener->actual_audio_rate() >= 4000.0);
}

TEST_CASE(listener_rate_moves_up_at_once_and_down_only_with_the_mode) {
    fernsdr::Config config;
    std::string error;
    CHECK(config.parse("[band:test]\nsource = test\nsample_rate = 2048000\ncenter = 7.1M\nnoise = 0.001\n"
                       "realtime = false\n", error));
    const auto& section = config.section("band:test");
    Band band("test", "Rates", fernsdr::make_source(section, error), section);
    auto listener = std::make_shared<Listener>(1, band);
    auto channel = listener->channel();
    channel.frequency_hz = 7100000.0;
    channel.mode = fernsdr::Mode::Usb;
    channel.bandwidth_low = 300;
    channel.bandwidth_high = 2700;
    listener->set_channel(channel);
    auto view = listener->viewport();
    view.enabled = false;
    listener->set_viewport(view);
    band.add_listener(listener);
    const auto rate_after = [&](const fernsdr::ChannelSettings& settings) {
        listener->set_channel(settings);
        CHECK(band.process_one_block());
        return listener->actual_audio_rate();
    };
    CHECK_EQ(rate_after(channel), 8000.0);

    // Wider than 8 kHz holds: up at once.
    channel.bandwidth_high = 3800;
    CHECK_EQ(rate_after(channel), 16000.0);
    // Back again: no second rebuild, and so no second gap, while an edge is dragged.
    channel.bandwidth_high = 2700;
    CHECK_EQ(rate_after(channel), 16000.0);
    // A new mode settles on what it needs.
    channel.mode = fernsdr::Mode::Lsb;
    channel.bandwidth_low = -2700;
    channel.bandwidth_high = -300;
    CHECK_EQ(rate_after(channel), 8000.0);
    channel.mode = fernsdr::Mode::Am;
    channel.bandwidth_low = -4500;
    channel.bandwidth_high = 4500;
    CHECK_EQ(rate_after(channel), 16000.0);
    channel.mode = fernsdr::Mode::Cw;
    channel.bandwidth_low = 450;
    channel.bandwidth_high = 950;
    CHECK_EQ(rate_after(channel), 4000.0);
}

namespace {

// A carrier 12 kHz over the band's centre, modulated 80% at 100 Hz, fading
// 12 dB over two seconds from `fade_at` and staying down, with a little noise
// for the gain control to measure.
class FadingAmSource final : public fernsdr::Source {
public:
    explicit FadingAmSource(double fade_at) : fade_at_(fade_at) {}
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++, n_++) {
            const double t = static_cast<double>(n_) / kBandRate;
            const double faded = std::pow(10.0, -12.0 * std::clamp((t - fade_at_) / 2.0, 0.0, 1.0) / 20.0);
            const double envelope = 0.05 * faded * (1.0 + 0.8 * std::cos(2 * M_PI * 100.0 * t));
            out[i] = std::polar(static_cast<float>(envelope), static_cast<float>(2 * M_PI * 12000.0 * t)) +
                     fernsdr::cfloat(noise_(rng_), noise_(rng_));
        }
        return true;
    }
    double sample_rate() const override { return kBandRate; }
    double center_hz() const override { return kBandCenter; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-am"; }
    size_t produced() const { return n_; }

private:
    double fade_at_;
    size_t n_ = 0;
    std::mt19937 rng_{43};
    std::normal_distribution<float> noise_{0.0f, 0.0002f};
};

// Runs a listener on `source` for `seconds` with `mode` and `agc`, and
// returns its decoded audio and, at the end, the gain its telemetry reports.
struct Listened {
    std::vector<float> audio;
    double rate = 0;
    float gain_db = 0;
};

Listened listen(std::unique_ptr<FadingAmSource> source, fernsdr::Mode mode, fernsdr::AgcProfile agc, double seconds,
                float manual_gain_db = 0.0f) {
    fernsdr::ConfigSection section("band:test");
    const FadingAmSource* produced = source.get();
    Band band("test", "test", std::move(source), section);
    auto listener = std::make_shared<Listener>(1, band);
    auto channel = listener->channel();
    channel.frequency_hz = kBandCenter + 12000.0;
    channel.mode = mode;
    const fernsdr::Passband passband = fernsdr::default_passband(mode, 700.0);
    channel.bandwidth_low = passband.low;
    channel.bandwidth_high = passband.high;
    channel.agc = agc;
    channel.manual_gain_db = manual_gain_db;
    listener->set_channel(channel);
    auto view = listener->viewport();
    view.enabled = false;
    listener->set_viewport(view);
    band.add_listener(listener);
    std::vector<std::vector<uint8_t>> messages;
    while (static_cast<double>(produced->produced()) < seconds * kBandRate) {
        CHECK(band.process_one_block());
        listener->drain(messages);
    }
    Listened out;
    out.rate = listener->actual_audio_rate();
    out.audio = decode_stream(messages, static_cast<int>(out.rate)).audio;
    out.gain_db = listener->telemetry().agc_gain_db;
    return out;
}

double rms_between(const Listened& heard, double from, double to) {
    const size_t begin = static_cast<size_t>(from * heard.rate), end = static_cast<size_t>(to * heard.rate);
    double power = 0;
    for (size_t i = begin; i < end && i < heard.audio.size(); i++) power += static_cast<double>(heard.audio[i]) * heard.audio[i];
    return std::sqrt(power / static_cast<double>(end - begin));
}

}  // namespace

TEST_CASE(integration_nfm_runs_without_gain_control_whatever_is_asked) {
    // The discriminator hears only the phase: a gain ahead of it would change
    // nothing but the S-meter, so a listener gives NFM none, even when the
    // page asks for a speed.
    const Listened heard = listen(std::make_unique<FadingAmSource>(100.0), fernsdr::Mode::Nfm, fernsdr::AgcProfile::Fast, 1.0,
                                  10.0f);
    CHECK_EQ(heard.gain_db, 0.0f);
}

TEST_CASE(integration_am_follows_its_carrier_through_a_fade) {
    // Levelled on carrier and sidebands together, the 100 Hz tone comes out
    // at its share of the target. The rule speech gets, 2 ms peaks no more
    // than 3 dB over it, fires on these crests every cycle and left the tone
    // 3 dB lower; its hold then kept it 3 dB low through the fade as well.
    const Listened heard = listen(std::make_unique<FadingAmSource>(2.0), fernsdr::Mode::Am, fernsdr::AgcProfile::Slow, 5.0);
    const double before = 20.0 * std::log10(rms_between(heard, 1.0, 2.0));
    const double share = 0.8 * fernsdr::Agc::kTarget / std::sqrt(1.0 + 0.8 * 0.8 / 2.0) / std::sqrt(2.0);
    CHECK_NEAR(before, 20.0 * std::log10(share), 1.0);
    CHECK_NEAR(20.0 * std::log10(rms_between(heard, 4.0, 4.5)), before, 1.5);
}

namespace {

// A broadcast FM station 300 kHz above the centre of a 2.048 Msps band: two
// tones of 15 kHz deviation each at 1 and 5 kHz, the 19 kHz pilot, a stereo
// difference signal on 38 kHz and a stand-in for RDS at 57 kHz, 75 kHz of
// deviation in all, with a little noise.
class FmStation final : public fernsdr::Source {
public:
    static constexpr double kRate = 2048000.0;
    static constexpr double kCentre = 100000000.0;
    static constexpr double kOffset = 300000.0;
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++, n_++) {
            const double t = static_cast<double>(n_) / kRate;
            const double mpx = 15000.0 * std::sin(2 * M_PI * 1000.0 * t) + 15000.0 * std::sin(2 * M_PI * 5000.0 * t) +
                               7000.0 * std::sin(2 * M_PI * 19000.0 * t) +
                               20000.0 * std::sin(2 * M_PI * 3000.0 * t) * std::sin(2 * M_PI * 38000.0 * t) +
                               3000.0 * std::sin(2 * M_PI * 57000.0 * t);
            phase_ += 2 * M_PI * (kOffset + mpx) / kRate;
            phase_ = std::remainder(phase_, 2 * M_PI);
            out[i] = std::polar(0.3f, static_cast<float>(phase_)) + fernsdr::cfloat(noise_(rng_), noise_(rng_));
        }
        return true;
    }
    double sample_rate() const override { return kRate; }
    double center_hz() const override { return kCentre; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-fm"; }
    size_t produced() const { return n_; }

private:
    size_t n_ = 0;
    double phase_ = 0.0;
    std::mt19937 rng_{47};
    std::normal_distribution<float> noise_{0.0f, 0.001f};
};

struct HeardFm {
    std::vector<float> audio;
    double rate = 0;
};

HeardFm listen_to_fm(float deemphasis_us) {
    fernsdr::ConfigSection section("band:fm");
    auto source = std::make_unique<FmStation>();
    const FmStation* produced = source.get();
    Band band("fm", "fm", std::move(source), section);
    CHECK(band.wfm());
    auto listener = std::make_shared<Listener>(1, band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = FmStation::kCentre + FmStation::kOffset;
    channel.mode = fernsdr::Mode::Wfm;
    const fernsdr::Passband passband = fernsdr::default_passband(fernsdr::Mode::Wfm, 700.0);
    channel.bandwidth_low = passband.low;
    channel.bandwidth_high = passband.high;
    channel.wfm_deemphasis_us = deemphasis_us;
    channel.audio_bitrate = 128000;
    listener->set_channel(channel);
    listener->set_bitrate_budget(400000);
    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band.add_listener(listener);
    std::vector<std::vector<uint8_t>> messages;
    while (static_cast<double>(produced->produced()) < 1.5 * FmStation::kRate) {
        CHECK(band.process_one_block());
        listener->drain(messages);
    }
    HeardFm out;
    out.rate = listener->actual_audio_rate();
    out.audio = decode_stream(messages, static_cast<int>(out.rate)).audio;
    return out;
}

}  // namespace

TEST_CASE(integration_broadcast_fm_is_heard_with_its_de_emphasis_and_without_its_subcarriers) {
    const HeardFm fifty = listen_to_fm(50.0f);
    // Audio well above what broadcast FM carries, below twice that.
    CHECK(fifty.rate >= 34000.0);
    CHECK(fifty.rate < 70000.0);
    const size_t start = static_cast<size_t>(0.5 * fifty.rate);
    const size_t count = static_cast<size_t>(0.5 * fifty.rate);
    CHECK(fifty.audio.size() >= start + count);
    if (fifty.audio.size() < start + count) return;
    const double low = amplitude_at(fifty.audio, 1000.0, fifty.rate, start, count);
    const double high = amplitude_at(fifty.audio, 5000.0, fifty.rate, start, count);
    CHECK(low > 0.05);
    // 50 microseconds: a pole at 3.18 kHz, 5.0 dB more down at 5 kHz than at 1.
    CHECK_NEAR(20.0 * std::log10(high / low), -5.0, 0.6);
    // Everything that is not the two tones, the pilot and the subcarriers
    // among it, well below them: each tone fitted and taken away.
    std::vector<double> rest(fifty.audio.begin() + static_cast<long>(start),
                             fifty.audio.begin() + static_cast<long>(start + count));
    double tones = 0.0;
    for (const double hz : {1000.0, 5000.0}) {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < count; i++) {
            const double a = 2.0 * M_PI * hz * static_cast<double>(i + start) / fifty.rate;
            re += rest[i] * std::cos(a);
            im += rest[i] * std::sin(a);
        }
        re *= 2.0 / static_cast<double>(count);
        im *= 2.0 / static_cast<double>(count);
        for (size_t i = 0; i < count; i++) {
            const double a = 2.0 * M_PI * hz * static_cast<double>(i + start) / fifty.rate;
            rest[i] -= re * std::cos(a) + im * std::sin(a);
        }
        tones += (re * re + im * im) / 2.0;
    }
    double residual = 0.0;
    for (const double x : rest) residual += x * x;
    residual /= static_cast<double>(count);
    CHECK(10.0 * std::log10(residual / tones) < -50.0);

    const HeardFm seventy_five = listen_to_fm(75.0f);
    const double low75 = amplitude_at(seventy_five.audio, 1000.0, seventy_five.rate, start, count);
    const double high75 = amplitude_at(seventy_five.audio, 5000.0, seventy_five.rate, start, count);
    // 75 microseconds: 7.3 dB between them.
    CHECK_NEAR(20.0 * std::log10(high75 / low75), -7.3, 0.6);
}

TEST_CASE(listeners_on_one_fm_station_share_its_demodulation) {
    fernsdr::ConfigSection section("band:fm");
    Band band("fm", "fm", std::make_unique<FmStation>(), section);
    const auto tuned = [&](uint64_t id, double offset_hz) {
        auto listener = std::make_shared<Listener>(id, band);
        fernsdr::ChannelSettings channel;
        channel.frequency_hz = FmStation::kCentre + offset_hz;
        channel.mode = fernsdr::Mode::Wfm;
        channel.bandwidth_low = -100000;
        channel.bandwidth_high = 100000;
        listener->set_channel(channel);
        fernsdr::ViewportSettings viewport;
        viewport.enabled = false;
        listener->set_viewport(viewport);
        band.add_listener(listener);
        return listener;
    };
    auto a = tuned(1, FmStation::kOffset);
    // 300 Hz off the same station: the same demodulation.
    auto b = tuned(2, FmStation::kOffset + 300.0);
    auto c = tuned(3, -400000.0);
    std::vector<std::vector<uint8_t>> heard_a, heard_b, ignored;
    for (int i = 0; i < 200; i++) {
        CHECK(band.process_one_block());
        a->drain(heard_a);
        b->drain(heard_b);
        c->drain(ignored);
    }
    CHECK_EQ(band.shared_fm_count(), 2u);
    // Sharing a station is sharing its audio.
    const auto one = decode_stream(heard_a, static_cast<int>(a->actual_audio_rate())).audio;
    const auto two = decode_stream(heard_b, static_cast<int>(b->actual_audio_rate())).audio;
    const double rate = a->actual_audio_rate();
    CHECK(one.size() > rate / 2);
    CHECK_NEAR(amplitude_at(one, 1000.0, rate, one.size() / 2, one.size() / 4),
               amplitude_at(two, 1000.0, rate, two.size() / 2, two.size() / 4), 0.01);
    // Moved to the same station, the third's own goes at the next block.
    auto channel = c->channel();
    channel.frequency_hz = FmStation::kCentre + FmStation::kOffset;
    c->set_channel(channel);
    for (int i = 0; i < 3; i++) {
        CHECK(band.process_one_block());
        c->drain(ignored);
    }
    CHECK_EQ(band.shared_fm_count(), 1u);
}

namespace {

// A station sending RDS: a 1 kHz tone, the pilot and, on 57 kHz, the groups
// for its name and text over and over, 2 kHz of deviation as stations use.
class RdsFmStation final : public fernsdr::Source {
public:
    static constexpr double kRate = 2048000.0;
    static constexpr double kCentre = 100000000.0;
    static constexpr double kOffset = -250000.0;
    RdsFmStation() : chips_(rds_signal::rds_chips(rds_signal::groups(0xD3C3, 10, "FERN FM ", "Heard through the band"))) {}
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++, n_++) {
            const double t = static_cast<double>(n_) / kRate;
            const int chip = chips_[static_cast<size_t>(t * 2375.0) % chips_.size()];
            const double mpx = 40000.0 * std::sin(2 * M_PI * 1000.0 * t) + 7000.0 * std::sin(2 * M_PI * 19000.0 * t) +
                               2000.0 * chip * std::cos(2 * M_PI * 57000.0 * t);
            phase_ += 2 * M_PI * (kOffset + mpx) / kRate;
            phase_ = std::remainder(phase_, 2 * M_PI);
            out[i] = std::polar(0.3f, static_cast<float>(phase_)) + fernsdr::cfloat(noise_(rng_), noise_(rng_));
        }
        return true;
    }
    double sample_rate() const override { return kRate; }
    double center_hz() const override { return kCentre; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-rds"; }

private:
    std::vector<int> chips_;
    size_t n_ = 0;
    double phase_ = 0.0;
    std::mt19937 rng_{53};
    std::normal_distribution<float> noise_{0.0f, 0.002f};
};

}  // namespace

TEST_CASE(integration_broadcast_fm_brings_the_stations_rds) {
    fernsdr::ConfigSection section("band:fm");
    Band band("fm", "fm", std::make_unique<RdsFmStation>(), section);
    auto listener = std::make_shared<Listener>(1, band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = RdsFmStation::kCentre + RdsFmStation::kOffset;
    channel.mode = fernsdr::Mode::Wfm;
    channel.bandwidth_low = -100000;
    channel.bandwidth_high = 100000;
    listener->set_channel(channel);
    fernsdr::ViewportSettings viewport;
    viewport.enabled = false;
    listener->set_viewport(viewport);
    band.add_listener(listener);
    std::vector<std::vector<uint8_t>> ignored;
    const auto run = [&](double seconds) {
        const int blocks = static_cast<int>(seconds / band.channelizer().block_seconds());
        for (int i = 0; i < blocks; i++) {
            CHECK(band.process_one_block());
            listener->drain(ignored);
        }
    };
    // Two rounds of the station's groups: name and text are in.
    run(2.0);
    const fernsdr::RdsState rds = listener->rds();
    CHECK_EQ(rds.pi, 0xD3C3);
    CHECK_EQ(rds.pty, 10);
    CHECK(rds.ps == "FERN FM ");
    CHECK(rds.rt == "Heard through the band");
    const uint64_t sequence = listener->telemetry().rds_sequence;
    CHECK(sequence > 0);
    // Tuned away to a mode without it, the RDS is gone and says so.
    channel.mode = fernsdr::Mode::Nfm;
    channel.bandwidth_low = -6000;
    channel.bandwidth_high = 6000;
    listener->set_channel(channel);
    run(0.2);
    CHECK(listener->telemetry().rds_sequence > sequence);
    CHECK(listener->rds().ps.empty());
    CHECK_EQ(listener->rds().pi, -1);
}
