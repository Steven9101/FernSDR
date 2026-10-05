#include "module_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../util/log.h"
#include "../util/password.h"
#include "../util/utf8.h"

namespace fernsdr {

namespace {

bool is_lower_hex(const std::string& text) {
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_setting_key(const std::string& key) {
    if (key.empty() || key.size() > 32 || !(key[0] >= 'a' && key[0] <= 'z')) return false;
    for (char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    // The band's own values arrive beside the settings in `open`, never in them.
    return key != "sample_rate" && key != "center" && key != "signal";
}

std::string system_error(const std::string& what) { return what + ": " + std::strerror(errno); }

bool write_all(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t put = ::write(fd, data + written, size - written);
        if (put > 0) written += static_cast<size_t>(put);
        else if (put < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

// A new file, flushed to disk before this returns. Never replaces one.
bool write_new_file(const std::string& path, const char* data, size_t size, mode_t mode, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) {
        error = system_error("cannot create " + path);
        return false;
    }
    if (!write_all(fd, data, size) || ::fsync(fd) != 0) {
        error = system_error("cannot write " + path);
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        error = system_error("cannot write " + path);
        ::unlink(path.c_str());
        return false;
    }
    return true;
}

void sync_directory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// A small regular file, never through a link.
bool read_small_file(const std::string& path, size_t limit, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<uint64_t>(info.st_size) > limit) {
        ::close(fd);
        return false;
    }
    std::string data(static_cast<size_t>(info.st_size), '\0');
    size_t filled = 0;
    while (filled < data.size()) {
        const ssize_t got = ::read(fd, &data[filled], data.size() - filled);
        if (got > 0) filled += static_cast<size_t>(got);
        else if (got < 0 && errno == EINTR) continue;
        else break;
    }
    ::close(fd);
    if (filled != data.size()) return false;
    out.swap(data);
    return true;
}

// Rewrites a small file by writing beside it and renaming over it.
bool replace_file(const std::string& directory, const std::string& name, const std::string& text,
                  std::string& error) {
    const std::string temporary = directory + "/." + name + ".tmp";
    ::unlink(temporary.c_str());
    if (!write_new_file(temporary, text.data(), text.size(), 0644, error)) return false;
    if (::rename(temporary.c_str(), (directory + "/" + name).c_str()) != 0) {
        error = system_error("cannot replace " + directory + "/" + name);
        ::unlink(temporary.c_str());
        return false;
    }
    sync_directory(directory);
    return true;
}

bool is_directory_not_link(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool make_directory(const std::string& path, std::string& error) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        error = system_error("cannot create " + path);
        return false;
    }
    if (!is_directory_not_link(path)) {
        error = path + " is not a directory";
        return false;
    }
    return true;
}

// Removes a directory that holds only plain files, following no link: a
// link planted inside would otherwise have removal delete whatever it
// points at.
bool remove_flat_directory(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        error = system_error("cannot open " + path);
        return false;
    }
    DIR* directory = ::fdopendir(fd);
    if (!directory) {
        error = system_error("cannot read " + path);
        ::close(fd);
        return false;
    }
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    for (const std::string& name : names) {
        struct stat info {};
        if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            error = path + " holds a directory, " + name + ", which a module never writes; remove it by hand";
            ::closedir(directory);
            return false;
        }
        if (::unlinkat(fd, name.c_str(), 0) != 0) {
            error = system_error("cannot remove " + path + "/" + name);
            ::closedir(directory);
            return false;
        }
    }
    ::closedir(directory);
    if (::rmdir(path.c_str()) != 0) {
        error = system_error("cannot remove " + path);
        return false;
    }
    return true;
}

bool parse_version(const std::string& version, int parts[3]) {
    if (!valid_module_version(version)) return false;
    size_t position = 0;
    for (int i = 0; i < 3; i++) {
        const size_t end = version.find('.', position);
        parts[i] = std::atoi(version.substr(position, end - position).c_str());
        position = end == std::string::npos ? version.size() : end + 1;
    }
    return true;
}

bool parse_setting(const Json& entry, ModuleSetting& out, std::string& error) {
    if (!entry.is_object()) {
        error = "a setting is not a JSON object";
        return false;
    }
    out.key = entry["key"].string();
    if (!valid_setting_key(out.key)) {
        error = "setting key '" + printable(entry["key"].string(), 40) + "' is not lowercase letters, digits and underscores, or is reserved";
        return false;
    }
    out.type = entry["type"].string();
    if (out.type != "string" && out.type != "number" && out.type != "boolean" && out.type != "choice") {
        error = "setting '" + out.key + "' has type '" + printable(out.type, 20) + "'; expected string, number, boolean or choice";
        return false;
    }
    out.label = entry["label"].string(out.key);
    out.help = entry["help"].string();
    out.unit = entry["unit"].string();
    if (out.label.size() > 64 || out.help.size() > 400 || out.unit.size() > 16) {
        error = "setting '" + out.key + "' has a label, help or unit that is too long";
        return false;
    }
    out.live = entry["live"].boolean(false);
    out.advanced = entry["advanced"].boolean(false);
    if (entry.has("min")) {
        if (!entry["min"].is_number() || !std::isfinite(entry["min"].number())) {
            error = "setting '" + out.key + "' has a min that is not a number";
            return false;
        }
        out.has_min = true;
        out.min = entry["min"].number();
    }
    if (entry.has("max")) {
        if (!entry["max"].is_number() || !std::isfinite(entry["max"].number())) {
            error = "setting '" + out.key + "' has a max that is not a number";
            return false;
        }
        out.has_max = true;
        out.max = entry["max"].number();
    }
    if (out.has_min && out.has_max && out.min > out.max) {
        error = "setting '" + out.key + "' has min above max";
        return false;
    }
    if (out.type == "choice") {
        const Json& choices = entry["choices"];
        if (!choices.is_array() || choices.size() == 0 || choices.size() > 64) {
            error = "choice setting '" + out.key + "' needs 1 to 64 choices";
            return false;
        }
        for (const Json& choice : choices.elements()) {
            if (!choice.is_string() || choice.string().empty() || choice.string().size() > 64) {
                error = "choice setting '" + out.key + "' has a choice that is not a short string";
                return false;
            }
            out.choices.push_back(choice.string());
        }
    }
    if (entry.has("default") && !entry["default"].is_null()) {
        Json checked;
        if (!check_module_setting(out, entry["default"], checked, error)) {
            error = "the default of setting '" + out.key + "' does not fit it: " + error;
            return false;
        }
        out.fallback = checked;
    }
    return true;
}

}  // namespace

std::vector<std::string> module_environment() {
    return {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "FERNSDR_MODULE_API=1"};
}

bool valid_module_id(const std::string& id) {
    if (id.empty() || id.size() > 32 || !(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool valid_module_version(const std::string& version) {
    int dots = 0;
    size_t digits = 0;
    char first = 0;
    for (char c : version) {
        if (c == '.') {
            if (digits == 0) return false;
            dots++;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (digits == 0) first = c;
            // One spelling per number, or 1.2.3 and 01.2.3 would be two
            // directories holding the same version.
            else if (first == '0') return false;
            if (++digits > 4) return false;
        } else {
            return false;
        }
    }
    return dots == 2 && digits > 0;
}

int compare_module_versions(const std::string& a, const std::string& b) {
    int left[3] = {0, 0, 0}, right[3] = {0, 0, 0};
    parse_version(a, left);
    parse_version(b, right);
    for (int i = 0; i < 3; i++) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}

const char* module_platform() {
#if defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__aarch64__)
    return "linux-aarch64";
#elif defined(__arm__) && defined(__ARM_PCS_VFP) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
    return "linux-armhf";
#else
    return "";
#endif
}

const ModuleSetting* ModuleManifest::setting(const std::string& key) const {
    for (const ModuleSetting& entry : settings) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

Json ModuleManifest::to_json() const {
    Json out = Json::make_object();
    out.set("id", id);
    out.set("name", name);
    out.set("version", version);
    out.set("kind", kind);
    out.set("platform", platform);
    out.set("size", static_cast<double>(size));
    out.set("sha256", sha256);
    out.set("license", license);
    out.set("source", source);
    out.set("description", description);
    Json list = Json::make_array();
    for (const ModuleSetting& setting : settings) {
        Json entry = Json::make_object();
        entry.set("key", setting.key);
        entry.set("type", setting.type);
        entry.set("label", setting.label);
        if (!setting.help.empty()) entry.set("help", setting.help);
        if (!setting.unit.empty()) entry.set("unit", setting.unit);
        if (setting.live) entry.set("live", true);
        if (setting.advanced) entry.set("advanced", true);
        if (setting.has_min) entry.set("min", setting.min);
        if (setting.has_max) entry.set("max", setting.max);
        if (!setting.choices.empty()) {
            Json choices = Json::make_array();
            for (const std::string& choice : setting.choices) choices.push_back(choice);
            entry.set("choices", choices);
        }
        if (!setting.fallback.is_null()) entry.set("default", setting.fallback);
        list.push_back(entry);
    }
    out.set("settings", list);
    if (!modes.empty()) {
        Json list = Json::make_array();
        for (const std::string& mode : modes) list.push_back(mode);
        out.set("modes", list);
    }
    if (!requires_.empty()) {
        Json needs = Json::make_array();
        for (const std::string& need : requires_) needs.push_back(need);
        out.set("requires", needs);
    }
    if (!tuning.signal.empty()) out.set("tuning", module_tuning_json(tuning));
    return out;
}

bool parse_module_tuning(const Json& tuning, ModuleManifest::Tuning& out) {
    const auto whole_hz = [](const Json& value, double most) {
        return value.is_number() && value.number() >= 0 && value.number() <= most &&
               std::floor(value.number()) == value.number();
    };
    const Json& ranges = tuning["ranges"];
    const Json& rates = tuning["rates"];
    const std::string signal = tuning["signal"].string();
    ModuleManifest::Tuning parsed;
    bool good = tuning.is_object() && ranges.is_array() && ranges.size() >= 1 && ranges.size() <= 8 &&
                rates.is_array() && rates.size() >= 1 && rates.size() <= 8 && (signal == "iq" || signal == "real");
    for (size_t i = 0; good && i < ranges.size(); i++) {
        const Json& range = ranges[i];
        good = range.is_array() && range.size() == 2 && whole_hz(range[0], 1e11) && whole_hz(range[1], 1e11) &&
               range[0].number() < range[1].number();
        if (good) parsed.ranges.emplace_back(range[0].number(), range[1].number());
    }
    for (size_t i = 0; good && i < rates.size(); i++) {
        good = whole_hz(rates[i], 1e10) && rates[i].number() >= 1000;
        if (good) parsed.rates.push_back(rates[i].number());
    }
    if (!good) return false;
    parsed.signal = signal;
    out = std::move(parsed);
    return true;
}

Json module_tuning_json(const ModuleManifest::Tuning& tuning) {
    Json entry = Json::make_object();
    Json ranges = Json::make_array();
    for (const auto& [low, high] : tuning.ranges) {
        Json range = Json::make_array();
        range.push_back(low);
        range.push_back(high);
        ranges.push_back(range);
    }
    entry.set("ranges", ranges);
    Json rates = Json::make_array();
    for (const double rate : tuning.rates) rates.push_back(rate);
    entry.set("rates", rates);
    entry.set("signal", tuning.signal);
    return entry;
}

bool parse_module_manifest(const std::string& text, ModuleManifest& out, std::string& error) {
    out = ModuleManifest{};
    if (text.size() > kMaxManifestBytes) {
        error = "the manifest is larger than 16 KiB";
        return false;
    }
    Json json;
    if (!Json::parse(text, json) || !json.is_object()) {
        error = "the manifest is not a JSON object";
        return false;
    }
    if (!json["schema"].is_number() || json["schema"].number() != 1) {
        error = "the manifest's schema is not 1";
        return false;
    }
    out.id = json["id"].string();
    if (!valid_module_id(out.id)) {
        error = "the manifest's id '" + printable(out.id, 40) + "' is not 1 to 32 lowercase letters, digits and dashes";
        return false;
    }
    out.name = json["name"].string();
    if (out.name.empty() || out.name.size() > 64) {
        error = "the manifest's name is empty or longer than 64 characters";
        return false;
    }
    out.version = json["version"].string();
    if (!valid_module_version(out.version)) {
        error = "the manifest's version '" + printable(out.version, 20) + "' is not major.minor.patch";
        return false;
    }
    out.kind = json["kind"].string();
    if (out.kind != "input" && out.kind != "decoder") {
        error = "the manifest's kind '" + printable(out.kind, 20) + "' is not 'input' or 'decoder'";
        return false;
    }
    // Input modules speak API 1, decoders API 2; see docs/MODULES.md.
    const int api = out.kind == "decoder" ? 2 : 1;
    if (!json["api"].is_number() || json["api"].number() != api) {
        error = out.id + " " + out.version + " needs module API " + printable(json["api"].serialize(), 16) +
                "; this receiver speaks API " + std::to_string(api) + " for " + out.kind + " modules";
        return false;
    }
    out.platform = json["platform"].string();
    if (out.platform != "linux-x86_64" && out.platform != "linux-aarch64" && out.platform != "linux-armhf") {
        error = "the manifest's platform '" + printable(out.platform, 30) + "' is not linux-x86_64, linux-aarch64 or linux-armhf";
        return false;
    }
    const double size = json["size"].number(-1);
    if (!json["size"].is_number() || size < 1 || size > static_cast<double>(kMaxModuleBytes) ||
        std::floor(size) != size) {
        error = "the manifest's size is not a whole number of bytes up to 32 MiB";
        return false;
    }
    out.size = static_cast<uint64_t>(size);
    out.sha256 = json["sha256"].string();
    if (out.sha256.size() != 64 || !is_lower_hex(out.sha256)) {
        error = "the manifest's sha256 is not 64 lowercase hex digits";
        return false;
    }
    out.license = json["license"].string();
    out.source = json["source"].string();
    out.description = json["description"].string();
    if (out.license.size() > 100 || out.source.size() > 300 || out.description.size() > 300) {
        error = "the manifest's license, source or description is too long";
        return false;
    }
    if (json.has("requires")) {
        const Json& needs = json["requires"];
        if (!needs.is_array() || needs.size() > 4) {
            error = "the manifest's requires is not a list of up to 4 sentences";
            return false;
        }
        for (const Json& need : needs.elements()) {
            const std::string text = need.string();
            const bool plain = need.is_string() && !text.empty() && text.size() <= 200 && valid_utf8(text) &&
                               std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
            if (!plain) {
                error = "the manifest's requires holds something other than a line of up to 200 characters";
                return false;
            }
            out.requires_.push_back(text);
        }
    }
    if (json.has("modes")) {
        // Names as a decoder section's mode takes them: up to 8 lowercase
        // letters and digits.
        const Json& modes = json["modes"];
        if (out.kind != "decoder" || !modes.is_array() || modes.size() == 0 || modes.size() > 16) {
            error = "the manifest's modes is not a list of 1 to 16 modes of a decoder module";
            return false;
        }
        for (const Json& mode : modes.elements()) {
            const std::string name = mode.string();
            const bool plain = mode.is_string() && !name.empty() && name.size() <= 8 &&
                               std::all_of(name.begin(), name.end(),
                                           [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
            if (!plain || std::find(out.modes.begin(), out.modes.end(), name) != out.modes.end()) {
                error = "the manifest's modes holds something other than distinct short lowercase names";
                return false;
            }
            out.modes.push_back(name);
        }
    }
    if (json.has("tuning") && !parse_module_tuning(json["tuning"], out.tuning)) {
        error = "the manifest's tuning is not up to 8 frequency ranges and 8 sample rates in whole Hz with a signal "
                "of iq or real";
        return false;
    }
    const Json& settings = json["settings"];
    if (!settings.is_array()) {
        error = "the manifest has no settings list";
        return false;
    }
    if (settings.size() > 64) {
        error = "the manifest declares more than 64 settings";
        return false;
    }
    for (const Json& entry : settings.elements()) {
        ModuleSetting setting;
        if (!parse_setting(entry, setting, error)) return false;
        if (out.setting(setting.key)) {
            error = "the manifest declares setting '" + setting.key + "' twice";
            return false;
        }
        out.settings.push_back(std::move(setting));
    }
    return true;
}

bool parse_module_package(const std::string& package, ModuleManifest& manifest, size_t& executable,
                          std::string& error) {
    static const char kMagic[] = "FERNMOD1\n";
    const size_t magic = sizeof(kMagic) - 1;
    if (package.size() < magic || package.compare(0, magic, kMagic) != 0) {
        error = "this is not a .fernmod package";
        return false;
    }
    size_t position = magic;
    size_t length = 0;
    size_t digits = 0;
    while (position < package.size() && package[position] != '\n') {
        const char c = package[position];
        if (c < '0' || c > '9' || ++digits > 5) {
            error = "the package header does not give the manifest's length";
            return false;
        }
        length = length * 10 + static_cast<size_t>(c - '0');
        position++;
    }
    if (position >= package.size() || digits == 0) {
        error = "the package ends inside its header";
        return false;
    }
    position++;
    if (length > kMaxManifestBytes) {
        error = "the package's manifest is larger than 16 KiB";
        return false;
    }
    if (package.size() - position < length) {
        error = "the package ends inside its manifest";
        return false;
    }
    if (!parse_module_manifest(package.substr(position, length), manifest, error)) return false;
    executable = position + length;
    const size_t program = package.size() - executable;
    if (program != manifest.size) {
        error = "the package holds " + std::to_string(program) + " bytes of program, and its manifest says " +
                std::to_string(manifest.size) + "; it is damaged or incomplete";
        return false;
    }
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(package.data()) + executable, program);
    uint8_t digest[32];
    hash.finish(digest);
    if (to_hex(digest, sizeof(digest)) != manifest.sha256) {
        error = "the program's SHA-256 does not match its manifest; the package is damaged";
        return false;
    }
    return true;
}

bool parse_package_name(const std::string& name, PackageName& out) {
    static const char kSuffix[] = ".fernmod";
    const size_t suffix = sizeof(kSuffix) - 1;
    if (name.size() <= suffix || name.compare(name.size() - suffix, suffix, kSuffix) != 0) return false;
    const std::string stem = name.substr(0, name.size() - suffix);
    for (const char* platform : {"linux-x86_64", "linux-aarch64", "linux-armhf"}) {
        const std::string tail = std::string("-") + platform;
        if (stem.size() <= tail.size() || stem.compare(stem.size() - tail.size(), tail.size(), tail) != 0) continue;
        const std::string rest = stem.substr(0, stem.size() - tail.size());
        const size_t dash = rest.rfind('-');
        if (dash == std::string::npos) return false;
        out.id = rest.substr(0, dash);
        out.version = rest.substr(dash + 1);
        out.platform = platform;
        return valid_module_id(out.id) && valid_module_version(out.version);
    }
    return false;
}

bool type_module_setting(const ModuleSetting& setting, const std::string& text, Json& out, std::string& error) {
    if (setting.type == "string") {
        if (text.size() > 1024) {
            error = "'" + setting.key + "' is longer than 1024 characters";
            return false;
        }
        out = Json(text);
        return true;
    }
    if (setting.type == "number") {
        const char* start = text.c_str();
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(start, &end);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (text.empty() || end == start || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
            error = "'" + setting.key + "' must be a number, not '" + text + "'";
            return false;
        }
        return check_module_setting(setting, Json(value), out, error);
    }
    if (setting.type == "boolean") {
        std::string lower = text;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "yes" || lower == "true" || lower == "on" || lower == "1") out = Json(true);
        else if (lower == "no" || lower == "false" || lower == "off" || lower == "0") out = Json(false);
        else {
            error = "'" + setting.key + "' must be yes or no, not '" + text + "'";
            return false;
        }
        return true;
    }
    return check_module_setting(setting, Json(text), out, error);
}

bool check_module_setting(const ModuleSetting& setting, const Json& value, Json& out, std::string& error) {
    if (setting.type == "string") {
        if (!value.is_string() || value.string().size() > 1024) {
            error = "'" + setting.key + "' must be text of at most 1024 characters";
            return false;
        }
    } else if (setting.type == "number") {
        if (!value.is_number() || !std::isfinite(value.number())) {
            error = "'" + setting.key + "' must be a number";
            return false;
        }
        char bounds[96];
        if ((setting.has_min && value.number() < setting.min) || (setting.has_max && value.number() > setting.max)) {
            std::snprintf(bounds, sizeof(bounds), "%g to %g", setting.has_min ? setting.min : -INFINITY,
                          setting.has_max ? setting.max : INFINITY);
            error = "'" + setting.key + "' must be from " + bounds;
            return false;
        }
    } else if (setting.type == "boolean") {
        if (!value.is_bool()) {
            error = "'" + setting.key + "' must be yes or no";
            return false;
        }
    } else {
        if (!value.is_string() ||
            std::find(setting.choices.begin(), setting.choices.end(), value.string()) == setting.choices.end()) {
            std::string offered;
            for (const std::string& choice : setting.choices) offered += (offered.empty() ? "" : ", ") + choice;
            error = "'" + setting.key + "' must be one of " + offered;
            return false;
        }
    }
    out = value;
    return true;
}

ModuleStore::ModuleStore(std::string directory) : directory_(std::move(directory)) {
    if (!directory_.empty() && directory_[0] != '/') {
        char here[4096];
        if (::getcwd(here, sizeof(here))) directory_ = std::string(here) + "/" + directory_;
    }
    while (directory_.size() > 1 && directory_.back() == '/') directory_.pop_back();
}

bool ModuleStore::read_module(const std::string& id, Module& out) const {
    out = Module{};
    out.id = id;
    const std::string base = path(id);
    if (!valid_module_id(id) || !is_directory_not_link(base)) return false;
    std::string text;
    if (read_small_file(base + "/active", 64, text)) {
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        if (valid_module_version(text)) out.active = text;
    }
    if (read_small_file(base + "/origin", 256, text)) {
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        out.origin = text;
    }
    struct stat info {};
    out.enabled = ::lstat((base + "/disabled").c_str(), &info) != 0;
    DIR* directory = ::opendir(base.c_str());
    if (!directory) return false;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string version = entry->d_name;
        if (!valid_module_version(version) || !is_directory_not_link(base + "/" + version)) continue;
        std::string manifest_text;
        ModuleManifest manifest;
        std::string problem;
        if (!read_small_file(base + "/" + version + "/manifest.json", kMaxManifestBytes, manifest_text) ||
            !parse_module_manifest(manifest_text, manifest, problem) || manifest.id != id ||
            manifest.version != version) {
            LOG_WARN("modules", "%s/%s is not a usable module version, skipped: %s", base.c_str(), version.c_str(),
                     problem.empty() ? "unreadable manifest" : problem.c_str());
            continue;
        }
        out.versions.push_back(std::move(manifest));
    }
    ::closedir(directory);
    std::sort(out.versions.begin(), out.versions.end(), [](const ModuleManifest& a, const ModuleManifest& b) {
        return compare_module_versions(a.version, b.version) > 0;
    });
    return true;
}

std::vector<ModuleStore::Module> ModuleStore::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Module> out;
    DIR* directory = ::opendir(directory_.c_str());
    if (!directory) return out;
    std::vector<std::string> ids;
    while (const dirent* entry = ::readdir(directory)) {
        if (valid_module_id(entry->d_name)) ids.push_back(entry->d_name);
    }
    ::closedir(directory);
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        Module module;
        if (read_module(id, module) && !module.versions.empty()) out.push_back(std::move(module));
    }
    return out;
}

bool ModuleStore::find(const std::string& id, Module& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return read_module(id, out) && !out.versions.empty();
}

bool ModuleStore::install(const std::string& package, const std::string& origin, const PackageName* expected,
                          ModuleManifest& installed, std::string& error) {
    size_t executable = 0;
    if (!parse_module_package(package, installed, executable, error)) return false;
    if (installed.platform != module_platform()) {
        error = installed.id + " " + installed.version + " is built for " + installed.platform +
                " and this receiver runs on " + (*module_platform() ? module_platform() : "a platform with no packages");
        return false;
    }
    if (expected && (expected->id != installed.id || expected->version != installed.version ||
                     expected->platform != installed.platform)) {
        error = "the package was published as " + expected->id + " " + expected->version + " for " +
                expected->platform + " but its manifest says " + installed.id + " " + installed.version + " for " +
                installed.platform + "; refusing it";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!make_directory(directory_, error)) return false;
    const std::string base = path(installed.id);
    if (!make_directory(base, error)) return false;

    std::string previous_origin;
    if (read_small_file(base + "/origin", 256, previous_origin)) {
        while (!previous_origin.empty() && previous_origin.back() == '\n') previous_origin.pop_back();
    }
    Module existing;
    const bool known = read_module(installed.id, existing) && !existing.versions.empty();
    // One catalog repository may not take over a module another one
    // published. A package installed from a file by an operator with a shell
    // is theirs to vouch for either way.
    if (known && !previous_origin.empty() && previous_origin != origin && origin != "file" &&
        previous_origin != "file") {
        error = installed.id + " was installed from " + previous_origin + "; a package from " + origin +
                " cannot replace it. Remove every version of " + installed.id + " first.";
        return false;
    }
    for (const ModuleManifest& version : existing.versions) {
        if (version.version != installed.version) continue;
        if (version.sha256 == installed.sha256) return true;  // already here, byte for byte
        error = installed.id + " " + installed.version +
                " is already installed with different contents; a published version never changes";
        return false;
    }

    // A crash during an earlier install can leave a staging directory; it
    // was never renamed into place, so nothing refers to it. Only an old one:
    // the command line and the running receiver may be installing at once,
    // and a fresh one may be the other's work in progress.
    if (DIR* directory = ::opendir(base.c_str())) {
        std::vector<std::string> stale;
        const time_t now = ::time(nullptr);
        while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            struct stat info {};
            if (name.rfind(".staging-", 0) == 0 && ::lstat((base + "/" + name).c_str(), &info) == 0 &&
                now - info.st_mtime > 3600) {
                stale.push_back(name);
            }
        }
        ::closedir(directory);
        for (const std::string& name : stale) {
            std::string ignored;
            remove_flat_directory(base + "/" + name, ignored);
        }
    }

    const std::string staging = base + "/.staging-" + random_hex(8);
    if (::mkdir(staging.c_str(), 0755) != 0) {
        error = system_error("cannot create " + staging);
        return false;
    }
    // The manifest is written exactly as it arrived, between the header line
    // and the program.
    const size_t manifest_start = package.find('\n', 9) + 1;
    const std::string manifest_text = package.substr(manifest_start, executable - manifest_start);
    if (!write_new_file(staging + "/manifest.json", manifest_text.data(), manifest_text.size(), 0644, error) ||
        !write_new_file(staging + "/module", package.data() + executable, installed.size, 0755, error)) {
        std::string ignored;
        remove_flat_directory(staging, ignored);
        return false;
    }
    sync_directory(staging);
    const std::string target = base + "/" + installed.version;
    if (::rename(staging.c_str(), target.c_str()) != 0) {
        error = system_error("cannot move " + installed.id + " " + installed.version + " into place");
        std::string ignored;
        remove_flat_directory(staging, ignored);
        return false;
    }
    sync_directory(base);
    const bool record_origin = previous_origin.empty() || (previous_origin == "file" && origin != "file");
    if (record_origin && !replace_file(base, "origin", origin + "\n", error)) return false;
    // The first version of a module is the one bands use; later ones wait
    // for the operator to activate them.
    if (existing.active.empty() && !replace_file(base, "active", installed.version + "\n", error)) return false;
    LOG_INFO("modules", "installed %s %s from %s", installed.id.c_str(), installed.version.c_str(), origin.c_str());
    return true;
}

bool ModuleStore::activate(const std::string& id, const std::string& version, std::string& error) {
    if (!valid_module_id(id) || !valid_module_version(version)) {
        error = "no such module version";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Module module;
    if (!read_module(id, module)) {
        error = id + " is not installed";
        return false;
    }
    const bool present = std::any_of(module.versions.begin(), module.versions.end(),
                                     [&](const ModuleManifest& m) { return m.version == version; });
    if (!present) {
        error = id + " " + version + " is not installed";
        return false;
    }
    if (!replace_file(path(id), "active", version + "\n", error)) return false;
    LOG_INFO("modules", "%s: version %s is now active", id.c_str(), version.c_str());
    return true;
}

bool ModuleStore::set_enabled(const std::string& id, bool enabled, std::string& error) {
    if (!valid_module_id(id)) {
        error = "no such module";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Module module;
    if (!read_module(id, module) || module.versions.empty()) {
        error = id + " is not installed";
        return false;
    }
    const std::string marker = path(id) + "/disabled";
    if (enabled) {
        if (::unlink(marker.c_str()) != 0 && errno != ENOENT) {
            error = system_error("cannot remove " + marker);
            return false;
        }
    } else {
        const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            error = system_error("cannot create " + marker);
            return false;
        }
        ::close(fd);
    }
    sync_directory(path(id));
    LOG_INFO("modules", "%s switched %s", id.c_str(), enabled ? "on" : "off");
    return true;
}

bool ModuleStore::remove(const std::string& id, const std::string& version, bool in_use, std::string& error) {
    if (!valid_module_id(id) || !valid_module_version(version)) {
        error = "no such module version";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Module module;
    if (!read_module(id, module)) {
        error = id + " is not installed";
        return false;
    }
    const bool present = std::any_of(module.versions.begin(), module.versions.end(),
                                     [&](const ModuleManifest& m) { return m.version == version; });
    if (!present) {
        error = id + " " + version + " is not installed";
        return false;
    }
    const auto lease = leases_.find({id, version});
    if (lease != leases_.end() && lease->second > 0) {
        error = "a band is running " + id + " " + version + "; stop it or activate another version first";
        return false;
    }
    if (version == module.active && in_use && module.versions.size() > 1) {
        error = id + " " + version + " is the version bands use; activate another one first";
        return false;
    }
    if (version == module.active && in_use) {
        error = "bands are configured to use " + id + "; change them before removing its last version";
        return false;
    }
    if (!remove_flat_directory(path(id) + "/" + version, error)) return false;
    if (module.versions.size() == 1) {
        // The last version: the module goes, and with it which repository it
        // came from, so it may be installed from another one afterwards.
        for (const char* name : {"/active", "/origin", "/disabled", "/.active.tmp", "/.origin.tmp"}) {
            ::unlink((path(id) + name).c_str());
        }
        std::string ignored;
        remove_flat_directory(path(id), ignored);
    } else if (version == module.active) {
        // Nothing uses it; the newest remaining version takes its place.
        for (const ModuleManifest& other : module.versions) {
            if (other.version == version) continue;
            if (!replace_file(path(id), "active", other.version + "\n", error)) return false;
            break;
        }
    }
    sync_directory(directory_);
    LOG_INFO("modules", "removed %s %s", id.c_str(), version.c_str());
    return true;
}

ModuleStore::Resolve ModuleStore::acquire(const std::string& id, Launch& out, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Module module;
    if (!valid_module_id(id) || !read_module(id, module) || module.versions.empty()) {
        error = "module " + id + " is not installed; install it in the admin panel under Modules, or with "
                "fernsdr --install-module";
        return Resolve::NotInstalled;
    }
    if (!module.enabled) {
        error = "module " + id + " is switched off in the admin panel";
        return Resolve::Disabled;
    }
    const auto chosen = std::find_if(module.versions.begin(), module.versions.end(),
                                     [&](const ModuleManifest& m) { return m.version == module.active; });
    if (chosen == module.versions.end()) {
        error = "module " + id + " has no usable active version; activate one in the admin panel";
        return Resolve::Broken;
    }
    const std::string executable = path(id) + "/" + chosen->version + "/module";
    struct stat info {};
    if (::lstat(executable.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        static_cast<uint64_t>(info.st_size) != chosen->size) {
        error = executable + " is missing or not the size its manifest records; reinstall " + id + " " +
                chosen->version;
        return Resolve::Broken;
    }
    if (chosen->platform != module_platform()) {
        error = id + " " + chosen->version + " is built for " + chosen->platform;
        return Resolve::Broken;
    }
    out.version = chosen->version;
    out.executable = executable;
    out.manifest = *chosen;
    leases_[{id, chosen->version}]++;
    return Resolve::Ok;
}

void ModuleStore::release(const std::string& id, const std::string& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find({id, version});
    if (found == leases_.end()) return;
    if (--found->second <= 0) leases_.erase(found);
}

}  // namespace fernsdr
