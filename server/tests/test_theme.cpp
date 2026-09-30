// The theme and the chat both take input from outside and both end up on a
// page, so both are tested for what they REFUSE rather than for what they
// accept. A theme comes from an authenticated operator, which lowers the odds
// but not the consequences: a receiver that lets its own operator paste
// `url(javascript:...)` into a colour is one stolen admin session away from
// serving script to every listener.
#include "../src/core/archive.h"
#include "../src/core/band.h"
#include "../src/core/chat.h"
#include "../src/core/radio.h"
#include "../src/core/theme.h"
#include "../src/util/config.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

using fernsdr::Json;

namespace {

Json theme_with(const std::string& json) {
    Json out;
    Json::parse(json, out);
    return out;
}

bool accepted(const std::string& json) {
    std::string error;
    return fernsdr::validate_theme(theme_with(json), error);
}

}  // namespace

TEST_CASE(the_default_theme_is_valid) {
    std::string error;
    CHECK(fernsdr::validate_theme(fernsdr::default_theme(), error));
}

TEST_CASE(colours_must_be_hex) {
    CHECK(accepted(R"json({"colors":{"primary":"#ff9900"}})json"));
    CHECK(accepted(R"json({"colors":{"primary":"#fff"}})json"));
    CHECK(accepted(R"json({"colors":{"primary":"#ff990080"}})json"));
    CHECK(!accepted(R"json({"colors":{"primary":"red"}})json"));
    CHECK(!accepted(R"json({"colors":{"primary":"url(javascript:alert(1))"}})json"));
    CHECK(!accepted(R"json({"colors":{"primary":"#gggggg"}})json"));
    CHECK(!accepted(R"json({"colors":{"primary":42}})json"));
    // An unknown token would become a CSS property nobody vetted.
    CHECK(!accepted(R"json({"colors":{"content":"#000000"}})json"));
}

TEST_CASE(background_and_logo_urls_are_restricted) {
    CHECK(accepted(R"json({"background":{"image":"https://example.org/a.jpg"}})json"));
    CHECK(accepted(R"json({"background":{"image":"/local/a.png"}})json"));
    CHECK(accepted(R"json({"background":{"image":"data:image/png;base64,AAAA"}})json"));
    CHECK(accepted(R"json({"background":{"image":""}})json"));
    CHECK(!accepted(R"json({"background":{"image":"javascript:alert(1)"}})json"));
    CHECK(!accepted(R"json({"background":{"image":"data:text/html,<script>"}})json"));
    CHECK(!accepted(R"json({"logo":"javascript:alert(1)"}})json"));
}

TEST_CASE(a_url_cannot_escape_the_css_it_is_written_into) {
    // A quote, a bracket or a backslash would close the url() it lands in.
    const char* hostile[] = {
        "https://x/a.jpg\");alert(1);/*",
        "https://x/a.jpg')",
        "https://x/a(1).jpg",
        "https://x/a\\.jpg",
    };
    for (const char* value : hostile) {
        Json theme = Json::make_object();
        Json background = Json::make_object();
        background.set("image", std::string(value));
        theme.set("background", background);
        std::string error;
        CHECK(!fernsdr::validate_theme(theme, error));
    }
}

TEST_CASE(background_numbers_are_bounded) {
    CHECK(accepted(R"json({"background":{"image":"","opacity":0.5,"blur":10}})json"));
    CHECK(!accepted(R"json({"background":{"image":"","opacity":4}})json"));
    CHECK(!accepted(R"json({"background":{"image":"","opacity":-1}})json"));
    CHECK(!accepted(R"json({"background":{"image":"","blur":900}})json"));
}

TEST_CASE(widgets_are_a_closed_set) {
    CHECK(accepted(R"json({"widgets":[{"type":"chat"},{"type":"clock"},{"type":"station"}]})json"));
    CHECK(!accepted(R"json({"widgets":[{"type":"script"}]})json"));
    CHECK(!accepted(R"json({"widgets":[{"type":"embed","url":"javascript:alert(1)"}]})json"));
    CHECK(accepted(R"json({"widgets":[{"type":"embed","url":"https://map.blitzortung.org/"}]})json"));
    CHECK(!accepted(R"json({"widgets":"chat"})json"));
}

// Back to the built-in look resets how the page looks, not what the operator
// put on it: the widgets stay.
TEST_CASE(resetting_the_look_keeps_the_widgets) {
    fernsdr::ThemeStore store;
    store.load("");
    fernsdr::Json theme;
    CHECK(fernsdr::Json::parse(
        R"json({"colors":{},"palette":"viridis","widgets":[{"type":"notice","title":"About","text":"Hello"}]})json", theme));
    std::string error;
    fernsdr::Json merged = store.snapshot();
    merged.set("palette", theme["palette"]);
    merged.set("widgets", theme["widgets"]);
    CHECK(store.replace(merged, error));
    CHECK(store.reset(error));
    const fernsdr::Json after = store.snapshot();
    CHECK_EQ(after["widgets"].size(), 1);
    CHECK_EQ_STR(after["widgets"][0]["text"].string(), "Hello");
    CHECK_EQ_STR(after["palette"].string(), "classic");
}

// A link opens in the listener's page: only web pages and pages on this
// receiver, never a script or data address.
TEST_CASE(a_links_widget_opens_only_web_pages) {
    CHECK(accepted(R"json({"widgets":[{"type":"links","items":[{"label":"Plan","url":"https://example.org/plan"},{"label":"Here","url":"/about.html"}]}]})json"));
    for (const char* url : {"javascript:alert(1)", "data:text/html,x", "", "vbscript:x", "https://x/\\\"onmouseover"}) {
        CHECK(!accepted(std::string(R"json({"widgets":[{"type":"links","items":[{"label":"x","url":")json") + url +
                        R"json("}]}]})json"));
    }
    CHECK(!accepted(R"json({"widgets":[{"type":"links","items":"https://example.org"}]})json"));
}

TEST_CASE(a_widget_title_cannot_carry_control_characters) {
    Json theme = Json::make_object();
    Json widgets = Json::make_array();
    Json widget = Json::make_object();
    widget.set("type", "notice");
    widget.set("title", std::string("two") + '\n' + "lines");
    widgets.push_back(widget);
    theme.set("widgets", widgets);
    std::string error;
    CHECK(!fernsdr::validate_theme(theme, error));
}

// --- chat ------------------------------------------------------------------

TEST_CASE(chat_text_is_flattened_not_interpreted) {
    // Markup survives as text; it is the client's job not to parse it, and it
    // does not. What is removed is anything that would let one message take up
    // more than one message's worth of screen.
    CHECK_EQ_STR(fernsdr::clean_chat_text("<b>hi</b>", 400), "<b>hi</b>");
    CHECK_EQ_STR(fernsdr::clean_chat_text(std::string("one") + '\n' + "two", 400), "one two");
    CHECK_EQ_STR(fernsdr::clean_chat_text("  padded  ", 400), "padded");
    CHECK_EQ_STR(fernsdr::clean_chat_text(std::string(3, '\x01'), 400), "");
    CHECK_EQ_STR(fernsdr::clean_chat_text("", 400), "");
    CHECK_EQ_STR(fernsdr::clean_chat_text("abcdefghij", 4), "abcd");
}

TEST_CASE(chat_is_rate_limited_per_listener) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    int accepted_count = 0;
    for (int i = 0; i < 12; i++) {
        if (room.post(1, "10.0.0.1", "G0ABC", "hello " + std::to_string(i), 1000, message, error)) accepted_count++;
    }
    CHECK_EQ(accepted_count, 6);
    CHECK(!error.empty());

    // A different listener has their own budget: one person flooding must not
    // silence everybody else.
    CHECK(room.post(2, "10.0.0.2", "M0XYZ", "hello", 1000, message, error));

    // And the window moves on.
    CHECK(room.post(1, "10.0.0.1", "G0ABC", "later", 1000 + 11000, message, error));
}

TEST_CASE(chat_history_is_bounded_and_ordered) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    for (int i = 0; i < 200; i++) {
        room.post(static_cast<uint64_t>(i), "10.0.0.3", "n", "message " + std::to_string(i),
                  1000 + static_cast<int64_t>(i) * 20000, message, error);
    }
    const auto history = room.history();
    CHECK_EQ(static_cast<long long>(history.size()),
             static_cast<long long>(fernsdr::ChatRoom::kHistory));
    CHECK_EQ_STR(history.back().text, "message 199");
    CHECK(history.front().id < history.back().id);
}

TEST_CASE(an_empty_name_becomes_anonymous) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    CHECK(room.post(1, "10.0.0.1", "   ", "hello", 1000, message, error));
    CHECK_EQ_STR(message.name, "anonymous");
}

TEST_CASE(a_muted_address_cannot_post_and_nobody_else_is_affected) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    const int64_t now = 1'700'000'000'000;

    room.mute("10.0.0.1", 0, now);
    CHECK(!room.post(1, "10.0.0.1", "G0ABC", "hello", now, message, error));
    // Told why. A silent mute only teaches somebody to shout louder.
    CHECK(error.find("muted") != std::string::npos);

    // The same person on a different address, and everybody else, are fine.
    CHECK(room.post(2, "10.0.0.2", "G0ABC", "hello", now, message, error));

    room.unmute("10.0.0.1");
    CHECK(room.post(3, "10.0.0.1", "G0ABC", "hello again", now, message, error));
}

TEST_CASE(a_mute_covers_the_ipv6_network_not_just_the_address) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    const int64_t now = 1'700'000'000'000;

    // A host is given a /64 and can take a fresh address from it at will.
    room.mute("2001:db8:1:2::abcd", 0, now);
    CHECK(!room.post(1, "2001:db8:1:2:ffff::1", "G0ABC", "hello", now, message, error));
    CHECK(room.muted("2001:db8:1:2::1", now));
    CHECK(room.post(2, "2001:db8:1:3::1", "G0XYZ", "hello", now, message, error));
    // Listed and lifted as the network it applies to.
    CHECK_EQ_STR(room.mutes(now).at(0).address, "2001:db8:1:2::/64");
    room.unmute("2001:db8:1:2::/64");
    CHECK(room.post(3, "2001:db8:1:2::abcd", "G0ABC", "hello again", now + 60'000, message, error));

    // A mute saved by an earlier version, under one address, applies to its
    // network once read back.
    fernsdr::Json saved = fernsdr::Json::make_array();
    fernsdr::Json entry = fernsdr::Json::make_object();
    entry.set("address", "2001:db8:9:9::1");
    entry.set("until", 0);
    saved.push_back(entry);
    room.load_mutes(saved);
    CHECK(room.muted("2001:db8:9:9:1234::5", now));
}

TEST_CASE(a_chat_that_is_off_takes_nothing_and_keeps_nothing) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    const int64_t now = 1'700'000'000'000;
    CHECK(room.post(1, "10.0.0.1", "G0ABC", "before", now, message, error));

    room.set_enabled(false);
    CHECK(!room.enabled());
    CHECK(room.history().empty());
    CHECK(!room.post(2, "10.0.0.2", "G0XYZ", "during", now + 60'000, message, error));
    CHECK(error == "the chat is off on this receiver");

    // Back on, it starts empty: what was said before is not brought back.
    room.set_enabled(true);
    CHECK(room.history().empty());
    CHECK(room.post(3, "10.0.0.3", "G0DEF", "after", now + 120'000, message, error));
    CHECK_EQ(static_cast<long long>(room.history().size()), 1);
}

TEST_CASE(a_mute_expires_on_its_own) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage message;
    std::string error;
    const int64_t now = 1'700'000'000'000;

    room.mute("10.0.0.1", 10, now);
    CHECK(room.muted("10.0.0.1", now + 9 * 60 * 1000));
    CHECK(!room.muted("10.0.0.1", now + 11 * 60 * 1000));
    CHECK(room.post(1, "10.0.0.1", "G0ABC", "hello", now + 11 * 60 * 1000, message, error));

    // And it stops being listed rather than lingering as a spent entry.
    CHECK_EQ(static_cast<long long>(room.mutes(now + 11 * 60 * 1000).size()), 0);
}

TEST_CASE(mutes_survive_being_written_and_read_back) {
    // The reason this is stored at all: a restart is exactly what somebody
    // being muted waits for.
    fernsdr::ChatRoom first;
    const int64_t now = 1'700'000'000'000;
    first.mute("10.0.0.1", 0, now);
    first.mute("2001:db8::5", 30, now);
    const fernsdr::Json stored = first.mutes_json(now);

    fernsdr::ChatRoom second;
    second.load_mutes(stored);
    CHECK(second.muted("10.0.0.1", now));
    CHECK(second.muted("2001:db8::5", now + 29 * 60 * 1000));
    CHECK(!second.muted("2001:db8::5", now + 31 * 60 * 1000));
    CHECK(!second.muted("10.0.0.9", now));

    // Nonsense in the file is ignored rather than crashing the receiver.
    fernsdr::Json rubbish;
    CHECK(fernsdr::Json::parse("[{\"address\":\"\"},{\"nope\":1},7]", rubbish));
    fernsdr::ChatRoom third;
    third.load_mutes(rubbish);
    CHECK_EQ(static_cast<long long>(third.mutes(now).size()), 0);
}

// --- per-band settings -----------------------------------------------------
//
// Against a real Radio with a real band, because the point is what the band
// ends up holding, not what a copy of the bounds says. max_bandwidth clamps
// every listener's filter on that band; accepting zero would leave it tunable
// and silent.

namespace {

bool build_radio(fernsdr::Config& config, fernsdr::Radio& radio) {
    std::string error;
    config.parse(
        "[site]\nname = Bounds\n"
        "[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n",
        error);
    return radio.configure(config, error);
}

}  // namespace

TEST_CASE(saving_the_station_keeps_the_per_band_settings) {
    // They were being deleted. Both live in one overlay file, and saving the
    // station details wrote only the station's own fields over the top, so
    // renaming a receiver silently reset every band's blanker and limits.
    const std::string directory = "/tmp/fernsdr-overlay-test";
    ::mkdir(directory.c_str(), 0755);
    const std::string config_path = directory + "/config.ini";
    const std::string overlay_path = directory + "/fernsdr-settings.json";
    ::remove(overlay_path.c_str());
    {
        std::FILE* file = std::fopen(config_path.c_str(), "wb");
        CHECK(file != nullptr);
        const std::string text =
            "[site]\nname = Before\n"
            "[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n";
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }

    fernsdr::Config config;
    std::string error;
    CHECK(config.load(config_path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));

    fernsdr::Json band = fernsdr::Json::make_object();
    band.set("noise_blanker", 0.6);
    CHECK(radio.apply_band_json("demo", band, error));

    fernsdr::Json site = fernsdr::Json::make_object();
    site.set("name", "After");
    CHECK(radio.apply_site_json(site, error));

    // Read the file back rather than the object: what is on disk is what a
    // restart will see.
    std::string written;
    {
        std::FILE* file = std::fopen(overlay_path.c_str(), "rb");
        CHECK(file != nullptr);
        char buffer[4096];
        const size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        std::fclose(file);
        written.assign(buffer, got);
    }
    fernsdr::Json stored;
    CHECK(fernsdr::Json::parse(written, stored));
    CHECK_EQ_STR(stored["name"].string(), "After");
    CHECK(stored.has("bands"));
    CHECK_EQ(stored["bands"]["demo"]["noise_blanker"].number(), 0.6);
}

namespace {

// A config file in a fresh directory of its own, with no settings beside it.
std::string fresh_config(const std::string& directory, const std::string& text) {
    ::mkdir(directory.c_str(), 0755);
    ::remove((directory + "/fernsdr-settings.json").c_str());
    const std::string path = directory + "/config.ini";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return path;
}

void rewrite(const std::string& path, const std::string& text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

const char* const kBand = "[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n";

}  // namespace

TEST_CASE(capacity_set_in_the_panel_survives_a_restart) {
    // The file's max_users was read after the settings saved from the panel,
    // so a capacity set there went back to the file's on every restart.
    const std::string path = fresh_config("/tmp/fernsdr-overlay-capacity-test",
                                          std::string("[site]\nname = Capacity\nmax_users = 50\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        CHECK_EQ(radio.site().max_users, 50);
        fernsdr::Json site = fernsdr::Json::make_object();
        site.set("max_users", 120.0);
        CHECK(radio.apply_site_json(site, error));
    }
    fernsdr::Config config;
    CHECK(config.load(path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    CHECK_EQ(radio.site().max_users, 120);
}

TEST_CASE(a_setting_edited_in_the_file_after_the_panel_wins_at_the_next_start) {
    const std::string path = fresh_config("/tmp/fernsdr-overlay-later-file-test",
                                          std::string("[site]\nname = First\nmax_users = 50\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        fernsdr::Json site = fernsdr::Json::make_object();
        site.set("name", "Panel");
        site.set("max_users", 120.0);
        CHECK(radio.apply_site_json(site, error));
    }
    // Edited by hand afterwards: the name, not the limit.
    rewrite(path, std::string("[site]\nname = Second\nmax_users = 50\n") + kBand);
    fernsdr::Config config;
    CHECK(config.load(path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    CHECK_EQ_STR(radio.site().name, "Second");
    CHECK_EQ(radio.site().max_users, 120);
}

TEST_CASE(the_config_editor_changes_only_the_settings_it_edited) {
    const std::string before_text = std::string("[site]\nname = File\nnotice = old\n") + kBand;
    const std::string after_text = std::string("[site]\nname = File\nnotice = new\n") + kBand;
    const std::string path = fresh_config("/tmp/fernsdr-overlay-editor-test", before_text);
    std::string error;
    {
        fernsdr::Config before;
        CHECK(before.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(before, error));
        fernsdr::Json site = fernsdr::Json::make_object();
        site.set("name", "Panel");
        CHECK(radio.apply_site_json(site, error));
        // As the admin panel's editor does it: write the file, then apply.
        rewrite(path, after_text);
        fernsdr::Config after;
        CHECK(after.parse(after_text, error));
        radio.apply_site(after);
        CHECK_EQ_STR(radio.site().name, "Panel");
        CHECK_EQ_STR(radio.site().notice, "new");
    }
    fernsdr::Config config;
    CHECK(config.load(path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    CHECK_EQ_STR(radio.site().name, "Panel");
    CHECK_EQ_STR(radio.site().notice, "new");
}

TEST_CASE(settings_saved_before_the_file_was_recorded_still_win) {
    // Written by a build that kept no record of the file: the panel's values
    // win, as they did then.
    const std::string directory = "/tmp/fernsdr-overlay-old-test";
    const std::string path = fresh_config(directory, std::string("[site]\nname = File\n") + kBand);
    rewrite(directory + "/fernsdr-settings.json", "{\"name\": \"Old panel\"}");
    std::string error;
    fernsdr::Config config;
    CHECK(config.load(path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    CHECK_EQ_STR(radio.site().name, "Old panel");
}

namespace {

// Starts a receiver from `path` and returns what it runs with.
fernsdr::SiteInfo started(const std::string& path) {
    std::string error;
    fernsdr::Config config;
    CHECK(config.load(path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    return radio.site();
}

}  // namespace

TEST_CASE(a_hand_edit_survives_other_saves_before_the_restart) {
    // Edited by hand while the receiver runs; before the restart that picks
    // it up, the settings file is written for something else: a mute, a band
    // setting, a station detail the edit did not touch.
    for (int writer = 0; writer < 3; writer++) {
        const std::string path = fresh_config("/tmp/fernsdr-overlay-pending-test",
                                              std::string("[site]\nname = Old\nnotice = old\n") + kBand);
        std::string error;
        {
            fernsdr::Config config;
            CHECK(config.load(path, error));
            fernsdr::Radio radio;
            CHECK(radio.configure(config, error));
            rewrite(path, std::string("[site]\nname = Old\nnotice = new\n") + kBand);
            if (writer == 0) {
                radio.chat().mute("192.0.2.1", 60, 0);
                CHECK(radio.save_mutes(0, error));
            } else if (writer == 1) {
                fernsdr::Json band = fernsdr::Json::make_object();
                band.set("noise_blanker", 0.6);
                CHECK(radio.apply_band_json("demo", band, error));
            } else {
                fernsdr::Json site = fernsdr::Json::make_object();
                site.set("name", "Panel");
                site.set("notice", "old");
                CHECK(radio.apply_site_json(site, error));
            }
        }
        const fernsdr::SiteInfo site = started(path);
        CHECK_EQ_STR(site.notice, "new");
        if (writer == 2) CHECK_EQ_STR(site.name, "Panel");
    }
}

TEST_CASE(the_config_editor_applies_a_hand_edit_it_finds) {
    // Edited by hand, then the editor saves another change: the edit is in
    // the text the editor wrote, so it applies at once and after a restart.
    const std::string path = fresh_config("/tmp/fernsdr-overlay-editor-pending-test",
                                          std::string("[site]\nnotice = old\nchat = yes\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        const std::string text = std::string("[site]\nnotice = new\nchat = no\n") + kBand;
        rewrite(path, text);
        fernsdr::Config after;
        CHECK(after.parse(text, error));
        radio.apply_site(after);
        CHECK_EQ_STR(radio.site().notice, "new");
    }
    CHECK_EQ_STR(started(path).notice, "new");
}

TEST_CASE(a_panel_save_while_the_file_is_unreadable_keeps_the_record) {
    const std::string path = fresh_config("/tmp/fernsdr-overlay-unreadable-test",
                                          std::string("[site]\nname = File\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        // Half-way through a hand edit, the file does not parse.
        rewrite(path, "[site\nname = File\n");
        fernsdr::Json site = fernsdr::Json::make_object();
        site.set("name", "Panel");
        CHECK(radio.apply_site_json(site, error));
        rewrite(path, std::string("[site]\nname = File\n") + kBand);
    }
    CHECK_EQ_STR(started(path).name, "Panel");
}

TEST_CASE(a_limit_outside_the_panels_range_costs_nothing_else) {
    // A file may close the receiver with max_users = 0, which the panel's own
    // field does not allow. The panel still saves the rest, and the saved
    // settings, bands and mutes survive restarts with the file's limit.
    const std::string path = fresh_config("/tmp/fernsdr-overlay-limit-test",
                                          std::string("[site]\nname = File\nmax_users = 0\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        fernsdr::Json site = radio.site_json();
        site.set("name", "Panel");
        CHECK(radio.apply_site_json(site, error));
        fernsdr::Json band = fernsdr::Json::make_object();
        band.set("noise_blanker", 0.6);
        CHECK(radio.apply_band_json("demo", band, error));
        radio.chat().mute("192.0.2.1", 60, 0);
        CHECK(radio.save_mutes(0, error));
    }
    for (int start = 0; start < 2; start++) {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        CHECK_EQ_STR(radio.site().name, "Panel");
        CHECK_EQ(radio.site().max_users, 0);
        CHECK(radio.overlay_document()["bands"]["demo"].has("noise_blanker"));
        CHECK(radio.overlay_document()["muted"].is_array());
    }
}

TEST_CASE(a_panel_change_after_a_hand_edit_of_the_same_setting_wins) {
    const std::string path = fresh_config("/tmp/fernsdr-overlay-same-key-test",
                                          std::string("[site]\nname = First\n") + kBand);
    std::string error;
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        rewrite(path, std::string("[site]\nname = Hand\n") + kBand);
        fernsdr::Json site = fernsdr::Json::make_object();
        site.set("name", "Panel");
        CHECK(radio.apply_site_json(site, error));
    }
    CHECK_EQ_STR(started(path).name, "Panel");
}

TEST_CASE(band_settings_apply_to_the_running_band) {
    fernsdr::Config config;
    fernsdr::Radio radio;
    CHECK(build_radio(config, radio));

    Json values = Json::make_object();
    values.set("name", "Evening 40");
    values.set("noise_blanker", 0.6);
    values.set("max_bandwidth", 8000.0);
    values.set("max_user_bitrate", 64000.0);

    std::string error;
    CHECK(radio.apply_band_json("demo", values, error));

    const Json bands = radio.bands_json();
    CHECK_EQ(static_cast<long long>(bands.size()), 1);
    const Json& band = bands[0];
    CHECK_EQ_STR(band["name"].string(), "Evening 40");
    CHECK_NEAR(band["noise_blanker"].number(), 0.6, 0.01);
    CHECK_NEAR(band["max_bandwidth"].number(), 8000.0, 0.5);
    CHECK_NEAR(band["max_user_bitrate"].number(), 64000.0, 0.5);
}

TEST_CASE(band_settings_refuse_values_that_would_break_a_band) {
    fernsdr::Config config;
    fernsdr::Radio radio;
    CHECK(build_radio(config, radio));

    struct Case { const char* field; double value; };
    const Case refused[] = {
        {"noise_blanker", -0.1}, {"noise_blanker", 1.5},
        {"max_bandwidth", 0.0},  {"max_bandwidth", 499.0}, {"max_bandwidth", 1e9},
        {"max_user_bitrate", 7999.0}, {"max_user_bitrate", 2000001.0},
        {"default_audio_bitrate", 100.0}, {"default_audio_bitrate", 1000000.0},
    };
    for (const Case& entry : refused) {
        Json values = Json::make_object();
        values.set(entry.field, entry.value);
        std::string error;
        CHECK(!radio.apply_band_json("demo", values, error));
        CHECK(!error.empty());
    }

    // And a rejected field leaves the band exactly as it was.
    const Json after = radio.bands_json();
    CHECK_NEAR(after[0]["max_bandwidth"].number(), 20000.0, 0.5);

    Json empty_name = Json::make_object();
    empty_name.set("name", std::string(""));
    std::string error;
    CHECK(!radio.apply_band_json("demo", empty_name, error));

    Json unknown = Json::make_object();
    unknown.set("name", std::string("x"));
    CHECK(!radio.apply_band_json("no-such-band", unknown, error));
}

namespace {

void write_file(const std::string& path, const std::string& text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    if (!file) return;
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

}  // namespace

TEST_CASE(band_input_corrections_apply_and_are_checked) {
    fernsdr::Config config;
    fernsdr::Radio radio;
    CHECK(build_radio(config, radio));
    Json values = Json::make_object();
    values.set("iq_swap", true);
    values.set("dc_remove", true);
    values.set("iq_balance", true);
    std::string error;
    CHECK(radio.apply_band_json("demo", values, error));
    Json bands = radio.bands_json();
    CHECK(bands[0]["iq_swap"].boolean(false));
    CHECK(bands[0]["dc_remove"].boolean(false));
    CHECK(bands[0]["iq_balance"].boolean(false));

    // Only true or false: a string that looks like one is refused whole.
    Json wrong = Json::make_object();
    wrong.set("dc_remove", std::string("no"));
    wrong.set("iq_swap", false);
    CHECK(!radio.apply_band_json("demo", wrong, error));
    CHECK(!error.empty());
    bands = radio.bands_json();
    CHECK(bands[0]["dc_remove"].boolean(false));
    CHECK(bands[0]["iq_swap"].boolean(false));

    // A real input has no Q: swapping or balancing it is refused, its DC is not.
    fernsdr::Config real_config;
    CHECK(real_config.parse("[band:demo]\nsource = test\nsample_rate = 192k\nsignal = real\n", error));
    fernsdr::Radio real;
    CHECK(real.configure(real_config, error));
    for (const char* key : {"iq_swap", "iq_balance"}) {
        Json refused = Json::make_object();
        refused.set(key, true);
        CHECK(!real.apply_band_json("demo", refused, error));
    }
    Json dc = Json::make_object();
    dc.set("dc_remove", true);
    CHECK(real.apply_band_json("demo", dc, error));
    CHECK(real.bands_json()[0]["dc_remove"].boolean(false));
}

TEST_CASE(saved_band_settings_survive_one_that_no_longer_fits) {
    // Saved for an IQ input that has since become a real one: the swap no
    // longer applies, but nothing else saved for the band may go with it.
    char directory[] = "/tmp/fernsdr-saved-band-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string config_path = std::string(directory) + "/config.ini";
    const std::string overlay_path = std::string(directory) + "/fernsdr-settings.json";
    write_file(config_path, "[band:demo]\nsource = test\nsample_rate = 192k\nsignal = real\n");
    write_file(overlay_path,
               "{\"bands\": {\"demo\": {\"iq_swap\": true, \"name\": \"Kept\", "
               "\"noise_blanker\": 0.4, \"dc_remove\": true}}}");
    fernsdr::Config config;
    std::string error;
    CHECK(config.load(config_path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    const Json bands = radio.bands_json();
    CHECK_EQ_STR(bands[0]["name"].string(), "Kept");
    CHECK_NEAR(bands[0]["noise_blanker"].number(), 0.4, 0.01);
    CHECK(bands[0]["dc_remove"].boolean(false));
    CHECK(!bands[0]["iq_swap"].boolean(true));
    ::unlink(overlay_path.c_str());
    ::unlink(config_path.c_str());
    ::rmdir(directory);
}

TEST_CASE(a_history_shape_saved_from_the_panel_keeps_its_record_across_a_restart) {
    // The band used to open its archive in the configured shape as it was
    // built, and the saved shape reopened it straight after: two reshapes,
    // each starting the record again, on every restart.
    char directory[] = "/tmp/fernsdr-saved-history-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string config_path = std::string(directory) + "/config.ini";
    const std::string overlay_path = std::string(directory) + "/fernsdr-settings.json";
    const std::string archive_path = std::string(directory) + "/demo.wfa";
    write_file(config_path, "[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n"
                            "history = public\nhistory_hours = 1\nhistory_bins = 64\n"
                            "history_path = " + archive_path + "\n");
    write_file(overlay_path, "{\"bands\": {\"demo\": {\"history_hours\": 2, \"history_bins\": 128}}}");

    // What the last run recorded, in the saved shape.
    const int64_t recorded = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count() - 60'000;
    {
        fernsdr::WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(archive_path, 128, 1.0, 2, error));
        std::vector<float> line(1024, -90.0f);
        archive.append(line.data(), line.size(), recorded);
        archive.close();
    }

    fernsdr::Config config;
    std::string error;
    CHECK(config.load(config_path, error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
    CHECK(radio.start(error));
    fernsdr::Band* band = radio.band("demo");
    CHECK(band != nullptr);
    if (band) {
        CHECK_EQ(band->history_bins(), 128);
        CHECK_EQ(band->history_hours(), 2);
        CHECK_EQ(band->info().history_oldest_ms, recorded);
    }
    radio.stop();
    ::unlink(archive_path.c_str());
    ::unlink((archive_path + ".span").c_str());
    ::unlink(overlay_path.c_str());
    ::unlink(config_path.c_str());
    ::rmdir(directory);
}

// Invisible characters that turn a line around or make one name look like
// another are dropped; the text around them, other scripts included, stays.
TEST_CASE(chat_text_loses_invisible_direction_and_zero_width_characters) {
    CHECK_EQ_STR(fernsdr::clean_chat_text("Ann\xE2\x80\xAE" "evil", 24), "Annevil");        // U+202E
    CHECK_EQ_STR(fernsdr::clean_chat_text("A\xE2\x80\x8B" "B\xEF\xBB\xBF" "C", 24), "ABC");  // U+200B, U+FEFF
    CHECK_EQ_STR(fernsdr::clean_chat_text("x\xE2\x81\xA6" "y\xE2\x80\xA8" "z\xC2\x85", 24), "xyz");  // U+2066, U+2028, U+0085
    CHECK_EQ_STR(fernsdr::clean_chat_text("Grüße, Привет, 你好", 400), "Grüße, Привет, 你好");
}

// Moving from /64 to /64 inside one /48, as anyone with a tunnel broker's
// /48 can, gets four times one address's rate and no more; and the whole
// chat has a ceiling however many addresses post.
TEST_CASE(chat_rate_holds_across_a_48_and_across_everyone) {
    fernsdr::ChatRoom room;
    fernsdr::ChatMessage out;
    std::string error;
    int taken = 0;
    for (int i = 0; i < 100; i++) {
        const std::string address = "2001:db8:1:" + std::to_string(i) + "::1";
        taken += room.post(1, address, "x", "hello", 1000, out, error);
    }
    CHECK_EQ(taken, 24);
    taken = 0;
    for (int i = 0; i < 200; i++) {
        const std::string address = "198.51." + std::to_string(i / 250) + "." + std::to_string(i % 250 + 1);
        taken += room.post(1, address, "x", "hello", 1000, out, error);
    }
    CHECK_EQ(taken, 60 - 24);
    // The next window starts afresh.
    CHECK(room.post(1, "203.0.113.5", "x", "hello", 12000, out, error));
}
