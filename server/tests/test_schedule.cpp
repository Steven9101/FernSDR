#include "../src/core/radio.h"
#include "../src/core/session.h"
#include "../src/util/json.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr int64_t kHour = 3'600'000;
// 2026-09-29 00:00 UTC.
constexpr int64_t kMidnight = 20'725LL * 24 * kHour;

bool configure(fernsdr::Radio& radio, const std::string& text, std::string& error) {
    fernsdr::Config config;
    if (!config.parse(text, error)) return false;
    return radio.configure(config, error);
}

std::string module_band(const std::string& id, const std::string& hours, const std::string& center = "14.1M") {
    return "[band:" + id + "]\nsource = module\nmodule = rtlsdr\nsample_rate = 2048k\ncenter = " + center + "\n" +
           (hours.empty() ? "" : "hours = " + hours + "\n");
}

const std::string kModules = "[modules]\ndirectory = /nonexistent-fernsdr-modules\n";

std::vector<fernsdr::Json> texts_of(fernsdr::Session& session) {
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> binaries;
    session.collect(texts, binaries);
    std::vector<fernsdr::Json> out;
    for (const std::string& text : texts) {
        fernsdr::Json message;
        if (fernsdr::Json::parse(text, message)) out.push_back(message);
    }
    return out;
}

// The last state message sent, or null. States go out at most every 100 ms,
// the latest of them.
fernsdr::Json last_state(fernsdr::Session& session) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    fernsdr::Json state;
    for (const auto& message : texts_of(session)) {
        if (message["type"].string() == "state") state = message;
    }
    return state;
}

}  // namespace

TEST_CASE(schedule_refuses_hours_that_are_not_hours) {
    fernsdr::Radio radio;
    std::string error;
    CHECK(!configure(radio, "[band:a]\nsource = test\nsample_rate = 192k\nhours = at night\n", error));
    CHECK(error.find("[band:a] hours:") == 0);
}

TEST_CASE(schedule_lets_bands_share_a_device_only_at_different_hours) {
    std::string error;
    {
        fernsdr::Radio radio;
        CHECK(configure(radio, kModules + module_band("day", "06:00-18:00") + module_band("night", "18:00-06:00", "7.1M"), error));
    }
    {
        fernsdr::Radio radio;
        CHECK(!configure(radio, kModules + module_band("day", "06:00-18:00") + module_band("evening", "17:00-23:00", "7.1M"), error));
        CHECK(error.find("both take the first rtlsdr device at the same hours") != std::string::npos);
    }
    {
        // Hours that meet only somewhere, by the Sun, are left to the schedule.
        fernsdr::Radio radio;
        CHECK(configure(radio, kModules + module_band("day", "sunrise-sunset") + module_band("night", "19:00-05:00", "7.1M"), error));
    }
    // Always, or the same hours, meet wherever the station is.
    for (const auto& [first, second] : {std::pair<std::string, std::string>{"", "sunset-sunrise"},
                                        std::pair<std::string, std::string>{"sunrise-sunset", "sunrise-sunset"}}) {
        fernsdr::Radio radio;
        CHECK(!configure(radio, kModules + module_band("day", first) + module_band("night", second, "7.1M"), error));
        CHECK(error.find("bands 'day' and 'night'") != std::string::npos);
    }
    {
        // The error names the two that meet, not the first on the device.
        fernsdr::Radio radio;
        CHECK(!configure(radio, kModules + module_band("a", "00:00-06:00") + module_band("b", "06:00-12:00", "7.1M") +
                                    module_band("c", "10:00-14:00", "3.6M"), error));
        CHECK(error.find("bands 'b' and 'c'") != std::string::npos);
    }
}

TEST_CASE(schedule_gives_one_device_to_one_band_even_when_its_first_band_is_off) {
    fernsdr::Radio radio;
    std::string error;
    // b by the Sun and c by the clock meet in the afternoon; a, the first
    // listed, is off then.
    CHECK(configure(radio, kModules + module_band("a", "00:00-06:00") + module_band("b", "sunrise-sunset", "7.1M") +
                               module_band("c", "12:00-18:00", "3.6M"),
                    error));
    fernsdr::Band* a = radio.band("a");
    fernsdr::Band* b = radio.band("b");
    fernsdr::Band* c = radio.band("c");
    if (!a || !b || !c) return;
    radio.apply_schedule(kMidnight + 10 * kHour);
    CHECK(!a->on_air() && b->on_air() && !c->on_air());
    // At 13:00 both b and c have their hours; b, on the air already, keeps
    // the device, and c cannot say when it comes on.
    radio.apply_schedule(kMidnight + 13 * kHour);
    CHECK(!a->on_air());
    CHECK(b->on_air());
    CHECK(!c->on_air());
    CHECK_EQ(c->next_change_ms(), -1);
    radio.stop();
}

TEST_CASE(schedule_stops_and_starts_bands_at_their_hours) {
    fernsdr::Radio radio;
    std::string error;
    CHECK(configure(radio,
                    "[band:morning]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\nhours = 00:00-12:00\n"
                    "[band:always]\nsource = test\nsample_rate = 192k\ncenter = 14.1M\n",
                    error));
    fernsdr::Band* morning = radio.band("morning");
    fernsdr::Band* always = radio.band("always");
    if (!morning || !always) return;
    // The first look only says who is on the air; start() starts those.
    CHECK(radio.apply_schedule(kMidnight + 6 * kHour).empty());
    CHECK(morning->on_air() && always->on_air());
    CHECK_EQ(morning->next_change_ms(), kMidnight + 12 * kHour);
    CHECK_EQ(always->next_change_ms(), -1);
    CHECK(morning->start(error) && always->start(error));

    const auto changed = radio.apply_schedule(kMidnight + 12 * kHour);
    CHECK_EQ(changed.size(), 1u);
    radio.wait_for_schedule();
    CHECK(!morning->on_air());
    CHECK(!morning->running());
    CHECK(always->running());
    // Off the air on purpose: healthy, saying when it comes back, and not
    // brought back by a restart.
    CHECK(morning->healthy());
    CHECK(morning->status() == "off the air until 00:00 UTC");
    CHECK(!morning->restart());
    CHECK(radio.default_band() == always);

    CHECK(radio.apply_schedule(kMidnight + 13 * kHour).empty());
    CHECK(radio.apply_schedule(kMidnight + 24 * kHour).size() == 1u);
    radio.wait_for_schedule();
    CHECK(morning->on_air() && morning->running());
    radio.stop();
}

TEST_CASE(schedule_hours_change_when_the_file_is_saved) {
    fernsdr::Radio radio;
    std::string error;
    CHECK(configure(radio, "[band:a]\nsource = test\nsample_rate = 192k\n", error));
    fernsdr::Band* band = radio.band("a");
    if (!band) return;
    radio.apply_schedule(kMidnight + 20 * kHour);
    CHECK(band->on_air());
    fernsdr::Config saved;
    CHECK(saved.parse("[band:a]\nsource = test\nsample_rate = 192k\nhours = sunrise-sunset\n", error));
    // Not a restart: the hours apply as they are saved.
    CHECK(radio.bands_needing_restart(saved).empty());
    CHECK(radio.band_changes(saved).empty());
    radio.apply_hours(saved);
    // Without a grid square, sunset is taken at 18:00 UTC.
    CHECK_EQ(radio.apply_schedule(kMidnight + 20 * kHour).size(), 1u);
    radio.wait_for_schedule();
    CHECK(!band->on_air());
    CHECK(band->info().hours == "sunrise-sunset");
}

TEST_CASE(schedule_moves_listeners_to_the_band_on_their_input) {
    fernsdr::Radio radio;
    std::string error;
    CHECK(configure(radio, kModules + module_band("day", "06:00-18:00") + module_band("night", "18:00-06:00", "7.1M"), error));
    fernsdr::Band* day = radio.band("day");
    fernsdr::Band* night = radio.band("night");
    if (!day || !night) return;
    radio.apply_schedule(kMidnight + 12 * kHour);
    CHECK(radio.default_band() == day);
    CHECK(radio.successor(night) == day);
    CHECK(radio.successor(day) == nullptr);

    fernsdr::Session session(1, radio);
    session.begin();
    CHECK(session.band_id() == "day");
    // The welcome says which bands take turns on one input, and when.
    fernsdr::Json welcome;
    for (const auto& message : texts_of(session)) {
        if (message["type"].string() == "welcome") welcome = message;
    }
    CHECK_EQ(welcome["bands"].size(), 2u);
    if (welcome["bands"].size() == 2) {
        const fernsdr::Json& second = welcome["bands"][1];
        CHECK(second["shared_input"].string() == "day");
        CHECK(!second["on_air"].boolean(true));
        CHECK(second["hours"].string() == "18:00-06:00");
        CHECK_EQ(second["next_change"].number(), static_cast<double>(kMidnight + 18 * kHour));
    }
    // A page asking for the band off the air lands on the one on the air,
    // and is told so in the state it adopts.
    session.handle_text(R"({"type":"tune","band":"night","freq":7050000,"mode":"cw","request_id":7})");
    fernsdr::Json state = last_state(session);
    CHECK(session.band_id() == "day");
    CHECK(state["band"].string() == "day");
    CHECK(state["note"].string().find("is off the air until 18:00 UTC; now on day") != std::string::npos);
    CHECK_EQ(state["ack"]["tune"].number(), 7);
    CHECK(state["mode"].string() == "cw");
    CHECK(state["freq"].number() >= day->low_hz() && state["freq"].number() <= day->high_hz());

    // At 18:00 the night band has the input, and the listener goes with it.
    CHECK_EQ(radio.apply_schedule(kMidnight + 18 * kHour).size(), 2u);
    CHECK(radio.default_band() == night);
    session.follow_schedule();
    CHECK(session.band_id() == "night");
    state = last_state(session);
    CHECK(state["band"].string() == "night");
    CHECK(state["note"].string().find("is off the air until 06:00 UTC; now on night") != std::string::npos);
    radio.stop();
}

TEST_CASE(schedule_switches_on_its_own_thread_while_others_read_the_bands) {
    fernsdr::Radio radio;
    std::string error;
    CHECK(configure(radio,
                    "[band:morning]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\nhours = 00:00-12:00\n"
                    "[band:always]\nsource = test\nsample_rate = 192k\ncenter = 14.1M\n",
                    error));
    CHECK(radio.start(error));
    fernsdr::Band* morning = radio.band("morning");
    if (!morning) return;
    CHECK_EQ(morning->running(), morning->on_air());
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            for (const auto& band : radio.bands()) {
                const fernsdr::BandInfo info = band->info();
                (void)info;
                (void)band->healthy();
            }
        }
    });
    // Twice across its next change and back: each time on the schedule's
    // thread, the band ends up running exactly when it is on the air.
    int64_t at = morning->next_change_ms();
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(radio.apply_schedule(at).size(), 1u);
        radio.wait_for_schedule();
        CHECK_EQ(morning->running(), morning->on_air());
        at = morning->next_change_ms();
    }
    done.store(true);
    reader.join();
    radio.stop();
}

TEST_CASE(schedule_hours_of_a_band_that_is_rewired_wait_for_the_restart) {
    // The setup flow replaces the test band with a radio's band of the same
    // id and hours: those hours belong to the new band, which runs after the
    // restart, not to the test band still running now.
    fernsdr::Radio radio;
    std::string error;
    CHECK(configure(radio, "[band:40m]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n", error));
    fernsdr::Band* band = radio.band("40m");
    if (!band) return;
    radio.apply_schedule(kMidnight + 12 * kHour);
    fernsdr::Config saved;
    CHECK(saved.parse(kModules + module_band("40m", "sunset-sunrise", "7.1M"), error));
    radio.apply_hours(saved);
    CHECK(radio.apply_schedule(kMidnight + 12 * kHour).empty());
    CHECK(band->on_air());
    CHECK(band->info().hours == "always");
}
