#include "../src/core/band_hours.h"
#include "../src/core/space_weather.h"
#include "test_util.h"

using namespace fernsdr;

namespace {

constexpr int64_t kHour = 3'600'000;
constexpr int64_t kDay = 24 * kHour;

// Days since 1970-01-01 of a date in the proleptic Gregorian calendar.
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

int64_t utc(int y, int m, int d, int hour, int minute) {
    return days_from_civil(y, m, d) * kDay + (hour * 60 + minute) * 60'000LL;
}

BandHours hours(const char* text) {
    BandHours out;
    std::string error;
    CHECK(BandHours::parse(text, out, error));
    return out;
}

// Where the Sun's centre crosses -0.833 degrees, by bisection on the
// elevation space_weather computes with another approximation: an
// independent check of sun_times.
int64_t crossing(int64_t low, int64_t high, const StationPlace& place) {
    const bool rising = sun_elevation_deg(low, place.lat, place.lon) < -0.833;
    for (int i = 0; i < 40; i++) {
        const int64_t middle = low + (high - low) / 2;
        const bool above = sun_elevation_deg(middle, place.lat, place.lon) > -0.833;
        if (above == rising) high = middle;
        else low = middle;
    }
    return low;
}

}  // namespace

TEST_CASE(band_hours_empty_and_always_are_always_on_air) {
    for (const char* text : {"", "  ", "always", "Always"}) {
        const BandHours h = hours(text);
        CHECK(h.always());
        CHECK(h.on_air(utc(2026, 9, 29, 3, 0), nullptr));
        CHECK_EQ(h.next_change(utc(2026, 9, 29, 3, 0), nullptr), -1);
        CHECK(h.text() == std::string("always"));
    }
}

TEST_CASE(band_hours_refuses_what_is_not_hours) {
    for (const char* text : {"18:00", "18-06", "25:00-06:00", "18:60-06:00", "06:00-06:00", "sunrise-sunrise",
                             "18:00-06:00,", "18:00-06:00 12:00-13:00", "noon-sunset", "sunset+13h-sunrise",
                             "sunset+1x-sunrise", "24:30-01:00"}) {
        BandHours out;
        std::string error;
        CHECK(!BandHours::parse(text, out, error));
        CHECK(!error.empty());
    }
}

TEST_CASE(band_hours_are_written_back_in_one_spelling) {
    CHECK(hours("6:00 - 18:00").text() == std::string("06:00-18:00"));
    CHECK(hours("SUNSET -1h - sunrise+90m, 12:00-13:00").text() == std::string("sunset-1h-sunrise+1h30m, 12:00-13:00"));
    CHECK(hours("sunset-06:00").text() == std::string("sunset-06:00"));
    CHECK(hours("22:00-24:00").text() == std::string("22:00-00:00"));
    // What is written back reads as the same hours.
    const BandHours h = hours("sunset-1h30m-sunrise+45m");
    CHECK(hours(h.text().c_str()) == h);
}

TEST_CASE(band_hours_of_clock_times_run_past_midnight) {
    const BandHours night = hours("18:00-06:00");
    CHECK(!night.on_air(utc(2026, 9, 29, 17, 59), nullptr));
    CHECK(night.on_air(utc(2026, 9, 29, 18, 0), nullptr));
    CHECK(night.on_air(utc(2026, 9, 30, 0, 0), nullptr));
    CHECK(night.on_air(utc(2026, 9, 30, 5, 59), nullptr));
    CHECK(!night.on_air(utc(2026, 9, 30, 6, 0), nullptr));
    CHECK_EQ(night.next_change(utc(2026, 9, 29, 12, 0), nullptr), utc(2026, 9, 29, 18, 0));
    CHECK_EQ(night.next_change(utc(2026, 9, 29, 18, 0), nullptr), utc(2026, 9, 30, 6, 0));
    CHECK_EQ(night.next_change(utc(2026, 9, 30, 2, 0), nullptr), utc(2026, 9, 30, 6, 0));

    const BandHours late = hours("22:00-24:00");
    CHECK(late.on_air(utc(2026, 9, 29, 23, 59), nullptr));
    CHECK(!late.on_air(utc(2026, 9, 30, 0, 0), nullptr));

    // Two ranges that touch are one stretch, with no change where they meet.
    const BandHours joined = hours("06:00-12:00, 12:00-18:00");
    CHECK_EQ(joined.next_change(utc(2026, 9, 29, 7, 0), nullptr), utc(2026, 9, 29, 18, 0));
    CHECK_EQ(joined.intervals(utc(2026, 9, 29, 0, 0), utc(2026, 9, 30, 0, 0), nullptr).size(), 1u);
}

TEST_CASE(band_hours_sun_times_match_the_published_ones) {
    // Berlin on the solstices: sunrise 04:43 and sunset 21:33 CEST on
    // 21 June, 08:15 and 15:54 CET on 21 December (timeanddate.com).
    const StationPlace berlin{52.52, 13.405};
    SunTimes june = sun_times(days_from_civil(2026, 6, 21), berlin);
    CHECK_NEAR(static_cast<double>(june.rise_ms), static_cast<double>(utc(2026, 6, 21, 2, 43)), 2 * 60'000.0);
    CHECK_NEAR(static_cast<double>(june.set_ms), static_cast<double>(utc(2026, 6, 21, 19, 33)), 2 * 60'000.0);
    SunTimes december = sun_times(days_from_civil(2026, 12, 21), berlin);
    CHECK_NEAR(static_cast<double>(december.rise_ms), static_cast<double>(utc(2026, 12, 21, 7, 15)), 2 * 60'000.0);
    CHECK_NEAR(static_cast<double>(december.set_ms), static_cast<double>(utc(2026, 12, 21, 14, 54)), 2 * 60'000.0);
}

TEST_CASE(band_hours_sun_times_agree_with_the_elevation_everywhere) {
    // From the equator to the polar circles, east and west, through a year:
    // within two minutes of where the other approximation of the Sun's
    // position puts the crossings.
    int compared = 0;
    for (const StationPlace place : {StationPlace{0.0, 0.0}, StationPlace{52.5, 13.4}, StationPlace{42.4, -71.1},
                                     StationPlace{-33.9, 151.2}, StationPlace{64.1, -21.9}, StationPlace{35.7, 139.7},
                                     StationPlace{-54.8, -68.3}, StationPlace{40.7, -111.9}}) {
        for (int month = 1; month <= 12; month++) {
            const int64_t day = days_from_civil(2026, month, 15);
            const SunTimes times = sun_times(day, place);
            if (times.set_ms - times.rise_ms < 2 * kHour || times.set_ms - times.rise_ms > 22 * kHour) continue;
            const int64_t noon = (times.rise_ms + times.set_ms) / 2;
            CHECK_NEAR(static_cast<double>(times.rise_ms), static_cast<double>(crossing(noon - 12 * kHour, noon, place)), 2 * 60'000.0);
            CHECK_NEAR(static_cast<double>(times.set_ms), static_cast<double>(crossing(noon, noon + 12 * kHour, place)), 2 * 60'000.0);
            compared++;
        }
    }
    CHECK(compared > 80);
}

TEST_CASE(band_hours_at_sunset_follow_the_seasons) {
    const StationPlace berlin{52.52, 13.405};
    const BandHours night = hours("sunset-sunrise");
    const BandHours day = hours("sunrise-sunset");
    // At 20:00 UTC it is night in December, day in June.
    CHECK(night.on_air(utc(2026, 12, 21, 20, 0), &berlin));
    CHECK(!day.on_air(utc(2026, 12, 21, 20, 0), &berlin));
    CHECK(!night.on_air(utc(2026, 6, 21, 19, 0), &berlin));
    CHECK(day.on_air(utc(2026, 6, 21, 19, 0), &berlin));
    // The change is at the sunset itself.
    const SunTimes june = sun_times(days_from_civil(2026, 6, 21), berlin);
    CHECK_EQ(day.next_change(utc(2026, 6, 21, 12, 0), &berlin), june.set_ms);
    CHECK_EQ(night.next_change(utc(2026, 6, 21, 12, 0), &berlin), june.set_ms);
    // Offsets move it.
    CHECK_EQ(hours("sunset-1h-sunrise").next_change(utc(2026, 6, 21, 12, 0), &berlin), june.set_ms - kHour);
}

TEST_CASE(band_hours_at_night_are_one_night_in_any_longitude) {
    // Boston's sunset is after midnight UTC in summer: sunset to sunrise is
    // still the one night from that sunset to the next morning's sunrise.
    const StationPlace boston{42.36, -71.06};
    const BandHours night = hours("sunset-sunrise");
    const auto spans = night.intervals(utc(2026, 6, 21, 12, 0), utc(2026, 6, 22, 18, 0), &boston);
    CHECK_EQ(spans.size(), 1u);
    if (spans.size() == 1) {
        CHECK_NEAR(static_cast<double>(spans[0].first), static_cast<double>(utc(2026, 6, 22, 0, 25)), 3 * 60'000.0);
        CHECK_NEAR(static_cast<double>(spans[0].second), static_cast<double>(utc(2026, 6, 22, 9, 8)), 3 * 60'000.0);
    }
    // Clock times against the Sun: from sunset to 06:00 UTC, a short night.
    const auto early = hours("sunset-06:00").intervals(utc(2026, 6, 21, 12, 0), utc(2026, 6, 22, 12, 0), &boston);
    CHECK_EQ(early.size(), 1u);
    if (early.size() == 1) CHECK_EQ(early[0].second, utc(2026, 6, 22, 6, 0));
}

TEST_CASE(band_hours_beyond_the_polar_circle) {
    const StationPlace tromso{69.65, 18.96};
    const BandHours day = hours("sunrise-sunset");
    const BandHours night = hours("sunset-sunrise");
    // Midnight sun: day all day and no night at all.
    const auto summer_day = day.intervals(utc(2026, 6, 21, 0, 0), utc(2026, 6, 23, 0, 0), &tromso);
    CHECK_EQ(summer_day.size(), 1u);
    if (summer_day.size() == 1) CHECK_EQ(summer_day[0].second - summer_day[0].first, 2 * kDay);
    CHECK(night.intervals(utc(2026, 6, 21, 0, 0), utc(2026, 6, 23, 0, 0), &tromso).empty());
    // Polar night: the other way round.
    CHECK(day.intervals(utc(2026, 12, 21, 0, 0), utc(2026, 12, 23, 0, 0), &tromso).empty());
    const auto winter_night = night.intervals(utc(2026, 12, 21, 0, 0), utc(2026, 12, 23, 0, 0), &tromso);
    CHECK_EQ(winter_night.size(), 1u);
    // Offsets that cross on a short day leave nothing, not nearly a day.
    const StationPlace north{66.0, 15.0};
    const auto short_day = hours("sunrise+3h-sunset-3h").intervals(utc(2026, 12, 15, 0, 0), utc(2026, 12, 16, 0, 0), &north);
    CHECK(short_day.empty());
}

TEST_CASE(band_hours_overlap_finds_where_two_bands_would_share_an_input) {
    const StationPlace berlin{52.52, 13.405};
    const int64_t from = utc(2026, 9, 29, 0, 0);
    // Day and night at the station never meet, in any season.
    CHECK_EQ(hours_overlap(hours("sunrise-sunset"), hours("sunset-sunrise"), from, 366, &berlin), -1);
    CHECK_EQ(hours_overlap(hours("06:00-18:00"), hours("18:00-06:00"), from, 366, &berlin), -1);
    // A fixed night against the Sun's day does, in summer.
    const int64_t at = hours_overlap(hours("sunrise-sunset"), hours("19:00-05:00"), from, 366, &berlin);
    CHECK(at > from);
    CHECK(hours("sunrise-sunset").on_air(at, &berlin) && hours("19:00-05:00").on_air(at, &berlin));
    CHECK(hours_overlap(hours("always"), hours("12:00-13:00"), from, 1, &berlin) == utc(2026, 9, 29, 12, 0));
}

TEST_CASE(band_hours_without_a_place_take_a_fixed_day) {
    const BandHours day = hours("sunrise-sunset");
    CHECK(day.uses_sun());
    CHECK(!hours("06:00-18:00").uses_sun());
    CHECK(day.on_air(utc(2026, 9, 29, 12, 0), nullptr));
    CHECK(!day.on_air(utc(2026, 9, 29, 20, 0), nullptr));
}

TEST_CASE(band_hours_offsets_never_turn_a_range_inside_out) {
    // Sunrise to sunset stays in its day whatever the offsets: crossed ones
    // leave the day empty, where they made a range of 22 hours.
    const int64_t from = utc(2026, 9, 29, 0, 0);
    const int64_t to = utc(2026, 10, 1, 0, 0);
    CHECK(hours("sunrise+7h-sunset-7h").intervals(from, to, nullptr).empty());
    const StationPlace berlin{52.52, 13.405};
    CHECK(hours("sunrise+7h-sunset-7h").intervals(from, to, &berlin).empty());
    // Sunset to sunrise ends in the next day, however far the offsets move
    // its ends towards each other: 11:00 to 13:00 the day after.
    CHECK(hours("sunset-7h-sunrise+7h").on_air(from + 15 * kHour, nullptr));
    // Between two of the same kind, the offsets are all there is to go by.
    const auto evening = hours("sunset-1h-sunset+2h").intervals(from, from + kDay, nullptr);
    CHECK_EQ(evening.size(), 1u);
    if (evening.size() == 1) CHECK_EQ(evening[0].second - evening[0].first, 3 * kHour);
    CHECK(hours("sunset+1h-sunset").on_air(from + 12 * kHour, nullptr));
    CHECK(!hours("sunset+1h-sunset").on_air(from + 18 * kHour + 30 * 60'000, nullptr));
}
