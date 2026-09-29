// Waterfall rows shared between listeners with the same view: every page
// decodes every row it is sent, before, while and after it takes the shared
// stream, and the rows it shares are the same bytes.
#include "../src/codec/waterfall_rc.h"
#include "../src/core/band.h"
#include "../src/core/listener.h"
#include "../src/core/protocol.h"
#include "../src/util/config.h"
#include "test_util.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using fernsdr::Band;
using fernsdr::Listener;

namespace {

// A 192 kHz band with a few carriers that come and go in noise, so rows
// differ from each other and the coder has something to predict.
class Carriers final : public fernsdr::Source {
public:
    bool start(std::string&) override { return true; }
    void stop() override {}
    fernsdr::SignalKind kind() const override { return fernsdr::SignalKind::Iq; }
    bool read(fernsdr::cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++, n_++) {
            const double t = static_cast<double>(n_) / kRate;
            fernsdr::cfloat v(noise_(rng_), noise_(rng_));
            for (int k = 0; k < 5; k++) {
                const double level = 0.02 * (0.5 + 0.5 * std::sin(2 * M_PI * (0.3 + 0.2 * k) * t));
                v += std::polar(static_cast<float>(level), static_cast<float>(2 * M_PI * (-60000.0 + 30000.0 * k) * t));
            }
            out[i] = v;
        }
        return true;
    }
    double sample_rate() const override { return kRate; }
    double center_hz() const override { return 7.1e6; }
    fernsdr::SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "test-carriers"; }
    static constexpr double kRate = 192000.0;

private:
    size_t n_ = 0;
    std::mt19937 rng_{5};
    std::normal_distribution<float> noise_{0.0f, 0.001f};
};

fernsdr::ViewportSettings view(const Band& band, int width) {
    fernsdr::ViewportSettings v;
    v.enabled = true;
    v.range_coded = true;
    v.adaptive_codec = true;
    v.native_grid = true;
    v.zero_runs = true;
    v.step_db = 2;
    v.low_hz = band.low_hz();
    v.high_hz = band.high_hz();
    v.width = width;
    v.lines_per_second = 12.0;
    return v;
}

// What a page does with the rows: decode each in order, as
// web/src/components/SpectrumDisplay.svelte does, resetting on a gap.
struct Page {
    fernsdr::wfc::RangedLineDecoder decoder;
    int expected = -1;
    int rows = 0, failed = 0, keys = 0;
    std::vector<std::vector<uint8_t>> payloads;

    void take(const std::vector<std::vector<uint8_t>>& messages) {
        for (const auto& m : messages) {
            if (m.empty() || m[0] != fernsdr::proto::kStreamWaterfall) continue;
            const int sequence = m[2] | m[3] << 8;
            const uint16_t width = static_cast<uint16_t>(m[20] | m[21] << 8);
            if (expected >= 0 && sequence != expected) decoder.reset();
            expected = (sequence + 1) & 0xffff;
            const uint8_t* payload = m.data() + fernsdr::proto::kWaterfallHeaderBytes;
            const size_t size = m.size() - fernsdr::proto::kWaterfallHeaderBytes;
            std::vector<float> out(width);
            const int step = (m[1] & 8) ? 2 : 1;
            rows++;
            if (size > 0 && (payload[0] & fernsdr::wfc::kRangedKey)) keys++;
            if (!decoder.decode(payload, size, width, out.data(), step)) failed++;
            payloads.emplace_back(payload, payload + size);
        }
    }
};

std::shared_ptr<Listener> watcher(Band& band, uint64_t id, const fernsdr::ViewportSettings& v, int budget = 400000) {
    auto listener = std::make_shared<Listener>(id, band);
    fernsdr::ChannelSettings channel;
    channel.frequency_hz = 7.1e6 + 1000.0 * static_cast<double>(id);  // everyone hears something else
    channel.mode = fernsdr::Mode::Usb;
    channel.bandwidth_low = 300;
    channel.bandwidth_high = 2700;
    channel.audio_enabled = false;
    listener->set_channel(channel);
    listener->set_bitrate_budget(budget);
    listener->set_viewport(v);
    band.add_listener(listener);
    return listener;
}

void run(Band& band, double seconds, const std::vector<std::pair<std::shared_ptr<Listener>, Page*>>& pages) {
    const int blocks = static_cast<int>(seconds / band.channelizer().block_seconds());
    std::vector<std::vector<uint8_t>> messages;
    for (int i = 0; i < blocks; i++) {
        CHECK(band.process_one_block());
        for (const auto& [listener, page] : pages) {
            messages.clear();
            listener->drain(messages);
            page->take(messages);
        }
    }
}

}  // namespace

TEST_CASE(shared_waterfall_rows_are_coded_once_for_one_view_and_decode_everywhere) {
    fernsdr::ConfigSection section("band:test");
    Band band("test", "test", std::make_unique<Carriers>(), section);
    Page first, second, narrow;
    auto a = watcher(band, 1, view(band, 1024));
    auto b = watcher(band, 2, view(band, 1024));
    // Another width is another view.
    auto c = watcher(band, 3, view(band, 700));
    run(band, 3.0, {{a, &first}, {b, &second}, {c, &narrow}});
    CHECK_EQ(band.shared_waterfall_count(), 2u);
    for (const Page* page : {&first, &second, &narrow}) {
        CHECK(page->rows >= 30);
        CHECK_EQ(page->failed, 0);
    }
    // Once both have joined, at the latest the shared stream's second key
    // row, they are sent the same bytes.
    const size_t tail = 12;
    CHECK(first.payloads.size() > tail && second.payloads.size() > tail);
    if (first.payloads.size() > tail && second.payloads.size() > tail) {
        for (size_t k = 1; k <= tail; k++) {
            CHECK(first.payloads[first.payloads.size() - k] == second.payloads[second.payloads.size() - k]);
        }
    }
}

TEST_CASE(shared_waterfall_takes_a_late_page_at_a_key_row_and_lets_it_go_on_a_lost_one) {
    fernsdr::ConfigSection section("band:test");
    Band band("test", "test", std::make_unique<Carriers>(), section);
    Page early, late;
    auto a = watcher(band, 1, view(band, 1024));
    run(band, 1.3, {{a, &early}});
    // Arrives mid-stream: its own rows first, from a key row of its own, and
    // the shared ones from the shared stream's next key row on, all decoded.
    auto b = watcher(band, 2, view(band, 1024));
    run(band, 3.0, {{a, &early}, {b, &late}});
    CHECK_EQ(late.failed, 0);
    CHECK(late.rows >= 30);
    CHECK_EQ(early.failed, 0);
    CHECK(late.payloads.back() == early.payloads.back());
    // A page that lost a row asks for a key row: it gets one at once, of
    // its own, and decodes on.
    const int keys_before = late.keys;
    b->request_waterfall_keyframe();
    run(band, 0.2, {{a, &early}, {b, &late}});
    CHECK(late.keys > keys_before);
    run(band, 2.5, {{a, &early}, {b, &late}});
    CHECK_EQ(late.failed, 0);
    CHECK(late.payloads.back() == early.payloads.back());
}

TEST_CASE(shared_waterfall_leaves_a_thin_link_on_rows_of_its_own) {
    fernsdr::ConfigSection section("band:test");
    Band band("test", "test", std::make_unique<Carriers>(), section);
    Page full, thin;
    auto a = watcher(band, 1, view(band, 1024));
    // A budget too small for twelve rows a second: it gets fewer, of its
    // own, since skipping shared rows would break its decoder.
    auto b = watcher(band, 2, view(band, 1024), 6000);
    run(band, 4.0, {{a, &full}, {b, &thin}});
    CHECK_EQ(full.failed, 0);
    CHECK_EQ(thin.failed, 0);
    CHECK(thin.rows > 0);
    CHECK(thin.rows < full.rows);
}

TEST_CASE(shared_waterfall_hands_a_page_a_key_row_after_a_rebuild_emptied_its_queue) {
    fernsdr::ConfigSection section("band:test");
    Band band("test", "test", std::make_unique<Carriers>(), section);
    Page first, second;
    auto a = watcher(band, 1, view(band, 1024));
    auto b = watcher(band, 2, view(band, 1024));
    run(band, 3.0, {{a, &first}, {b, &second}});
    // A row waits in the second's queue when a new audio rate rebuilds its
    // channel and empties the queue: the page sees the gap, and must get a
    // key row next rather than the shared stream's next row.
    std::vector<std::vector<uint8_t>> messages;
    for (int i = 0; i < 400 && b->queued_bytes() == 0; i++) {
        CHECK(band.process_one_block());
        messages.clear();
        a->drain(messages);
        first.take(messages);
    }
    CHECK(b->queued_bytes() > 0);
    fernsdr::ChannelSettings channel = b->channel();
    channel.requested_audio_rate = 24000;
    b->set_channel(channel);
    const int failed = second.failed, rows = second.rows;
    run(band, 2.5, {{a, &first}, {b, &second}});
    CHECK(second.rows - rows >= 25);
    CHECK_EQ(second.failed, failed);
}

TEST_CASE(shared_waterfall_does_not_swing_a_listener_at_its_budget_between_streams) {
    // Budgets around what twelve rows a second of this view cost: alone or
    // beside a listener with room, a listener there keeps to its rate and
    // to a key row every two seconds or so, rather than joining and leaving
    // the shared stream with a key row each time.
    for (const bool alone : {true, false}) {
        for (const int budget : {24000, 24500, 25000}) {
            fernsdr::ConfigSection section("band:test");
            Band band("test", "test", std::make_unique<Carriers>(), section);
            Page roomy, tight;
            auto b = watcher(band, 2, view(band, 1024), budget);
            std::shared_ptr<Listener> a;
            if (!alone) a = watcher(band, 1, view(band, 1024));
            std::vector<std::pair<std::shared_ptr<Listener>, Page*>> pages{{b, &tight}};
            if (a) pages.push_back({a, &roomy});
            run(band, 3.0, pages);
            const int rows = tight.rows, keys = tight.keys;
            run(band, 20.0, pages);
            CHECK_EQ(tight.failed, 0);
            CHECK(tight.rows - rows <= 241);
            CHECK(tight.keys - keys <= 13);
        }
    }
}
