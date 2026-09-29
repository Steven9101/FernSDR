#include "space_weather.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "../update/system.h"
#include "../util/json.h"
#include "directory.h"

namespace fernsdr {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;

// NOAA gives some numbers as numbers and some as strings ("Scale": "0",
// KC2G's "latitude": "30.4"); either is a number here, anything else NaN.
double number_of(const Json& value) {
    if (value.is_number()) return value.number();
    if (value.is_string() && !value.string().empty()) {
        char* end = nullptr;
        const double parsed = std::strtod(value.string().c_str(), &end);
        if (end && *end == '\0') return parsed;
    }
    return std::nan("");
}

double rounded(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

// "2026-09-28T09:15:00" or with a "Z": milliseconds since 1970, UTC; -1 when
// it is neither.
int64_t utc_ms(const std::string& text) {
    int year, month, day, hour, minute, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 5) return -1;
    // Days since 1970 by the civil calendar, without the local time zone that
    // mktime would bring in.
    const int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return ((days * 24 + hour) * 60 + minute) * 60'000LL + second * 1000LL;
}

Json parsed(const std::string& text) {
    Json out;
    if (text.empty() || !Json::parse(text, out)) return Json();
    return out;
}

// The last element of an array of objects, or null.
const Json& last_of(const Json& array) {
    static const Json none;
    return array.is_array() && array.size() > 0 ? array[array.size() - 1] : none;
}

}  // namespace

double sun_elevation_deg(int64_t utc_ms_value, double lat, double lon) {
    const double n = utc_ms_value / 86'400'000.0 + 2440587.5 - 2451545.0;  // days since J2000
    const double mean_longitude = std::fmod(280.460 + 0.9856474 * n, 360.0);
    const double anomaly = std::fmod(357.528 + 0.9856003 * n, 360.0) * kRad;
    const double ecliptic = (mean_longitude + 1.915 * std::sin(anomaly) + 0.020 * std::sin(2 * anomaly)) * kRad;
    const double obliquity = (23.439 - 0.0000004 * n) * kRad;
    const double right_ascension = std::atan2(std::cos(obliquity) * std::sin(ecliptic), std::cos(ecliptic));
    const double declination = std::asin(std::sin(obliquity) * std::sin(ecliptic));
    const double sidereal_hours = std::fmod(18.697374558 + 24.06570982441908 * n, 24.0);
    const double hour_angle = (sidereal_hours * 15.0 + lon) * kRad - right_ascension;
    const double elevation = std::asin(std::sin(lat * kRad) * std::sin(declination) +
                                       std::cos(lat * kRad) * std::cos(declination) * std::cos(hour_angle));
    return elevation / kRad;
}

std::string flare_class(double flux) {
    if (!(flux > 0)) return "";
    static const char classes[] = {'A', 'B', 'C', 'M', 'X'};
    int index = static_cast<int>(std::floor(std::log10(flux))) + 8;  // 1e-8 is A1
    index = std::clamp(index, 0, 4);
    const double scale = std::pow(10.0, index - 8);
    char text[16];
    // Cut, not rounded, as the classes are read: 9.97e-6 is C9.9, never C10.0.
    // X goes past X9.9 (X17 in 2003) rather than into another letter.
    std::snprintf(text, sizeof text, "%c%.1f", classes[index], std::floor(flux / scale * 10 + 1e-9) / 10);
    return text;
}

bool locator_centre(const std::string& grid, double& lat, double& lon) {
    if (!valid_grid_locator(grid)) return false;
    const auto upper = [&](size_t i) { return std::toupper(static_cast<unsigned char>(grid[i])); };
    lon = (upper(0) - 'A') * 20 - 180 + (grid[2] - '0') * 2;
    lat = (upper(1) - 'A') * 10 - 90 + (grid[3] - '0');
    if (grid.size() == 6) {
        lon += (upper(4) - 'A') * (5.0 / 60) + 2.5 / 60;
        lat += (upper(5) - 'A') * (2.5 / 60) + 1.25 / 60;
    } else {
        lon += 1;
        lat += 0.5;
    }
    return true;
}

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double dlat = (lat2 - lat1) * kRad;
    const double dlon = (lon2 - lon1) * kRad;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * kRad) * std::cos(lat2 * kRad) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

std::vector<BandOutlook> band_outlook(double mufd, double fof2, bool day, double kp, double lat) {
    static const struct { const char* band; double mhz; } kBands[] = {
        {"160 m", 1.9},  {"80 m", 3.6},  {"60 m", 5.35}, {"40 m", 7.1},  {"30 m", 10.12}, {"20 m", 14.2},
        {"17 m", 18.1},  {"15 m", 21.2}, {"12 m", 24.94}, {"10 m", 28.5}, {"6 m", 50.2},
    };
    const bool reading = mufd > 0 && fof2 > 0;
    std::vector<BandOutlook> out;
    for (const auto& band : kBands) {
        BandOutlook outlook{band.band, band.mhz, "unknown", false, ""};
        if (reading) {
            const double ratio = band.mhz / mufd;
            outlook.dx = ratio <= 0.85 ? "open" : ratio <= 1.0 ? "marginal" : "closed";
            outlook.nearby = band.mhz <= fof2 && band.mhz < 30;
            if (band.mhz >= 50 && mufd < 50) {
                outlook.dx = "closed";
                outlook.note = "F2 does not reach 50 MHz; sporadic E is not forecast";
            } else if (day && band.mhz < 5) {
                // The D layer, there only while the Sun is up, absorbs the low
                // bands on the way to the F2 layer and back.
                outlook.dx = "closed";
                outlook.note = "absorbed by day";
                if (band.mhz < 3) outlook.nearby = false;
            }
            const bool storm = kp >= 7 || (kp >= 5 && std::fabs(lat) >= 45);
            if (storm && std::string(outlook.dx) != "closed") {
                outlook.dx = std::string(outlook.dx) == "open" ? "marginal" : "closed";
                outlook.note = "geomagnetic storm";
            }
        }
        out.push_back(outlook);
    }
    return out;
}

std::string space_weather_json(const SpaceWeatherSources& sources, int64_t now_ms, double lat, double lon) {
    Json out = Json::make_object();
    out.set("updated", static_cast<double>(now_ms));

    // NOAA's scales now: radio blackouts, solar radiation storms, geomagnetic storms.
    const Json scales = parsed(sources.scales);
    if (scales.is_object() && scales["0"].is_object()) {
        const Json& today = scales["0"];
        Json now = Json::make_object();
        for (const char* scale : {"R", "S", "G"}) {
            const double level = number_of(today[scale]["Scale"]);
            if (std::isfinite(level)) now.set(std::string(1, static_cast<char>(scale[0] + 32)), level);
        }
        out.set("scales", now);
        // Tomorrow's geomagnetic forecast, the one listeners plan by.
        const double tomorrow = number_of(scales["1"]["G"]["Scale"]);
        if (std::isfinite(tomorrow)) out.set("g_tomorrow", tomorrow);
    }

    double kp_now = std::nan("");
    const Json kp = parsed(sources.kp);
    if (kp.is_array() && kp.size() > 0) {
        Json history = Json::make_array();
        const size_t first = kp.size() > 8 ? kp.size() - 8 : 0;  // the last day, in 3-hour steps
        for (size_t i = first; i < kp.size(); ++i) {
            const double value = number_of(kp[i]["Kp"]);
            if (!std::isfinite(value)) continue;
            Json point = Json::make_object();
            point.set("time", kp[i]["time_tag"].string());
            point.set("kp", rounded(value, 2));
            history.push_back(point);
            kp_now = value;
        }
        if (std::isfinite(kp_now)) out.set("kp", rounded(kp_now, 2));
        out.set("kp_history", history);
        const double a = number_of(last_of(kp)["a_running"]);
        if (std::isfinite(a)) out.set("a", a);
    }

    const double flux = number_of(last_of(parsed(sources.flux))["flux"]);
    if (std::isfinite(flux)) out.set("sfi", flux);

    const double speed = number_of(last_of(parsed(sources.wind_speed))["proton_speed"]);
    if (std::isfinite(speed)) out.set("wind_speed", speed);
    const Json field_readings = parsed(sources.wind_field);
    const Json& field = last_of(field_readings);
    const double bz = number_of(field["bz_gsm"]);
    const double bt = number_of(field["bt"]);
    if (std::isfinite(bz)) out.set("bz", bz);
    if (std::isfinite(bt)) out.set("bt", bt);

    // GOES X-rays, the long channel that the flare classes are defined on:
    // the latest reading and the largest of the six hours.
    const Json xrays = parsed(sources.xrays);
    if (xrays.is_array()) {
        double latest = std::nan(""), peak = 0;
        for (const Json& row : xrays.elements()) {
            if (row["energy"].string() != "0.1-0.8nm") continue;
            const double value = number_of(row["flux"]);
            if (!(value > 0)) continue;
            latest = value;
            peak = std::max(peak, value);
        }
        if (std::isfinite(latest)) {
            out.set("xray", flare_class(latest));
            out.set("xray_peak", flare_class(peak));
        }
    }

    // The sunspot number of the last day in NOAA's daily table: the fifth
    // column of its last data line.
    {
        std::istringstream lines(sources.indices);
        std::string line;
        double ssn = std::nan("");
        while (std::getline(lines, line)) {
            if (line.empty() || line[0] == ':' || line[0] == '#') continue;
            int year, month, day, radio, number;
            if (std::sscanf(line.c_str(), "%d %d %d %d %d", &year, &month, &day, &radio, &number) == 5 && number >= 0)
                ssn = number;
        }
        if (std::isfinite(ssn)) out.set("ssn", ssn);
    }

    // The nearest ionosonde with a reading from the last ninety minutes, and
    // not one its own software scored as unreliable (confidence 0; -1 means
    // the station does not score).
    const bool placed = std::isfinite(lat) && std::isfinite(lon);
    const Json stations = parsed(sources.ionosondes);
    const Json* nearest = nullptr;
    double nearest_km = 0;
    if (placed && stations.is_array()) {
        for (const Json& reading : stations.elements()) {
            const int64_t at = utc_ms(reading["time"].string());
            const double mufd = number_of(reading["mufd"]);
            const double fof2 = number_of(reading["fof2"]);
            const double confidence = number_of(reading["cs"]);
            if (at < 0 || now_ms - at > 90 * 60'000 || at - now_ms > 10 * 60'000) continue;
            if (!(mufd > 0) || !(fof2 > 0) || confidence == 0) continue;
            double slat = number_of(reading["station"]["latitude"]);
            double slon = number_of(reading["station"]["longitude"]);
            if (!std::isfinite(slat) || !std::isfinite(slon)) continue;
            if (slon > 180) slon -= 360;
            const double km = distance_km(lat, lon, slat, slon);
            if (!nearest || km < nearest_km) {
                nearest = &reading;
                nearest_km = km;
            }
        }
    }

    double mufd = 0, fof2 = 0;
    if (nearest) {
        mufd = number_of((*nearest)["mufd"]);
        fof2 = number_of((*nearest)["fof2"]);
        Json station = Json::make_object();
        station.set("name", (*nearest)["station"]["name"].string());
        station.set("code", (*nearest)["station"]["code"].string());
        station.set("km", std::round(nearest_km));
        station.set("time", (*nearest)["time"].string());
        station.set("mufd", rounded(mufd, 1));
        station.set("fof2", rounded(fof2, 2));
        out.set("ionosonde", station);
    }
    if (placed) {
        const double elevation = sun_elevation_deg(now_ms, lat, lon);
        out.set("sun", rounded(elevation, 1));
        Json bands = Json::make_array();
        for (const BandOutlook& band : band_outlook(mufd, fof2, elevation > 0, std::isfinite(kp_now) ? kp_now : 0, lat)) {
            Json entry = Json::make_object();
            entry.set("band", band.band);
            entry.set("mhz", band.mhz);
            entry.set("dx", band.dx);
            entry.set("nearby", band.nearby);
            if (*band.note) entry.set("note", band.note);
            bands.push_back(entry);
        }
        out.set("bands", bands);
    }
    return out.serialize();
}

namespace {

struct Source {
    const char* url;
    size_t limit;
    std::string SpaceWeatherSources::*field;
};

const Source kSources[] = {
    {"https://services.swpc.noaa.gov/products/noaa-scales.json", 64 << 10, &SpaceWeatherSources::scales},
    {"https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json", 256 << 10, &SpaceWeatherSources::kp},
    {"https://services.swpc.noaa.gov/products/summary/10cm-flux.json", 16 << 10, &SpaceWeatherSources::flux},
    {"https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json", 16 << 10,
     &SpaceWeatherSources::wind_speed},
    {"https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json", 16 << 10,
     &SpaceWeatherSources::wind_field},
    {"https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json", 2 << 20, &SpaceWeatherSources::xrays},
    {"https://services.swpc.noaa.gov/text/daily-solar-indices.txt", 64 << 10, &SpaceWeatherSources::indices},
    {"https://prop.kc2g.com/api/stations.json", 1 << 20, &SpaceWeatherSources::ionosondes},
};

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

SpaceWeather::SpaceWeather() : SpaceWeather(fetch_release_file) {}

SpaceWeather::SpaceWeather(Fetch fetch) : fetch_(std::move(fetch)) { thread_ = std::thread([this] { run(); }); }

SpaceWeather::~SpaceWeather() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void SpaceWeather::configure(bool enabled, double lat, double lon) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool moved = !(lat == lat_ || (std::isnan(lat) && std::isnan(lat_))) ||
                       !(lon == lon_ || (std::isnan(lon) && std::isnan(lon_)));
    lat_ = lat;
    lon_ = lon;
    if (enabled && !enabled_) due_ = true;
    enabled_ = enabled;
    // A new locator changes the ionosonde and the bands, not the data: they
    // are worked out again from what is already here.
    if (moved && !json_.empty()) json_ = space_weather_json(last_, wall_ms(), lat_, lon_);
    wake_.notify_all();
}

std::string SpaceWeather::json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return json_;
}

void SpaceWeather::settle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return stopping_ || (!busy_ && !due_); });
}

void SpaceWeather::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    int64_t wait_ms = kIntervalMs;
    while (!stopping_) {
        if (!enabled_ || !due_) {
            if (enabled_) {
                // Due again after the interval, or sooner when turned on anew.
                if (!wake_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                                    [this] { return stopping_ || due_ || !enabled_; }))
                    due_ = true;
            } else {
                wake_.wait(lock, [this] { return stopping_ || enabled_; });
            }
            continue;
        }
        busy_ = true;
        due_ = false;
        lock.unlock();

        SpaceWeatherSources got;
        int answered = 0;
        for (const Source& source : kSources) {
            std::string body, error;
            if (fetch_(source.url, source.limit, body, error)) {
                got.*source.field = std::move(body);
                ++answered;
            }
        }

        lock.lock();
        for (const Source& source : kSources)
            if (!(got.*source.field).empty()) last_.*source.field = std::move(got.*source.field);
        if (answered > 0 || !json_.empty()) json_ = space_weather_json(last_, wall_ms(), lat_, lon_);
        wait_ms = answered > 0 ? kIntervalMs : kRetryMs;
        busy_ = false;
        idle_.notify_all();
    }
}

}  // namespace fernsdr
