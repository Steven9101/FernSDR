#include "band_hours.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace fernsdr {

namespace {

constexpr int64_t kHourMs = 3'600'000;
constexpr int64_t kDayMs = 24 * kHourMs;
constexpr double kRad = 3.14159265358979323846 / 180.0;
constexpr size_t kMostRanges = 16;

int64_t floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The Sun's declination (radians) and the equation of time (minutes) at a
// moment, from NOAA's solar calculator.
void solar_terms(int64_t utc_ms, double& declination, double& equation_minutes) {
    const double t = (static_cast<double>(utc_ms) / 86'400'000.0 + 2440587.5 - 2451545.0) / 36525.0;
    const double mean_longitude = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0) * kRad;
    const double anomaly = (357.52911 + t * (35999.05029 - 0.0001537 * t)) * kRad;
    const double eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double centre = std::sin(anomaly) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                          std::sin(2 * anomaly) * (0.019993 - 0.000101 * t) + std::sin(3 * anomaly) * 0.000289;
    const double node = (125.04 - 1934.136 * t) * kRad;
    const double apparent = (mean_longitude / kRad + centre - 0.00569 - 0.00478 * std::sin(node)) * kRad;
    const double mean_obliquity = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double obliquity = (mean_obliquity + 0.00256 * std::cos(node)) * kRad;
    declination = std::asin(std::sin(obliquity) * std::sin(apparent));
    const double y = std::tan(obliquity / 2) * std::tan(obliquity / 2);
    equation_minutes = 4.0 / kRad *
                       (y * std::sin(2 * mean_longitude) - 2 * eccentricity * std::sin(anomaly) +
                        4 * eccentricity * y * std::sin(anomaly) * std::cos(2 * mean_longitude) -
                        0.5 * y * y * std::sin(4 * mean_longitude) - 1.25 * eccentricity * eccentricity * std::sin(2 * anomaly));
}

// Noon, and the hour angle of the Sun at the horizon in minutes of time
// (0 when it never rises, 720 when it never sets), with the terms taken at
// `at`.
void noon_and_half_day(int64_t day, const StationPlace& place, int64_t at, double& noon_ms, double& half_minutes) {
    double declination = 0.0, equation = 0.0;
    solar_terms(at, declination, equation);
    noon_ms = static_cast<double>(day * kDayMs) + (720.0 - 4.0 * place.lon - equation) * 60'000.0;
    const double lat = place.lat * kRad;
    const double cosine = (std::cos(90.833 * kRad) - std::sin(lat) * std::sin(declination)) / (std::cos(lat) * std::cos(declination));
    if (cosine >= 1.0) half_minutes = 0.0;
    else if (cosine <= -1.0) half_minutes = 720.0;
    else half_minutes = std::acos(cosine) / kRad * 4.0;
}

}  // namespace

SunTimes sun_times(int64_t day, const StationPlace& place) {
    // Once at noon for where the events are, then each again with the terms
    // at its own time: the declination moves by up to 0.4 degrees a day,
    // which is a minute or two of sunrise at mid-latitudes.
    double noon = 0.0, half = 0.0;
    const int64_t mean_noon = day * kDayMs + kDayMs / 2 - static_cast<int64_t>(place.lon / 15.0 * kHourMs);
    noon_and_half_day(day, place, mean_noon, noon, half);
    SunTimes out;
    // Without a sunrise or a sunset, the day's ends are those of the solar
    // day itself, the same instants for this day and its neighbours: each
    // day's own true noon, used instead, left gaps and overlaps of seconds
    // between one day of midnight sun and the next, each a night.
    if (half <= 0.0) {
        out.rise_ms = out.set_ms = mean_noon;
        return out;
    }
    double rise_noon = 0.0, rise_half = 0.0, set_noon = 0.0, set_half = 0.0;
    noon_and_half_day(day, place, static_cast<int64_t>(noon - half * 60'000.0), rise_noon, rise_half);
    noon_and_half_day(day, place, static_cast<int64_t>(noon + half * 60'000.0), set_noon, set_half);
    out.rise_ms = rise_half >= 720.0 ? mean_noon - kDayMs / 2
                                     : static_cast<int64_t>(std::llround(rise_noon - rise_half * 60'000.0));
    out.set_ms = set_half >= 720.0 ? mean_noon + kDayMs / 2
                                   : static_cast<int64_t>(std::llround(set_noon + set_half * 60'000.0));
    if (out.set_ms < out.rise_ms) out.set_ms = out.rise_ms;
    return out;
}

namespace {

// Reads `+1h`, `-30m`, `+1h30m` at `at`, without moving past anything else.
bool read_offset(const std::string& text, size_t& at, int64_t& ms) {
    size_t i = at;
    if (i >= text.size() || (text[i] != '+' && text[i] != '-')) return false;
    const int sign = text[i] == '-' ? -1 : 1;
    i++;
    int64_t total = 0;
    bool any = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int64_t number = 0;
        size_t digits = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && digits < 5) {
            number = number * 10 + (text[i] - '0');
            i++;
            digits++;
        }
        if (i >= text.size() || (text[i] != 'h' && text[i] != 'm')) return false;
        total += number * (text[i] == 'h' ? kHourMs : 60'000);
        i++;
        any = true;
    }
    if (!any) return false;
    ms = sign * total;
    at = i;
    return true;
}

void skip_spaces(const std::string& text, size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) at++;
}

std::string format_offset(int64_t ms) {
    if (ms == 0) return "";
    std::string out = ms < 0 ? "-" : "+";
    const int64_t minutes = std::llabs(ms) / 60'000;
    if (minutes >= 60) out += std::to_string(minutes / 60) + "h";
    if (minutes % 60) out += std::to_string(minutes % 60) + "m";
    return out;
}

}  // namespace

bool BandHours::parse(const std::string& raw, BandHours& out, std::string& error) {
    out = BandHours();
    std::string text;
    for (char c : raw) text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return true;
    text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
    if (text == "always") return true;
    if (text.size() > 256) {
        error = "hours are too long; at most " + std::to_string(kMostRanges) + " ranges";
        return false;
    }

    size_t at = 0;
    const auto point = [&](Point& p) -> bool {
        skip_spaces(text, at);
        if (text.compare(at, 7, "sunrise") == 0 || text.compare(at, 6, "sunset") == 0) {
            const bool rise = text.compare(at, 7, "sunrise") == 0;
            p.kind = rise ? Point::Sunrise : Point::Sunset;
            at += rise ? 7 : 6;
            p.ms = 0;
            skip_spaces(text, at);
            // A '-' here is an offset only when a number with its unit
            // follows ("sunset-1h"); "sunset-06:00" is the range's dash.
            int64_t offset = 0;
            while (read_offset(text, at, offset)) {
                p.ms += offset;
                skip_spaces(text, at);
            }
            if (std::llabs(p.ms) > 12 * kHourMs) {
                error = "an offset from sunrise or sunset is at most 12 hours";
                return false;
            }
            return true;
        }
        int hours = 0, minutes = 0, digits = 0;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) && digits < 2) {
            hours = hours * 10 + (text[at] - '0');
            at++;
            digits++;
        }
        if (digits == 0 || at + 2 >= text.size() || text[at] != ':' ||
            !std::isdigit(static_cast<unsigned char>(text[at + 1])) ||
            !std::isdigit(static_cast<unsigned char>(text[at + 2]))) {
            error = "'" + raw + "' is not hours: write times as 18:00 (UTC), sunrise or sunset, a range as 18:00-06:00";
            return false;
        }
        minutes = (text[at + 1] - '0') * 10 + (text[at + 2] - '0');
        at += 3;
        if (minutes > 59 || hours > 24 || (hours == 24 && minutes != 0)) {
            error = "'" + text.substr(0, at) + "' is not a time of day";
            return false;
        }
        p.kind = Point::Clock;
        p.ms = (static_cast<int64_t>(hours) * 60 + minutes) * 60'000 % kDayMs;
        return true;
    };

    while (at < text.size()) {
        Range range;
        if (!point(range.from)) return false;
        skip_spaces(text, at);
        if (at >= text.size() || text[at] != '-') {
            error = "'" + raw + "' needs a start and an end for each range, as 18:00-06:00";
            return false;
        }
        at++;
        if (!point(range.until)) return false;
        if (range.from.kind == range.until.kind && range.from.ms == range.until.ms) {
            error = "a range cannot start and end at the same time; leave hours empty for always";
            return false;
        }
        out.ranges_.push_back(range);
        if (out.ranges_.size() > kMostRanges) {
            error = "at most " + std::to_string(kMostRanges) + " ranges";
            return false;
        }
        skip_spaces(text, at);
        if (at < text.size()) {
            if (text[at] != ',') {
                error = "'" + raw + "' is not hours: separate ranges with a comma";
                return false;
            }
            at++;
            skip_spaces(text, at);
            if (at >= text.size()) {
                error = "'" + raw + "' ends in a comma";
                return false;
            }
        }
    }
    return true;
}

bool BandHours::uses_sun() const {
    for (const Range& range : ranges_) {
        if (range.from.kind != Point::Clock || range.until.kind != Point::Clock) return true;
    }
    return false;
}

std::string BandHours::text() const {
    if (ranges_.empty()) return "always";
    std::string out;
    const auto write = [&](const Point& p) {
        if (p.kind == Point::Clock) {
            char clock[8];
            std::snprintf(clock, sizeof clock, "%02d:%02d", static_cast<int>(p.ms / kHourMs),
                          static_cast<int>(p.ms / 60'000 % 60));
            out += clock;
        } else {
            out += p.kind == Point::Sunrise ? "sunrise" : "sunset";
            out += format_offset(p.ms);
        }
    };
    for (const Range& range : ranges_) {
        if (!out.empty()) out += ", ";
        write(range.from);
        out += '-';
        write(range.until);
    }
    return out;
}

void BandHours::instances(int64_t day, const StationPlace* place, std::vector<std::pair<int64_t, int64_t>>& out) const {
    // Without a place, a day of 06:00 to 18:00 UTC at longitude 0.
    const double lon = place ? place->lon : 0.0;
    const auto sun = [&](int64_t d) {
        if (place) return sun_times(d, *place);
        SunTimes fixed;
        fixed.rise_ms = d * kDayMs + 6 * kHourMs;
        fixed.set_ms = d * kDayMs + 18 * kHourMs;
        return fixed;
    };
    for (const Range& range : ranges_) {
        const bool clock_only = range.from.kind == Point::Clock && range.until.kind == Point::Clock;
        // The window a day's clock times are placed in: the UTC day for
        // clock ranges, the solar day (local mean midnight to midnight) for
        // ranges that meet the Sun.
        const auto window = [&](int64_t d) {
            return clock_only ? d * kDayMs : d * kDayMs - static_cast<int64_t>(lon / 15.0 * kHourMs);
        };
        const auto resolve = [&](const Point& p, int64_t d) {
            if (p.kind == Point::Clock) {
                const int64_t base = window(d);
                int64_t t = d * kDayMs + p.ms;
                while (t < base) t += kDayMs;
                while (t >= base + kDayMs) t -= kDayMs;
                return t;
            }
            const SunTimes times = sun(d);
            return (p.kind == Point::Sunrise ? times.rise_ms : times.set_ms) + p.ms;
        };
        const int64_t start = resolve(range.from, day);
        int64_t end;
        if (range.from.kind != Point::Clock && range.until.kind != Point::Clock) {
            // Sunrise to sunset stays in the day, sunset to sunrise ends in
            // the next, decided by the kinds alone: offsets that cross leave
            // the day empty rather than turn it into nearly a day. Between
            // two of one kind, the offsets are all there is to go by.
            const bool next_day = range.from.kind != range.until.kind ? range.from.kind == Point::Sunset
                                                                      : range.until.ms <= range.from.ms;
            end = resolve(range.until, next_day ? day + 1 : day);
        } else {
            end = resolve(range.until, day);
            if (end <= start) end = resolve(range.until, day + 1);
        }
        if (end > start) out.emplace_back(start, end);
    }
}

bool BandHours::on_air(int64_t utc_ms, const StationPlace* place) const {
    if (ranges_.empty()) return true;
    std::vector<std::pair<int64_t, int64_t>> spans;
    const int64_t day = floor_div(utc_ms, kDayMs);
    for (int64_t d = day - 2; d <= day + 1; d++) instances(d, place, spans);
    for (const auto& [start, end] : spans) {
        if (start <= utc_ms && utc_ms < end) return true;
    }
    return false;
}

std::vector<std::pair<int64_t, int64_t>> BandHours::intervals(int64_t from_ms, int64_t to_ms, const StationPlace* place) const {
    std::vector<std::pair<int64_t, int64_t>> spans;
    if (to_ms <= from_ms) return spans;
    if (ranges_.empty()) {
        spans.emplace_back(from_ms, to_ms);
        return spans;
    }
    for (int64_t d = floor_div(from_ms, kDayMs) - 2; d <= floor_div(to_ms, kDayMs) + 1; d++) instances(d, place, spans);
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<int64_t, int64_t>> merged;
    for (auto [start, end] : spans) {
        start = std::max(start, from_ms);
        end = std::min(end, to_ms);
        if (end <= start) continue;
        if (!merged.empty() && start <= merged.back().second) merged.back().second = std::max(merged.back().second, end);
        else merged.emplace_back(start, end);
    }
    return merged;
}

int64_t BandHours::next_change(int64_t utc_ms, const StationPlace* place) const {
    if (ranges_.empty()) return -1;
    const int64_t horizon = utc_ms + 9 * kDayMs;
    const auto spans = intervals(utc_ms, horizon, place);
    if (spans.empty()) return -1;
    if (spans.front().first == utc_ms) return spans.front().second < horizon ? spans.front().second : -1;
    return spans.front().first;
}

int64_t hours_overlap(const BandHours& a, const BandHours& b, int64_t from_ms, int days, const StationPlace* place) {
    const int64_t to_ms = from_ms + static_cast<int64_t>(days) * kDayMs;
    const auto first = a.intervals(from_ms, to_ms, place);
    const auto second = b.intervals(from_ms, to_ms, place);
    size_t i = 0, j = 0;
    while (i < first.size() && j < second.size()) {
        const int64_t start = std::max(first[i].first, second[j].first);
        const int64_t end = std::min(first[i].second, second[j].second);
        if (start < end) return start;
        if (first[i].second < second[j].second) i++;
        else j++;
    }
    return -1;
}

}  // namespace fernsdr
