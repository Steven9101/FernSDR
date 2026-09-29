// When a band is on the air: the `hours` of its section.
//
// A receiver with one SDR that shows 20 m in the day and 40 m at night is
// two bands on one input, each with its hours; the schedule runs whichever
// is on the air and the other waits. Hours are clock times in UTC or the
// sunrise and sunset at the station, which follow the seasons as the bands
// do:
//
//   hours = 06:00-18:00
//   hours = sunrise-sunset
//   hours = sunset-1h-sunrise+30m, 12:00-13:00
//
// Empty, or `always`, is always on the air.
//
// Each range is taken once per day. A range of clock times whose end is at
// or before its start runs past midnight. Sunrise and sunset belong to the
// station's solar day, the day around its local noon, so `sunset-sunrise`
// is one night wherever the station is, not the pieces of two nights either
// side of UTC midnight. A range from sunrise to sunset stays in its day and
// one from sunset to sunrise ends in the next, whatever the offsets; where
// the offsets cross (sunrise+2h-sunset-2h on a four-hour winter day) the
// range is empty that day rather than turned inside out. At the poles a day
// without a sunset is day from end to end, and one without a sunrise is
// night.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fernsdr {

// Where the station is, for sunrise and sunset: degrees, east and north
// positive.
struct StationPlace {
    double lat = 0.0;
    double lon = 0.0;
};

// Sunrise and sunset, UTC milliseconds, of the solar day `day` at a place:
// the day whose mean local noon is day * 86400000 + 12 h - lon / 15 h.
// NOAA's equations, with the upper limb and refraction at -0.833 degrees;
// good to about a minute. Without a sunrise both are the mean local noon,
// without a sunset they are the mean local midnights 12 hours either side of
// it, so days of midnight sun or polar night join without gaps.
struct SunTimes {
    int64_t rise_ms = 0;
    int64_t set_ms = 0;
};
SunTimes sun_times(int64_t day, const StationPlace& place);

class BandHours {
public:
    // False with a reason for the operator when `text` is not hours.
    static bool parse(const std::string& text, BandHours& out, std::string& error);

    bool always() const { return ranges_.empty(); }
    bool uses_sun() const;
    // The hours as written back: `always`, or the ranges in one spelling.
    std::string text() const;

    // A place is needed only when uses_sun(); without one, sunrise and
    // sunset are taken at 06:00 and 18:00 UTC so the answer is still defined.
    bool on_air(int64_t utc_ms, const StationPlace* place) const;
    // The first moment after `utc_ms` at which on_air() changes, looked for
    // over the next nine days; -1 when it does not change in that time.
    int64_t next_change(int64_t utc_ms, const StationPlace* place) const;
    // The on-air stretches that meet [from_ms, to_ms), merged and clipped.
    std::vector<std::pair<int64_t, int64_t>> intervals(int64_t from_ms, int64_t to_ms, const StationPlace* place) const;

    bool operator==(const BandHours& other) const { return text() == other.text(); }
    bool operator!=(const BandHours& other) const { return !(*this == other); }

private:
    struct Point {
        enum Kind { Clock, Sunrise, Sunset } kind = Clock;
        int64_t ms = 0;  // after midnight for a clock time, the offset for the Sun
    };
    struct Range {
        Point from;
        Point until;
    };
    void instances(int64_t day, const StationPlace* place, std::vector<std::pair<int64_t, int64_t>>& out) const;

    std::vector<Range> ranges_;
};

// The first moment in the `days` from `from_ms` at which both are on the
// air, or -1 when there is none: two bands on one input must never be.
int64_t hours_overlap(const BandHours& a, const BandHours& b, int64_t from_ms, int days, const StationPlace* place);

}  // namespace fernsdr
