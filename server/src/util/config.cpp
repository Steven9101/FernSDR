#include "config.h"

#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

namespace fernsdr {

namespace {

// A line without its comment, '#' or ';' to the end, but not inside a quoted
// value. Config::parse and the section helpers below read lines through the
// same function, so a line is a header, a comment or a key for all of them
// alike: the helpers decide which sections leave a machine in a backup.
std::string uncommented(const std::string& line) {
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"') in_quotes = !in_quotes;
        if (!in_quotes && (line[i] == '#' || line[i] == ';')) return line.substr(0, i);
    }
    return line;
}

// Each line of `text`, its newline included, with the section it is in and
// the line as Config::parse reads it (uncommented and trimmed); the part
// before the first header is in the section "".
template <typename Each>
void for_each_line_in_section(const std::string& text, Each each) {
    std::string section;
    size_t start = 0;
    while (start < text.size()) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string::npos ? text.size() : newline + 1;
        const std::string line = text.substr(start, end - start);
        const std::string bare = trim(uncommented(line.substr(0, line.size() - (newline == std::string::npos ? 0 : 1))));
        if (!bare.empty() && bare.front() == '[' && bare.back() == ']') section = trim(bare.substr(1, bare.size() - 2));
        each(section, line, bare);
        start = end;
    }
}

bool named(const std::vector<std::string>& names, const std::string& section) {
    return std::find(names.begin(), names.end(), section) != names.end();
}

}  // namespace

std::string sections_of(const std::string& text, const std::vector<std::string>& names) {
    std::string out;
    for_each_line_in_section(text, [&](const std::string& section, const std::string& line, const std::string&) {
        if (named(names, section)) out += line;
    });
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
}

std::string without_sections(const std::string& text, const std::vector<std::string>& names) {
    std::string out;
    for_each_line_in_section(text, [&](const std::string& section, const std::string& line, const std::string&) {
        if (!named(names, section)) out += line;
    });
    return out;
}

std::string without_keys(const std::string& text,
                         const std::function<bool(const std::string& section, const std::string& key)>& drop) {
    std::string out;
    for_each_line_in_section(text, [&](const std::string& section, const std::string& line, const std::string& bare) {
        const size_t equals = bare.find('=');
        if (!bare.empty() && bare.front() != '[' && equals != std::string::npos &&
            drop(section, trim(bare.substr(0, equals)))) {
            return;
        }
        out += line;
    });
    return out;
}

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) begin++;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(begin, end - begin);
}

std::string ConfigSection::get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

double ConfigSection::get_double(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    double hz;
    if (parse_frequency(it->second, hz)) return hz;
    return fallback;
}

long ConfigSection::get_int(const std::string& key, long fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    double value;
    if (!parse_frequency(it->second, value)) return fallback;
    // A value no long holds is as unusable as one that does not parse, and
    // casting it anyway is undefined. The long's own bounds, 32 bits on
    // armhf: its lowest value is a power of two, exact as a double, and so is
    // that negated, one past its highest.
    constexpr double lowest = static_cast<double>(std::numeric_limits<long>::min());
    if (!(value >= lowest && value < -lowest)) return fallback;
    return static_cast<long>(value);
}

bool ConfigSection::get_bool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    std::string v = trim(it->second);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

bool Config::load(const std::string& path, std::string& error) {
    std::string text;
    if (!read_text_file(path, text)) {
        error = "cannot open " + path;
        return false;
    }
    if (!parse(text, error)) return false;
    path_ = path;
    return true;
}

bool Config::parse(const std::string& text, std::string& error) {
    sections_.clear();
    sections_.emplace_back("");  // settings before any header

    std::istringstream stream(text);
    std::string line;
    int line_number = 0;

    while (std::getline(stream, line)) {
        line_number++;
        const std::string trimmed = trim(uncommented(line));
        if (trimmed.empty()) continue;

        if (trimmed.front() == '[') {
            if (trimmed.back() != ']') {
                error = "line " + std::to_string(line_number) + ": section header missing ']'";
                return false;
            }
            sections_.emplace_back(trim(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }

        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(line_number) + ": expected 'key = value'";
            return false;
        }

        const std::string key = trim(trimmed.substr(0, equals));
        std::string value = trim(trimmed.substr(equals + 1));
        if (key.empty()) {
            error = "line " + std::to_string(line_number) + ": empty key";
            return false;
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        sections_.back().set(key, value);
    }
    return true;
}

const ConfigSection& Config::section(const std::string& name) const {
    for (const auto& s : sections_) {
        if (s.name() == name) return s;
    }
    return empty_;
}

bool Config::has_section(const std::string& name) const {
    for (const auto& s : sections_) {
        if (s.name() == name) return true;
    }
    return false;
}

std::vector<const ConfigSection*> Config::sections_with_prefix(const std::string& prefix) const {
    std::vector<const ConfigSection*> out;
    const std::string full = prefix + ":";
    for (const auto& s : sections_) {
        if (s.name().compare(0, full.size(), full) == 0) out.push_back(&s);
    }
    return out;
}

bool parse_frequency(const std::string& text, double& hz) {
    const std::string t = trim(text);
    if (t.empty()) return false;

    char* end = nullptr;
    const double value = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;

    // Accept "7.1M", "7.1MHz", "14074k", "14074 kHz", "500Hz", and nothing
    // after the unit: "7.1MHzjunk" is a typo, not a frequency.
    std::string suffix = trim(std::string(end));
    for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    double multiplier = 1.0;
    if (!suffix.empty() && suffix != "hz") {
        switch (suffix[0]) {
            case 'k': multiplier = 1e3; break;
            case 'm': multiplier = 1e6; break;
            case 'g': multiplier = 1e9; break;
            default: return false;
        }
        if (suffix.size() > 1 && suffix.compare(1, std::string::npos, "hz") != 0) return false;
    }
    // "nan" and "inf" read as numbers, and so does 1e400 as infinity; no
    // setting means either, and a NaN passes every range check after it.
    if (!std::isfinite(value * multiplier)) return false;
    hz = value * multiplier;
    return true;
}

// Plain descriptors rather than streams, because a stream cannot be opened
// close-on-exec and a module started meanwhile would inherit the file.
bool read_text_file(const std::string& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string data;
    char buffer[65536];
    while (true) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got > 0) {
            data.append(buffer, static_cast<size_t>(got));
        } else if (got == 0) {
            break;
        } else if (errno != EINTR) {
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    out.swap(data);
    return true;
}

bool write_text_file(const std::string& path, const std::string& text, std::string& error,
                     FileAccess access) {
    // Write beside the target and rename over it. A half-written config after
    // a full disk or a crash mid-save would leave the receiver unable to start,
    // and rename(2) is atomic on the same filesystem. The data is flushed to
    // disk first, or a power cut could leave the renamed file empty.
    const std::string temporary = path + ".tmp";
    const bool owner_only = access == FileAccess::OwnerOnly;
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                          owner_only ? 0600 : 0644);
    if (fd < 0) {
        error = "cannot write " + temporary + ": " + std::strerror(errno);
        return false;
    }
    // A temporary left behind by an interrupted save keeps the mode it was
    // made with, so the mode is set here, before anything is written into it.
    if (owner_only && ::fchmod(fd, 0600) != 0) {
        error = "cannot restrict " + temporary + ": " + std::strerror(errno);
        ::close(fd);
        std::remove(temporary.c_str());
        return false;
    }
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t put = ::write(fd, text.data() + written, text.size() - written);
        if (put > 0) {
            written += static_cast<size_t>(put);
        } else if (put < 0 && errno == EINTR) {
            continue;
        } else {
            error = "write to " + temporary + " failed: " + std::strerror(errno);
            ::close(fd);
            std::remove(temporary.c_str());
            return false;
        }
    }
    // Closed whether or not the sync worked, and the sync's reason is the
    // one given: every failed save would otherwise keep a descriptor open.
    const bool synced = ::fsync(fd) == 0;
    const int sync_errno = errno;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed) {
        error = "write to " + temporary + " failed: " + std::strerror(synced ? errno : sync_errno);
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path;
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

}  // namespace fernsdr
