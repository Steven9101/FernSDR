// Space weather for the listener page's widget: NOAA SWPC's scales, indices
// and solar wind, and the F2 layer over the station as the nearest ionosonde
// measures it, with what that means for each HF band.
//
// The receiver fetches it, not the listeners' browsers: every listener's
// address would otherwise go to NOAA and to KC2G each time the page opens,
// and a hundred listeners would ask a hundred times for the same numbers.
// It fetches only while an operator has the widget on the page.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fernsdr {

// What each source returned, as text; empty for a source that failed.
struct SpaceWeatherSources {
    std::string scales;        // products/noaa-scales.json
    std::string kp;            // products/noaa-planetary-k-index.json
    std::string flux;          // products/summary/10cm-flux.json
    std::string wind_speed;    // products/summary/solar-wind-speed.json
    std::string wind_field;    // products/summary/solar-wind-mag-field.json
    std::string xrays;         // json/goes/primary/xrays-6-hour.json
    std::string indices;       // text/daily-solar-indices.txt
    std::string ionosondes;    // prop.kc2g.com/api/stations.json
};

// The Sun's elevation above the horizon in degrees, at a place and a time
// (milliseconds since 1970, UTC). The approximation NOAA's solar calculator
// uses, good to a small fraction of a degree: enough to call day and night.
double sun_elevation_deg(int64_t utc_ms, double lat, double lon);

// The GOES class of an X-ray flux in W/m² (0.1-0.8 nm): "B9.9", "C1.2", "X2.1".
std::string flare_class(double flux);

// The centre of a Maidenhead locator of 4 or 6 characters; false for
// anything else.
bool locator_centre(const std::string& grid, double& lat, double& lon);

// Great-circle distance in kilometres.
double distance_km(double lat1, double lon1, double lat2, double lon2);

struct BandOutlook {
    const char* band;   // "40 m"
    double mhz;         // a frequency in it
    // Beyond a thousand kilometres or so, by one hop off the F2 layer:
    // "open", "marginal", "closed", or "unknown" without a reading.
    const char* dx;
    // Within a few hundred kilometres, straight up and down (NVIS).
    bool nearby;
    const char* note;   // why, where it is not the MUF alone; "" otherwise
};

// Each amateur HF band, and 6 m, from the F2 layer's MUF for 3000 km and its
// critical frequency where the station is, whether the Sun is up there, and
// the planetary K index. A rule of thumb, and the widget says so: a band a
// little under the MUF carries a hop, the D layer swallows 160 to 60 m by
// day, a storm (Kp 5 and up) closes paths across high latitudes first, and
// 6 m by F2 needs a MUF above 50 MHz, which a sporadic-E opening does not.
// mufd or fof2 not above zero means no reading.
std::vector<BandOutlook> band_outlook(double mufd, double fof2, bool day, double kp, double lat);

// The widget's JSON from whatever the sources returned. lat and lon are the
// station's, NaN when it has no locator: then no ionosonde is chosen.
std::string space_weather_json(const SpaceWeatherSources& sources, int64_t now_ms, double lat, double lon);

class SpaceWeather {
public:
    static constexpr int64_t kIntervalMs = 10 * 60'000;
    // After a round in which nothing came back.
    static constexpr int64_t kRetryMs = 2 * 60'000;

    using Fetch = std::function<bool(const std::string& url, size_t limit, std::string& body, std::string& error)>;

    SpaceWeather();              // fetches with curl or wget
    explicit SpaceWeather(Fetch fetch);
    ~SpaceWeather();
    SpaceWeather(const SpaceWeather&) = delete;
    SpaceWeather& operator=(const SpaceWeather&) = delete;

    // On while a widget of the kind is on the page; off, nothing is fetched.
    // The position is the station's locator's, NaN when there is none.
    void configure(bool enabled, double lat, double lon);

    // The widget's JSON from the last round, empty before the first.
    std::string json() const;

    // Waits for a round under way to finish; for tests.
    void settle();

private:
    void run();

    Fetch fetch_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::thread thread_;
    bool stopping_ = false;
    bool enabled_ = false;
    bool busy_ = false;
    bool due_ = false;
    double lat_ = 0;
    double lon_ = 0;
    // Each source's last good answer, kept when a later fetch of it fails.
    SpaceWeatherSources last_;
    std::string json_;
};

}  // namespace fernsdr
