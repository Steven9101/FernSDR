// Turning what the receiver measures into what a signal report means.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace fernsdr {

/**
 * One measured point: at `hz`, add `offset_db` to a dBFS reading to get dBm.
 *
 * Found by feeding a known level in and noting what the receiver says. If a
 * generator putting out -73 dBm reads -50 dBFS, the offset is -23.
 */
struct CalibrationPoint {
    double hz = 0.0;
    double offset_db = 0.0;
};

/**
 * The dBFS-to-dBm correction for one band.
 *
 * Per band, and with more than one point, because neither the antenna nor the
 * front end is flat. A wire that is resonant at 7 MHz is several dB down at
 * 28, and a receiver's own response falls away towards the top of its range;
 * one number for a whole receiver is right at one frequency and wrong
 * everywhere else. So the operator states what they measured, at the
 * frequencies they measured it, and the readings in between are interpolated.
 *
 * Empty means uncalibrated, which is not the same as an offset of zero. An
 * uncalibrated meter is a relative indication and has to be labelled as one:
 * quoting S-units as though they were absolute is how signal reports become
 * fiction. That distinction is the whole reason this is a list rather than a
 * double with a default.
 */
class Calibration {
public:
    bool empty() const { return points_.empty(); }
    const std::vector<CalibrationPoint>& points() const { return points_; }

    /**
     * Replaces the points. They are sorted here so the caller may supply them
     * in any order, and duplicates at one frequency are collapsed to the last
     * one given, which is the one the operator just typed.
     */
    void set_points(std::vector<CalibrationPoint> points) {
        std::stable_sort(points.begin(), points.end(),
                         [](const CalibrationPoint& a, const CalibrationPoint& b) {
                             return a.hz < b.hz;
                         });
        points_.clear();
        for (const auto& point : points) {
            if (!points_.empty() && points_.back().hz == point.hz) {
                points_.back() = point;
            } else {
                points_.push_back(point);
            }
        }
    }

    /**
     * The correction at one frequency, interpolated between the two points
     * either side and held flat beyond the outermost.
     *
     * Held flat rather than extrapolated on purpose: a straight line drawn
     * through two points and followed past them produces confident nonsense at
     * the edges of a wide band. The last measurement is the best available
     * answer out there, and it is honest about being the last one.
     */
    double offset_at(double hz) const {
        if (points_.empty()) return 0.0;
        if (hz <= points_.front().hz) return points_.front().offset_db;
        if (hz >= points_.back().hz) return points_.back().offset_db;

        for (size_t i = 1; i < points_.size(); i++) {
            const CalibrationPoint& high = points_[i];
            if (hz > high.hz) continue;
            const CalibrationPoint& low = points_[i - 1];
            const double span = high.hz - low.hz;
            if (span <= 0.0) return high.offset_db;
            const double t = (hz - low.hz) / span;
            return low.offset_db + t * (high.offset_db - low.offset_db);
        }
        return points_.back().offset_db;
    }

    /**
     * Reads "14.1M:-23.5, 28M:-29" and the like.
     *
     * Returns false and leaves the calibration untouched on anything it does
     * not understand, so a typo in a config file cannot silently produce a
     * meter that is confidently wrong.
     */
    bool parse(const std::string& text, std::string& error);

    /** The inverse of `parse`, for writing the configuration back out. */
    std::string to_string() const;

private:
    std::vector<CalibrationPoint> points_;
};

/**
 * S9, in dBm.
 *
 * IARU Region 1 Technical Recommendation R.1: on the bands below 30 MHz S9 is
 * -73 dBm at the receiver input, and one S-unit is 6 dB.
 */
constexpr double kS9Dbm = -73.0;
constexpr double kDbPerSUnit = 6.0;

}  // namespace fernsdr
