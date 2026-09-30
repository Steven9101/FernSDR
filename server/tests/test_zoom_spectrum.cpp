// A view finer than the band's spectrum gets a spectrum of its own: it
// resolves what the band's cannot, reads a carrier at the level the band's
// line does, at the frequency it is on, and is shared by everyone on the view.
#include "../src/core/band.h"
#include "../src/core/listener.h"
#include "../src/core/protocol.h"
#include "../src/codec/waterfall_rc.h"
#include "../src/dsp/zoom_spectrum.h"
#include "../src/util/config.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <random>
#include <vector>

using fernsdr::Channelizer;
using fernsdr::SignalKind;
using fernsdr::ZoomKey;
using fernsdr::ZoomSpectrum;

namespace {

struct Tone {
    double hz;         // from the spectrum origin
    double amplitude;  // complex amplitude for IQ, peak for real
};

// Runs `seconds` of tones in faint noise through a channelizer, a zoom on
// [low, high] and the band's own analyser, as the band does, and returns
// the zoom's last line rendered at `width` and the band's peak level.
struct Result {
    std::vector<float> zoom;
    float band_peak_db = -200.0f;
    bool zoom_ready = false;
};

// `origin` and `scale` place the samples in RF as a band's spectrum origin
// and crystal correction do; tones are given in the samples' own hertz.
Result run(SignalKind kind, double rate, const std::vector<Tone>& tones, double low, double high, int width,
           double seconds, double origin = 0.0, double scale = 1.0) {
    const size_t fft = fernsdr::choose_fft_size(rate);
    Channelizer channelizer(rate, fft, kind);
    ZoomKey key;
    Result result;
    if (!ZoomSpectrum::plan(channelizer, origin, scale, low, high, width, key)) return result;
    ZoomSpectrum zoom(channelizer, origin, scale, key, 25.0, 8, 0.5f);
    fernsdr::SpectrumAnalyzer band(rate, 65536, 25.0, 8, 0.5f, kind);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1e-5f);
    const size_t block = channelizer.block_size();
    std::vector<fernsdr::cfloat> iq(block);
    std::vector<float> real(block);
    std::vector<float> line;
    const size_t blocks = static_cast<size_t>(seconds * rate / static_cast<double>(block));
    for (size_t b = 0, n = 0; b < blocks; b++) {
        for (size_t i = 0; i < block; i++, n++) {
            const double t = static_cast<double>(n) / rate;
            if (kind == SignalKind::Iq) {
                fernsdr::cfloat v(noise(rng), noise(rng));
                for (const Tone& tone : tones)
                    v += std::polar(static_cast<float>(tone.amplitude), static_cast<float>(2 * M_PI * std::fmod(tone.hz * t, 1.0)));
                iq[i] = v;
            } else {
                float v = noise(rng);
                for (const Tone& tone : tones)
                    v += static_cast<float>(tone.amplitude * std::cos(2 * M_PI * std::fmod(tone.hz * t, 1.0)));
                real[i] = v;
            }
        }
        if (kind == SignalKind::Iq) {
            channelizer.process(iq.data());
            band.push(iq.data(), block);
        } else {
            channelizer.process_real(real.data());
            band.push_real(real.data(), block);
        }
        zoom.process(channelizer.current_block());
        while (band.take_line(line)) result.band_peak_db = *std::max_element(line.begin(), line.end());
    }
    result.zoom_ready = zoom.ready();
    result.zoom.assign(static_cast<size_t>(width), -200.0f);
    if (zoom.ready()) zoom.line().render(low, high, result.zoom.data(), result.zoom.size());
    return result;
}

size_t peak_index(const std::vector<float>& line, size_t from, size_t to) {
    return static_cast<size_t>(std::max_element(line.begin() + static_cast<long>(from), line.begin() + static_cast<long>(to)) -
                               line.begin());
}

}  // namespace

TEST_CASE(zoom_is_wanted_only_where_the_band_bins_are_wider_than_the_pixels) {
    // 1024 pixels over 2 MHz are 1953 Hz each: a 989 Hz bin draws them.
    CHECK(!fernsdr::zoom_wanted(6e6, 8e6, 1024, 989.0));
    // Over 300 kHz they are 293 Hz: it does not.
    CHECK(fernsdr::zoom_wanted(7.0e6, 7.3e6, 1024, 989.0));
    // A 31 Hz bin draws a 32 kHz view at 1024 pixels, not a 20 kHz one.
    CHECK(!fernsdr::zoom_wanted(7.1e6, 7.132e6, 1024, 31.25));
    CHECK(fernsdr::zoom_wanted(7.1e6, 7.12e6, 1024, 31.25));
    CHECK(!fernsdr::zoom_wanted(7.1e6, 7.1e6, 1024, 31.25));
    CHECK(!fernsdr::zoom_wanted(7.1e6, 7.12e6, 0, 31.25));
}

TEST_CASE(zoom_resolves_two_carriers_the_band_line_cannot_and_reads_their_level) {
    // Twenty hertz apart on a 2.048 Msps band, whose own bins are 31 Hz: in
    // a 2 kHz view at 1000 pixels the zoom's 2 Hz bins show two peaks with
    // a deep gap between them, each at the level the band's line reads for
    // a lone carrier.
    const double f = 100000.0;
    const Result r = run(SignalKind::Iq, 2.048e6, {{f, 0.01}, {f + 20.0, 0.01}}, f - 1000.0, f + 1000.0, 1000, 2.0);
    CHECK(r.zoom_ready);
    const double pixel = 2.0;
    const size_t first = static_cast<size_t>(1000.0 / pixel);
    const size_t second = static_cast<size_t>(1020.0 / pixel);
    const size_t a = peak_index(r.zoom, first - 3, first + 3);
    const size_t b = peak_index(r.zoom, second - 3, second + 3);
    CHECK_NEAR(r.zoom[a], -40.0, 1.0);
    CHECK_NEAR(r.zoom[b], -40.0, 1.0);
    const float gap = r.zoom[(first + second) / 2];
    CHECK(gap < r.zoom[a] - 20.0f);
    // Well away from both, only the noise: 2 Hz bins of it, far below.
    CHECK(r.zoom[100] < -100.0f);
}

TEST_CASE(zoom_puts_a_carrier_where_it_is_and_at_the_band_line_level) {
    const double f = -250000.0 + 7.3;
    const Result single = run(SignalKind::Iq, 2.048e6, {{f, 0.01}}, f - 1000.0, f + 1000.0, 1000, 1.5);
    CHECK(single.zoom_ready);
    const size_t i = peak_index(single.zoom, 0, single.zoom.size());
    const double found = (f - 1000.0) + (static_cast<double>(i) + 0.5) * 2.0;
    CHECK_NEAR(found, f, 2.0);
    CHECK_NEAR(single.zoom[i], single.band_peak_db, 1.0);
}

TEST_CASE(zoom_on_a_direct_sampling_band_reads_as_its_line_does) {
    // A real 20.48 Msps band, as an RX-888 at a third of its rate: the band
    // line's bins are 156 Hz, and a 40 m SSB-sized view of 3 kHz at 1000
    // pixels gets 3 Hz bins with the carrier at the band line's level.
    const double f = 7.1e6 + 13.0;
    const Result r = run(SignalKind::Real, 20.48e6, {{f, 0.02}}, 7.0985e6, 7.1015e6, 1000, 1.2);
    CHECK(r.zoom_ready);
    const size_t i = peak_index(r.zoom, 0, r.zoom.size());
    const double found = 7.0985e6 + (static_cast<double>(i) + 0.5) * 3.0;
    CHECK_NEAR(found, f, 3.0);
    CHECK_NEAR(r.zoom[i], r.band_peak_db, 1.0);
    CHECK_NEAR(r.zoom[i], -40.0, 1.0);
}

namespace {

class Quiet final : public fernsdr::Source {
public:
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++) out[i] = fernsdr::cfloat(noise_(rng_), noise_(rng_));
        return true;
    }
    double sample_rate() const override { return 192000.0; }
    double center_hz() const override { return 7.1e6; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-quiet"; }

private:
    std::mt19937 rng_{3};
    std::normal_distribution<float> noise_{0.0f, 0.001f};
};

std::shared_ptr<fernsdr::Listener> looking_at(fernsdr::Band& band, uint64_t id, double low, double high, int width) {
    auto listener = std::make_shared<fernsdr::Listener>(id, band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = 7.1e6;
    channel.mode = fernsdr::Mode::Usb;
    channel.audio_enabled = false;
    listener->set_channel(channel);
    fernsdr::ViewportSettings v;
    v.enabled = true;
    v.range_coded = true;
    v.low_hz = low;
    v.high_hz = high;
    v.width = width;
    v.lines_per_second = 25.0;
    listener->set_viewport(v);
    band.add_listener(listener);
    return listener;
}

}  // namespace

TEST_CASE(zoom_spectra_are_shared_per_view_and_released_with_it) {
    fernsdr::ConfigSection section("band:test");
    fernsdr::Band band("test", "test", std::make_unique<Quiet>(), section);
    // The whole band needs none; two on one narrow view share one; another
    // narrow view is another.
    auto whole = looking_at(band, 1, band.low_hz(), band.high_hz(), 1024);
    auto a = looking_at(band, 2, 7.1e6, 7.103e6, 1000);
    auto b = looking_at(band, 3, 7.1e6, 7.103e6, 1000);
    auto c = looking_at(band, 4, 7.11e6, 7.112e6, 800);
    for (int i = 0; i < 40; i++) CHECK(band.process_one_block());
    CHECK_EQ(band.zoom_count(), 2u);
    std::vector<std::vector<uint8_t>> messages;
    a->drain(messages);
    CHECK(!messages.empty());
    band.remove_listener(4);
    c.reset();
    for (int i = 0; i < 2; i++) CHECK(band.process_one_block());
    CHECK_EQ(band.zoom_count(), 1u);
}

namespace {

// Two carriers 20 Hz apart on a 192 kHz band, whose own line has 23 Hz bins.
class Pair final : public fernsdr::Source {
public:
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++, n_++) {
            const double t = static_cast<double>(n_) / 192000.0;
            fernsdr::cfloat v(noise_(rng_), noise_(rng_));
            for (double hz : {10000.0, 10020.0})
                v += std::polar(0.01f, static_cast<float>(2 * M_PI * std::fmod(hz * t, 1.0)));
            out[i] = v;
        }
        return true;
    }
    double sample_rate() const override { return 192000.0; }
    double center_hz() const override { return 7.1e6; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-pair"; }

private:
    size_t n_ = 0;
    std::mt19937 rng_{11};
    std::normal_distribution<float> noise_{0.0f, 1e-5f};
};

}  // namespace

TEST_CASE(zoomed_rows_a_page_decodes_show_two_carriers_closer_than_a_band_bin) {
    fernsdr::ConfigSection section("band:test");
    fernsdr::Band band("test", "test", std::make_unique<Pair>(), section);
    const double low = 7.1e6 + 9000.0, high = 7.1e6 + 11000.0;
    auto listener = looking_at(band, 1, low, high, 1000);
    fernsdr::wfc::RangedLineDecoder decoder;
    std::vector<float> row;
    std::vector<std::vector<uint8_t>> messages;
    for (int i = 0; i < static_cast<int>(2.0 / band.channelizer().block_seconds()); i++) {
        CHECK(band.process_one_block());
        messages.clear();
        listener->drain(messages);
        for (const auto& m : messages) {
            if (m.empty() || m[0] != fernsdr::proto::kStreamWaterfall) continue;
            const uint16_t width = static_cast<uint16_t>(m[20] | m[21] << 8);
            row.assign(width, 0.0f);
            decoder.decode(m.data() + fernsdr::proto::kWaterfallHeaderBytes,
                           m.size() - fernsdr::proto::kWaterfallHeaderBytes, width, row.data(), (m[1] & 8) ? 2 : 1);
        }
    }
    CHECK_EQ(row.size(), 1000u);
    if (row.size() != 1000u) return;
    // Pixels are 2 Hz: the carriers are at 500 and 510, the gap between at 505.
    const float a = *std::max_element(row.begin() + 497, row.begin() + 503);
    const float b = *std::max_element(row.begin() + 507, row.begin() + 513);
    CHECK(a > -45.0f);
    CHECK(b > -45.0f);
    CHECK(row[505] < std::min(a, b) - 15.0f);
}

namespace {

// The last row each listener's page decoded, after `seconds` of the Pair
// band, with or without workers for the band's zooms.
std::vector<std::vector<float>> last_rows(fernsdr::DspWorkers* workers, const std::vector<double>& lows, double seconds) {
    fernsdr::ConfigSection section("band:test");
    fernsdr::Band band("test", "test", std::make_unique<Pair>(), section);
    if (workers) band.set_dsp_workers(workers);
    std::vector<std::shared_ptr<fernsdr::Listener>> listeners;
    for (size_t i = 0; i < lows.size(); i++) listeners.push_back(looking_at(band, 10 + i, lows[i], lows[i] + 2000.0, 1000));
    std::vector<fernsdr::wfc::RangedLineDecoder> decoders(lows.size());
    std::vector<std::vector<float>> rows(lows.size());
    std::vector<std::vector<uint8_t>> messages;
    for (int b = 0; b < static_cast<int>(seconds / band.channelizer().block_seconds()); b++) {
        CHECK(band.process_one_block());
        for (size_t i = 0; i < listeners.size(); i++) {
            messages.clear();
            listeners[i]->drain(messages);
            for (const auto& m : messages) {
                if (m.empty() || m[0] != fernsdr::proto::kStreamWaterfall) continue;
                const uint16_t width = static_cast<uint16_t>(m[20] | m[21] << 8);
                rows[i].assign(width, 0.0f);
                decoders[i].decode(m.data() + fernsdr::proto::kWaterfallHeaderBytes,
                                   m.size() - fernsdr::proto::kWaterfallHeaderBytes, width, rows[i].data(),
                                   (m[1] & 8) ? 2 : 1);
            }
        }
    }
    CHECK_EQ(band.zoom_count(), lows.size());
    return rows;
}

}  // namespace

TEST_CASE(zoom_spectra_spread_over_the_workers_give_the_rows_they_give_alone) {
    // Five views of the two carriers, each placed differently, enough to go
    // to the workers: each resolves the pair, and every page is sent the
    // same row it is sent when the band runs the zooms itself.
    // 400 Hz apart, more than a channel's grid step, so each has its own.
    const std::vector<double> lows = {7.1e6 + 8100.0, 7.1e6 + 8500.0, 7.1e6 + 8900.0, 7.1e6 + 9300.0, 7.1e6 + 9700.0};
    fernsdr::DspWorkers workers(3, 4);
    const auto spread = last_rows(&workers, lows, 2.5);
    const auto alone = last_rows(nullptr, lows, 2.5);
    for (size_t i = 0; i < lows.size(); i++) {
        CHECK_EQ(spread[i].size(), 1000u);
        if (spread[i].size() != 1000u) continue;
        CHECK(spread[i] == alone[i]);
        // Pixels are 2 Hz from the view's low edge.
        const size_t first = static_cast<size_t>((7.1e6 + 10000.0 - lows[i]) / 2.0);
        const float a = *std::max_element(spread[i].begin() + first - 3, spread[i].begin() + first + 3);
        const float b = *std::max_element(spread[i].begin() + first + 7, spread[i].begin() + first + 13);
        CHECK(a > -45.0f && b > -45.0f);
        CHECK(spread[i][first + 5] < std::min(a, b) - 15.0f);
    }
}

TEST_CASE(zoom_leaves_a_view_too_wide_for_any_channel_to_the_band_line) {
    // A 48 kHz sound-card band shown almost whole at 2560 pixels, finer
    // than its own bins: no channel the band can cut holds the view in its
    // flat part, so none is made, and the band's line draws the edges a
    // channel's filter would have taken away.
    Channelizer channelizer(48000.0, fernsdr::choose_fft_size(48000.0), SignalKind::Iq);
    ZoomKey key;
    CHECK(!ZoomSpectrum::plan(channelizer, 0.0, 1.0, -22560.0, 22560.0, 2560, key));
    // A view of a third of it fits.
    CHECK(ZoomSpectrum::plan(channelizer, 0.0, 1.0, -8000.0, 8000.0, 2560, key));
}

TEST_CASE(zoom_draws_nothing_past_the_edge_of_an_iq_band) {
    // The channelizer's spectrum is circular: a view past +rate/2 would read
    // the band's other edge, here a carrier at -1.017 MHz, as if it were at
    // +1.031 MHz. It reads nothing there, as the band's own line does.
    const Result r = run(SignalKind::Iq, 2.048e6, {{-1.017e6, 0.01}}, 1.030e6, 1.032e6, 1000, 1.5);
    CHECK(r.zoom_ready);
    CHECK(*std::max_element(r.zoom.begin(), r.zoom.end()) <= -150.0f);
}

TEST_CASE(zoom_places_a_carrier_by_the_band_origin_and_crystal_correction) {
    // A band centred on 7.1 MHz whose crystal is 20 ppm fast: a carrier
    // 12 kHz up in the samples is at 7.1 MHz plus 12 kHz times 1.00002.
    const double scale = 1.0 + 20e-6;
    const double rf = 7.1e6 + 12000.0 * scale;
    const Result r = run(SignalKind::Iq, 2.048e6, {{12000.0, 0.01}}, rf - 1000.0, rf + 1000.0, 1000, 1.5, 7.1e6, scale);
    CHECK(r.zoom_ready);
    const size_t i = peak_index(r.zoom, 0, r.zoom.size());
    CHECK_NEAR((rf - 1000.0) + (static_cast<double>(i) + 0.5) * 2.0, rf, 2.0);
}

TEST_CASE(zoom_is_kept_while_a_view_pans_within_its_channel) {
    // A 2 kHz view panned by 100 Hz keeps its channel, and the channel still
    // holds it in the flat part; panned by a whole view width it does not.
    Channelizer channelizer(2.048e6, fernsdr::choose_fft_size(2.048e6), SignalKind::Iq);
    ZoomKey here, near, far;
    CHECK(ZoomSpectrum::plan(channelizer, 0.0, 1.0, 100000.0, 102000.0, 1000, here));
    CHECK(ZoomSpectrum::plan(channelizer, 0.0, 1.0, 100100.0, 102100.0, 1000, near));
    CHECK(ZoomSpectrum::plan(channelizer, 0.0, 1.0, 102000.0, 104000.0, 1000, far));
    CHECK(here == near);
    CHECK(here != far);
    ZoomSpectrum zoom(channelizer, 0.0, 1.0, here, 25.0, 8, 0.5f);
    CHECK(zoom.covers(100100.0, 102100.0));
    CHECK(!zoom.covers(102000.0, 104000.0));
    // Every view that plans onto a channel lies in its flat part.
    for (double low = 99000.0; low < 103000.0; low += 37.0) {
        ZoomKey key;
        CHECK(ZoomSpectrum::plan(channelizer, 0.0, 1.0, low, low + 2000.0, 1000, key));
        ZoomSpectrum planned(channelizer, 0.0, 1.0, key, 25.0, 8, 0.5f);
        CHECK(planned.covers(low, low + 2000.0));
    }
}

TEST_CASE(zooming_in_a_step_keeps_drawing_fine_rows_while_the_new_zoom_gathers) {
    // From 4 kHz to 2 kHz around the pair: the new view needs a finer
    // transform, whose first line is half a second away. Meanwhile the old
    // zoom covers the new view and draws it, so the first row after the
    // step already shows the two carriers apart, where the band's own line
    // would show one smear.
    fernsdr::ConfigSection section("band:test");
    fernsdr::Band band("test", "test", std::make_unique<Pair>(), section);
    auto listener = looking_at(band, 1, 7.1e6 + 8000.0, 7.1e6 + 12000.0, 1000);
    std::vector<std::vector<uint8_t>> messages;
    for (int b = 0; b < static_cast<int>(2.0 / band.channelizer().block_seconds()); b++) CHECK(band.process_one_block());
    listener->drain(messages);
    fernsdr::ViewportSettings v = listener->viewport();
    v.low_hz = 7.1e6 + 9000.0;
    v.high_hz = 7.1e6 + 11000.0;
    listener->set_viewport(v);
    std::vector<float> row;
    fernsdr::wfc::RangedLineDecoder decoder;
    for (int b = 0; b < 200 && row.empty(); b++) {
        CHECK(band.process_one_block());
        messages.clear();
        listener->drain(messages);
        for (const auto& m : messages) {
            if (m.empty() || m[0] != fernsdr::proto::kStreamWaterfall || !row.empty()) continue;
            const uint16_t width = static_cast<uint16_t>(m[20] | m[21] << 8);
            row.assign(width, 0.0f);
            CHECK(decoder.decode(m.data() + fernsdr::proto::kWaterfallHeaderBytes,
                                 m.size() - fernsdr::proto::kWaterfallHeaderBytes, width, row.data(), (m[1] & 8) ? 2 : 1));
        }
    }
    CHECK_EQ(row.size(), 1000u);
    if (row.size() != 1000u) return;
    const float a = *std::max_element(row.begin() + 497, row.begin() + 503);
    const float b = *std::max_element(row.begin() + 507, row.begin() + 513);
    CHECK(row[505] < std::min(a, b) - 10.0f);
}
