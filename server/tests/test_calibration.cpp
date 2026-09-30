#include "../src/core/calibration.h"
#include "test_util.h"

#include <cmath>
#include <string>

using fernsdr::Calibration;
using fernsdr::CalibrationPoint;

TEST_CASE(calibration_empty_is_not_the_same_as_zero) {
    Calibration cal;
    CHECK(cal.empty());
    // The offset is zero either way, but "empty" is what tells the client to
    // label the meter as uncalibrated rather than quote absolute S-units.
    CHECK(cal.offset_at(14.2e6) == 0.0);
}

TEST_CASE(calibration_one_point_applies_everywhere) {
    Calibration cal;
    cal.set_points({{14.2e6, -23.5}});
    CHECK(std::fabs(cal.offset_at(1.8e6) - -23.5) < 1e-9);
    CHECK(std::fabs(cal.offset_at(28.0e6) - -23.5) < 1e-9);
}

TEST_CASE(calibration_interpolates_between_measurements) {
    Calibration cal;
    // Flat at 7 MHz, 6 dB down at 28: the shape of a wire cut for one band.
    cal.set_points({{7.0e6, -20.0}, {28.0e6, -26.0}});
    CHECK(std::fabs(cal.offset_at(7.0e6) - -20.0) < 1e-9);
    CHECK(std::fabs(cal.offset_at(28.0e6) - -26.0) < 1e-9);
    CHECK(std::fabs(cal.offset_at(17.5e6) - -23.0) < 1e-9);
}

TEST_CASE(calibration_holds_flat_beyond_the_outermost_point) {
    Calibration cal;
    cal.set_points({{7.0e6, -20.0}, {28.0e6, -26.0}});
    // Extrapolating a straight line past the measurements produces confident
    // nonsense at the edges; the last measurement is the honest answer.
    CHECK(std::fabs(cal.offset_at(1.8e6) - -20.0) < 1e-9);
    CHECK(std::fabs(cal.offset_at(50.0e6) - -26.0) < 1e-9);
}

TEST_CASE(calibration_sorts_and_collapses_what_it_is_given) {
    Calibration cal;
    cal.set_points({{28.0e6, -26.0}, {7.0e6, -20.0}, {7.0e6, -21.0}});
    CHECK(cal.points().size() == 2);
    CHECK(cal.points()[0].hz == 7.0e6);
    // The later entry at a repeated frequency wins: it is the one just typed.
    CHECK(std::fabs(cal.points()[0].offset_db - -21.0) < 1e-9);
}

TEST_CASE(calibration_parses_the_configuration_form) {
    Calibration cal;
    std::string error;
    CHECK(cal.parse("14.1M:-23.5, 28M:-29", error));
    CHECK(error.empty());
    CHECK(cal.points().size() == 2);
    CHECK(std::fabs(cal.points()[0].hz - 14.1e6) < 1.0);
    CHECK(std::fabs(cal.points()[1].offset_db - -29.0) < 1e-9);

    CHECK(cal.parse("7100k:+12", error));
    CHECK(std::fabs(cal.points()[0].hz - 7.1e6) < 1.0);
    CHECK(std::fabs(cal.points()[0].offset_db - 12.0) < 1e-9);

    // The suffixes the rest of the file takes.
    CHECK(cal.parse("14.1 MHz:-3, 7100kHz:-2", error));
    CHECK(cal.points().size() == 2);
}

TEST_CASE(calibration_refuses_what_it_cannot_read) {
    Calibration cal;
    cal.set_points({{7.0e6, -20.0}});
    std::string error;

    // A typo must not silently produce a meter that is confidently wrong, and
    // must not destroy the calibration that was already there.
    CHECK(!cal.parse("14.1M", error));
    CHECK(!error.empty());
    CHECK(!cal.parse("here:-23", error));
    CHECK(!cal.parse("14.1M:loud", error));
    CHECK(!cal.parse("14.1M:-23dB", error));
    CHECK(!cal.parse("14.1M:-999", error));
    CHECK(!cal.parse("14.1M:nan", error));
    CHECK(!cal.parse("nan:-3", error));
    CHECK(!cal.parse("14.1Mjunk:-3", error));
    CHECK(cal.points().size() == 1);
    CHECK(std::fabs(cal.points()[0].offset_db - -20.0) < 1e-9);
}

TEST_CASE(calibration_an_empty_string_clears_it) {
    Calibration cal;
    cal.set_points({{7.0e6, -20.0}});
    std::string error;
    CHECK(cal.parse("   ", error));
    CHECK(cal.empty());
}

TEST_CASE(calibration_survives_a_round_trip) {
    Calibration cal;
    std::string error;
    CHECK(cal.parse("14.1M:-23.5, 28M:-29", error));
    const std::string written = cal.to_string();

    Calibration again;
    CHECK(again.parse(written, error));
    CHECK(again.points().size() == 2);
    CHECK(std::fabs(again.offset_at(21.0e6) - cal.offset_at(21.0e6)) < 0.01);
}
