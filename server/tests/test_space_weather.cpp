// Space weather: the arithmetic the widget rests on, the JSON built from what
// NOAA and KC2G answered on 2026-09-28 (testdata/space_weather), and the
// fetcher's schedule, with no request leaving the machine.
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../src/core/space_weather.h"
#include "../src/util/json.h"
#include "test_util.h"

namespace {

std::string fixture(const std::string& name) {
    const std::string here = __FILE__;
    std::ifstream file(here.substr(0, here.rfind('/')) + "/../testdata/space_weather/" + name, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

fernsdr::SpaceWeatherSources fixtures() {
    fernsdr::SpaceWeatherSources sources;
    sources.scales = fixture("noaa-scales.json");
    sources.kp = fixture("kp.json");
    sources.flux = fixture("f107.json");
    sources.wind_speed = fixture("wind-speed.json");
    sources.wind_field = fixture("wind-mag.json");
    sources.xrays = fixture("xrays.json");
    sources.indices = fixture("daily-solar-indices.txt");
    sources.ionosondes = fixture("stations.json");
    return sources;
}

// 2026-09-28 09:20 UTC, a few minutes after the ionosonde readings.
constexpr int64_t kNow = 1790587200000LL;

}  // namespace

TEST_CASE(space_weather_flare_classes_are_cut_not_rounded) {
    CHECK_EQ_STR(fernsdr::flare_class(1.371e-8), "A1.3");
    CHECK_EQ_STR(fernsdr::flare_class(9.97e-8), "A9.9");
    CHECK_EQ_STR(fernsdr::flare_class(1.2e-6), "C1.2");
    CHECK_EQ_STR(fernsdr::flare_class(5.0e-5), "M5.0");
    CHECK_EQ_STR(fernsdr::flare_class(1.7e-3), "X17.0");
    CHECK_EQ_STR(fernsdr::flare_class(0), "");
}

TEST_CASE(space_weather_sun_and_distance_come_out_where_they_should) {
    // 21 June 2026, 12:00 UTC, Greenwich: 90 - 51.48 + 23.44, about 62 degrees.
    CHECK_NEAR(fernsdr::sun_elevation_deg(1782043200000LL, 51.48, 0.0), 61.9, 0.5);
    // The same moment on the antimeridian: deep night.
    CHECK(fernsdr::sun_elevation_deg(1782043200000LL, 51.48, 180.0) < -10);
    // Equinox noon on the equator: nearly overhead.
    CHECK(fernsdr::sun_elevation_deg(1774094400000LL, 0.0, 0.0) > 85);
    // Dourbes to Juliusruh, about 750 km.
    CHECK_NEAR(fernsdr::distance_km(50.1, 4.6, 54.6, 13.4), 780, 30);
}

TEST_CASE(space_weather_bands_follow_the_muf_the_sun_and_storms) {
    // Dourbes at 09:15 UTC: MUF(3000) 25.7 MHz, foF2 7.4 MHz, by day.
    const auto day = fernsdr::band_outlook(25.7, 7.4, true, 1.7, 51.5);
    std::map<std::string, fernsdr::BandOutlook> by;
    for (const auto& band : day) by.emplace(band.band, band);
    CHECK_EQ_STR(std::string(by.at("20 m").dx), "open");
    CHECK_EQ_STR(std::string(by.at("12 m").dx), "marginal");
    CHECK_EQ_STR(std::string(by.at("10 m").dx), "closed");
    CHECK_EQ_STR(std::string(by.at("80 m").dx), "closed");
    CHECK_EQ_STR(std::string(by.at("80 m").note), "absorbed by day");
    CHECK(by.at("80 m").nearby);            // NVIS under foF2 all the same
    CHECK(!by.at("160 m").nearby);          // but not 160 m by day
    CHECK(by.at("40 m").nearby);
    CHECK(!by.at("20 m").nearby);           // above foF2: it skips over
    CHECK_EQ_STR(std::string(by.at("6 m").dx), "closed");
    // By night the low bands carry.
    for (const auto& band : fernsdr::band_outlook(9.0, 3.0, false, 1.7, 51.5))
        if (std::string(band.band) == "80 m") CHECK_EQ_STR(std::string(band.dx), "open");
    // A storm at 51 degrees takes a level off.
    for (const auto& band : fernsdr::band_outlook(25.7, 7.4, true, 6, 51.5))
        if (std::string(band.band) == "20 m") {
            CHECK_EQ_STR(std::string(band.dx), "marginal");
            CHECK_EQ_STR(std::string(band.note), "geomagnetic storm");
        }
    // No reading, no claims.
    for (const auto& band : fernsdr::band_outlook(0, 0, true, 2, 51.5)) CHECK_EQ_STR(std::string(band.dx), "unknown");
}

TEST_CASE(space_weather_json_from_what_the_sources_said) {
    fernsdr::Json out;
    CHECK(fernsdr::Json::parse(fernsdr::space_weather_json(fixtures(), kNow, 51.5, 7.0), out));
    CHECK_NEAR(out["scales"]["r"].number(-1), 0, 0);
    CHECK_NEAR(out["scales"]["g"].number(-1), 0, 0);
    CHECK(out["kp"].is_number());
    CHECK_EQ(out["kp_history"].size(), 8u);
    CHECK_NEAR(out["sfi"].number(), 97, 0);
    CHECK_NEAR(out["ssn"].number(), 67, 0);
    CHECK_NEAR(out["wind_speed"].number(), 347, 0);
    CHECK_NEAR(out["bz"].number(99), -1, 0);
    CHECK(out["xray"].string().size() >= 4);
    // JO31: Dourbes is the nearest ionosonde with a fresh reading.
    CHECK_EQ_STR(out["ionosonde"]["code"].string(), "DB049");
    CHECK(out["ionosonde"]["km"].number() < 300);
    CHECK_EQ(out["bands"].size(), 11u);
    CHECK_EQ_STR(out["bands"][5]["band"].string(), "20 m");
    CHECK_EQ_STR(out["bands"][5]["dx"].string(), "open");

    // A day later every reading is stale: no ionosonde, and the bands say so.
    CHECK(fernsdr::Json::parse(fernsdr::space_weather_json(fixtures(), kNow + 86'400'000LL, 51.5, 7.0), out));
    CHECK(out["ionosonde"].is_null());
    CHECK_EQ_STR(out["bands"][5]["dx"].string(), "unknown");

    // Without a locator there is nothing to be near.
    CHECK(fernsdr::Json::parse(fernsdr::space_weather_json(fixtures(), kNow, std::nan(""), std::nan("")), out));
    CHECK(out["ionosonde"].is_null());
    CHECK(out["bands"].is_null());
    CHECK_NEAR(out["sfi"].number(), 97, 0);

    // Nothing answered, or answered nonsense: an object all the same.
    CHECK(fernsdr::Json::parse(fernsdr::space_weather_json({}, kNow, 51.5, 7.0), out));
    CHECK(out.is_object());
    fernsdr::SpaceWeatherSources broken;
    broken.kp = "<html>";
    broken.ionosondes = "[1,2,{\"mufd\":\"x\"}]";
    CHECK(fernsdr::Json::parse(fernsdr::space_weather_json(broken, kNow, 51.5, 7.0), out));
    CHECK(out["kp"].is_null());
}

TEST_CASE(space_weather_fetches_only_while_on_and_keeps_what_it_had) {
    std::atomic<int> calls{0};
    std::atomic<bool> failing{false};
    const std::map<std::string, std::string> answers = {
        {"noaa-scales.json", "noaa-scales.json"}, {"noaa-planetary-k-index.json", "kp.json"},
        {"10cm-flux.json", "f107.json"},          {"solar-wind-speed.json", "wind-speed.json"},
        {"solar-wind-mag-field.json", "wind-mag.json"}, {"xrays-6-hour.json", "xrays.json"},
        {"daily-solar-indices.txt", "daily-solar-indices.txt"}, {"stations.json", "stations.json"},
    };
    fernsdr::SpaceWeather weather([&](const std::string& url, size_t, std::string& body, std::string& error) {
        ++calls;
        if (failing) {
            error = "offline";
            return false;
        }
        for (const auto& [ending, file] : answers)
            if (url.size() >= ending.size() && url.compare(url.size() - ending.size(), ending.size(), ending) == 0) {
                body = fixture(file);
                return true;
            }
        error = "unknown";
        return false;
    });
    weather.configure(false, 51.5, 7.0);
    weather.settle();
    CHECK_EQ(calls.load(), 0);
    CHECK(weather.json().empty());

    weather.configure(true, 51.5, 7.0);
    weather.settle();
    CHECK_EQ(calls.load(), 8);
    CHECK(weather.json().find("\"sfi\":97") != std::string::npos);

    // A new locator is worked out from what is there, without fetching.
    weather.configure(true, 41.5, -111.0);
    weather.settle();
    CHECK_EQ(calls.load(), 8);
    CHECK(weather.json().find("DB049") == std::string::npos);
}

TEST_CASE(space_weather_locators_as_positions) {
    double lat = 0, lon = 0;
    CHECK(fernsdr::locator_centre("JO31", lat, lon));
    CHECK_NEAR(lat, 51.5, 1e-9);
    CHECK_NEAR(lon, 7.0, 1e-9);
    CHECK(fernsdr::locator_centre("dn41", lat, lon));
    CHECK_NEAR(lat, 41.5, 1e-9);
    CHECK_NEAR(lon, -111.0, 1e-9);
    CHECK(fernsdr::locator_centre("JO31ne", lat, lon));
    CHECK_NEAR(lon, 6 + 13 * 5.0 / 60 + 2.5 / 60, 1e-9);
    CHECK(!fernsdr::locator_centre("", lat, lon));
    CHECK(!fernsdr::locator_centre("JO3", lat, lon));
    CHECK(!fernsdr::locator_centre("ZZ99", lat, lon));
}
