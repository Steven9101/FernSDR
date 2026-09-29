// Configuration file reader.
//
// A small INI dialect: [section] headers, key = value lines, '#' or ';'
// comments.  Chosen over JSON or YAML because the people who run these
// receivers edit the file over SSH on a machine in a shed, and a config that
// a stray comma cannot turn into a parse error is worth more than expressive
// syntax.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fernsdr {

class ConfigSection {
public:
    ConfigSection() = default;
    explicit ConfigSection(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    bool has(const std::string& key) const { return values_.count(key) > 0; }

    std::string get(const std::string& key, const std::string& fallback = "") const;
    double get_double(const std::string& key, double fallback) const;
    long get_int(const std::string& key, long fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    void set(const std::string& key, const std::string& value) { values_[key] = value; }
    const std::map<std::string, std::string>& values() const { return values_; }

private:
    std::string name_;
    std::map<std::string, std::string> values_;
};

class Config {
public:
    // Returns false and fills `error` on a syntax problem, naming the line.
    bool load(const std::string& path, std::string& error);
    bool parse(const std::string& text, std::string& error);

    // Where load() read this from, empty for a config built from a string.
    const std::string& path() const { return path_; }

    // Missing sections return an empty section rather than failing, so every
    // setting can have a default.
    const ConfigSection& section(const std::string& name) const;
    bool has_section(const std::string& name) const;

    // All sections named "<prefix>:<something>", in file order.  Used for
    // repeated [band:40m] style sections.
    std::vector<const ConfigSection*> sections_with_prefix(const std::string& prefix) const;

    const std::vector<ConfigSection>& sections() const { return sections_; }

private:
    std::vector<ConfigSection> sections_;
    ConfigSection empty_;
    std::string path_;
};

/** Who may read a file the receiver writes. */
enum class FileAccess {
    // As the process umask allows, like any other new file.
    Default,
    // The owner alone (0600): the config holds the admin password hash, and
    // the settings hold the addresses of muted listeners.
    OwnerOnly,
};

// Whole-file text, for the admin panel's config editor. Both report why they
// failed rather than returning a bare false: "cannot save" with no reason is
// the least helpful message an operator can be given.
bool read_text_file(const std::string& path, std::string& out);
bool write_text_file(const std::string& path, const std::string& text, std::string& error,
                     FileAccess access = FileAccess::Default);

// Trims ASCII whitespace from both ends.
std::string trim(const std::string& s);

// Parses "7.1M", "14074k", "3690000" into Hz.  Suffixes are a real
// convenience when the alternative is counting zeros in a band plan.
bool parse_frequency(const std::string& text, double& hz);

// Splits "a, b c" into {"a","b","c"}; commas and spaces both separate, because
// people write config lists both ways.
std::vector<std::string> split_list(const std::string& text);

}  // namespace fernsdr
