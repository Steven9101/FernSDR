#include "calibration.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace fernsdr {

namespace {

/** Trims spaces and tabs from both ends. */
std::string trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
    return text.substr(start, end - start);
}

/**
 * Reads a frequency, accepting the suffixes the rest of the configuration
 * does, so "14.1M" here means what it means everywhere else in the file.
 */
bool parse_frequency(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;

    std::string suffix = trim(std::string(end));
    double scale = 1.0;
    if (suffix == "k" || suffix == "K") scale = 1e3;
    else if (suffix == "M" || suffix == "m") scale = 1e6;
    else if (suffix == "G" || suffix == "g") scale = 1e9;
    else if (!suffix.empty()) return false;

    out = value * scale;
    return out > 0.0;
}

}  // namespace

bool Calibration::parse(const std::string& text, std::string& error) {
    error.clear();
    std::vector<CalibrationPoint> parsed;

    const std::string body = trim(text);
    if (body.empty()) {
        points_.clear();
        return true;
    }

    size_t position = 0;
    while (position <= body.size()) {
        const size_t comma = body.find(',', position);
        const std::string item =
            trim(body.substr(position, comma == std::string::npos ? std::string::npos
                                                                  : comma - position));
        if (!item.empty()) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) {
                error = "\"" + item + "\" should be a frequency and an offset, like 14.1M:-23.5";
                return false;
            }
            CalibrationPoint point;
            if (!parse_frequency(trim(item.substr(0, colon)), point.hz)) {
                error = "\"" + item.substr(0, colon) + "\" is not a frequency";
                return false;
            }
            const std::string offset = trim(item.substr(colon + 1));
            char* end = nullptr;
            point.offset_db = std::strtod(offset.c_str(), &end);
            if (offset.empty() || end == offset.c_str() || *end != '\0') {
                error = "\"" + offset + "\" is not an offset in dB";
                return false;
            }
            // A correction of more than 120 dB is a typo, not a measurement,
            // and accepting it produces a meter that is confidently absurd.
            if (point.offset_db < -120.0 || point.offset_db > 120.0) {
                error = "an offset of " + offset + " dB is outside anything real";
                return false;
            }
            parsed.push_back(point);
        }
        if (comma == std::string::npos) break;
        position = comma + 1;
    }

    set_points(std::move(parsed));
    return true;
}

std::string Calibration::to_string() const {
    std::string out;
    for (const auto& point : points_) {
        if (!out.empty()) out += ", ";
        // Written in MHz, which is how an operator thinks about a frequency
        // and how they typed it in.
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6gM:%+.2f", point.hz / 1e6, point.offset_db);
        out += buffer;
    }
    return out;
}

}  // namespace fernsdr
