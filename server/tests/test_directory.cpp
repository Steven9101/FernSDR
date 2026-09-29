#include <string>
#include <vector>

#include "../src/core/directory.h"
#include "../src/util/json.h"
#include "test_util.h"

TEST_CASE(directory_checks_grid_locators) {
    for (const char* good : {"JO62", "jo62", "JO62qm", "JO62QM", "AA00aa", "RR99xx"}) {
        CHECK(fernsdr::valid_grid_locator(good));
    }
    for (const char* bad : {"", "JO6", "JO62q", "JS62", "J062", "JO6A", "JO62qy", "JO62qm1", "JO 62"}) {
        CHECK(!fernsdr::valid_grid_locator(bad));
    }
}

TEST_CASE(directory_checks_public_addresses) {
    for (const char* good : {"sdr.example.org", "37.114.49.156", "my-sdr.example", "localhost"}) {
        CHECK(fernsdr::valid_public_host(good));
    }
    for (const char* bad : {"", "http://sdr.example.org", "sdr.example.org:8073", "sdr.example.org/", "[::1]", "::1",
                            ".example.org", "example.org.", "-sdr.example.org", "sdr example.org"}) {
        CHECK(!fernsdr::valid_public_host(bad));
    }
    CHECK(!fernsdr::valid_public_host(std::string(254, 'a')));
}

TEST_CASE(directory_report_says_what_the_directory_shows) {
    fernsdr::DirectoryReport report;
    report.receiver_id = "0123456789abcdef0123456789abcdef";
    report.instance_id = "cafe";
    report.host = "sdr.example.org";
    report.port = 8073;
    report.grid = "JO62qm";
    report.name = "Test";
    report.antenna = "Loop";
    report.users = 3;
    report.max_users = 100;
    report.bands = {{7.0e6, 7.3e6}, {14.0e6, 14.35e6}};
    fernsdr::Json body;
    CHECK(fernsdr::Json::parse(fernsdr::directory_report_json(report), body));
    CHECK_EQ_STR(body["receiver_id"].string(), report.receiver_id);
    CHECK_EQ_STR(body["hostname"].string(), "sdr.example.org");
    CHECK_EQ(body["port"].number(), 8073);
    CHECK_EQ_STR(body["grid_locator"].string(), "JO62qm");
    CHECK_EQ(body["users"].number(), 3);
    CHECK_EQ(body["max_users"].number(), 100);
    CHECK_EQ(body["receiver_count"].number(), 2);
    // The span the bands cover, and the width they actually take.
    CHECK_EQ(body["range_start_hz"].number(), 7.0e6);
    CHECK_EQ(body["range_end_hz"].number(), 14.35e6);
    CHECK_EQ(body["center_frequency"].number(), 10.675e6);
    CHECK_EQ(body["bandwidth"].number(), 650e3);
    CHECK_EQ_STR(body["software"].string(), "FernSDR");
    // No version: it would tell a stranger which fixes the receiver lacks.
    CHECK(!body.has("version"));
}

TEST_CASE(directory_reports_once_a_minute_and_waits_longer_while_failing) {
    std::vector<std::string> sent;
    bool accept = true;
    fernsdr::DirectoryListing listing([&](const std::string& body, std::string& error) {
        sent.push_back(body);
        if (!accept) error = "sdr-list.xyz answered with HTTP 503";
        return accept;
    });
    const auto report = [] { return std::string("{}"); };
    const auto tick = [&](int64_t now) {
        listing.tick(now, report);
        listing.settle();
    };
    CHECK_EQ_STR(listing.status().state, "off");
    tick(0);
    CHECK(sent.empty());

    listing.set_enabled(true, 0, 10'000);
    CHECK_EQ_STR(listing.status().state, "waiting");
    tick(9'999);
    CHECK(sent.empty());
    tick(10'000);
    CHECK_EQ(sent.size(), 1u);
    CHECK_EQ_STR(listing.status().state, "listed");
    CHECK(listing.status().listed_ms > 0);
    // The next report a minute after the last.
    tick(10'001);
    tick(70'000);
    CHECK_EQ(sent.size(), 1u);
    tick(70'001);
    CHECK_EQ(sent.size(), 2u);

    // While the directory is away: one minute, then two, four, eight, and
    // never more than fifteen.
    accept = false;
    int64_t now = 70'002;
    tick(now);
    now = 130'002;
    for (const int64_t wait : {60'000, 120'000, 240'000, 480'000, 900'000, 900'000}) {
        const size_t before = sent.size();
        tick(now);
        CHECK_EQ(sent.size(), before + 1);
        CHECK_EQ_STR(listing.status().state, "failing");
        CHECK_EQ_STR(listing.status().detail, "sdr-list.xyz answered with HTTP 503");
        tick(now + 1);
        tick(now + wait);
        CHECK_EQ(sent.size(), before + 1);
        now = now + 1 + wait;
    }

    // Back, and a minute apart again.
    accept = true;
    tick(now);
    CHECK_EQ_STR(listing.status().state, "listed");
    const size_t back = sent.size();
    tick(now + 1);
    tick(now + 60'000);
    CHECK_EQ(sent.size(), back);
    tick(now + 60'001);
    CHECK_EQ(sent.size(), back + 1);
    now += 60'002;

    // Switched off, it says nothing more.
    listing.set_enabled(false, now);
    CHECK_EQ_STR(listing.status().state, "off");
    const size_t last = sent.size();
    tick(now + 10'000'000);
    CHECK_EQ(sent.size(), last);
}

TEST_CASE(directory_skips_a_round_it_has_nothing_to_report_for) {
    int sends = 0;
    fernsdr::DirectoryListing listing([&](const std::string&, std::string&) {
        sends++;
        return true;
    });
    listing.set_enabled(true, 0);
    listing.tick(0, [] { return std::string(); });
    listing.settle();
    CHECK_EQ(sends, 0);
    listing.tick(59'999, [] { return std::string("{}"); });
    listing.settle();
    CHECK_EQ(sends, 0);
    listing.tick(60'000, [] { return std::string("{}"); });
    listing.settle();
    CHECK_EQ(sends, 1);
}
