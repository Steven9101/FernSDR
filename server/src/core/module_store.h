// Installed modules, and the package format they arrive in. See MODULES.md.
//
// Everything read from a package or from the modules directory is checked
// before it is used: an id or a version becomes part of a path, so neither
// is ever taken from anywhere without matching its pattern first.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../util/json.h"

namespace fernsdr {

constexpr size_t kMaxManifestBytes = 16 * 1024;
constexpr size_t kMaxModuleBytes = 32 * 1024 * 1024;

// One setting a module declares: what the admin panel draws, and how the
// text an operator wrote in the config becomes the value the module gets.
struct ModuleSetting {
    std::string key;
    std::string type;  // string, number, boolean or choice
    std::string label;
    std::string help;
    std::string unit;
    std::vector<std::string> choices;
    bool live = false;
    bool advanced = false;  // most operators should leave it alone
    bool has_min = false, has_max = false;
    double min = 0.0, max = 0.0;
    Json fallback;  // the declared default, null when there is none
};

struct ModuleManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string kind;
    std::string platform;
    std::string sha256;
    std::string license;
    std::string source;
    std::string description;
    uint64_t size = 0;
    std::vector<ModuleSetting> settings;
    // What the operator has to install first that the package cannot carry,
    // in words, such as a vendor's API and its service. Shown before a band
    // is set up with it. Optional; receivers from before it ignore it.
    std::vector<std::string> requires_;
    // For an input module, what its radio can be set to, so that the admin
    // panel can offer bands that fit instead of asking for numbers: the
    // centre frequencies it tunes (ranges in Hz), the sample rates worth
    // offering (the first the default) and whether it delivers IQ or a real
    // signal, which covers 0 Hz to half the rate. Optional; a module without
    // it is set up by hand.
    struct Tuning {
        std::vector<std::pair<double, double>> ranges;
        std::vector<double> rates;
        std::string signal;  // "iq" or "real"; empty when there is no tuning
    } tuning;

    const ModuleSetting* setting(const std::string& key) const;
    Json to_json() const;
};

// The environment every module starts with, and nothing more.
std::vector<std::string> module_environment();

bool valid_module_id(const std::string& id);
bool valid_module_version(const std::string& version);
// Orders versions numerically: 0.10.0 is newer than 0.9.3.
int compare_module_versions(const std::string& a, const std::string& b);
// The platform packages must be built for to run here, or empty when there
// are none for this machine.
const char* module_platform();

bool parse_module_manifest(const std::string& text, ModuleManifest& out, std::string& error);
// Splits a .fernmod file. `executable` is where the program starts in it.
bool parse_module_package(const std::string& package, ModuleManifest& manifest, size_t& executable,
                          std::string& error);

// What a published file name promises: <id>-<version>-<platform>.fernmod.
struct PackageName {
    std::string id;
    std::string version;
    std::string platform;
};
bool parse_package_name(const std::string& name, PackageName& out);

// The operator's text for one setting, as the JSON the module declared.
bool type_module_setting(const ModuleSetting& setting, const std::string& text, Json& out, std::string& error);
// A value the admin panel sent for a live setting, checked the same way.
bool check_module_setting(const ModuleSetting& setting, const Json& value, Json& out, std::string& error);

class ModuleStore {
public:
    // A relative directory is taken from the current one, once, here: a
    // module is started by its absolute path.
    explicit ModuleStore(std::string directory);

    const std::string& directory() const { return directory_; }

    // Installs a package. `origin` is the "owner/name" it came from, or
    // "file" for one installed from the command line. `expected`, when
    // given, is what the published file name promised.
    bool install(const std::string& package, const std::string& origin, const PackageName* expected,
                 ModuleManifest& installed, std::string& error);
    bool activate(const std::string& id, const std::string& version, std::string& error);
    bool set_enabled(const std::string& id, bool enabled, std::string& error);
    // Refused while a band runs that version, and for the active version of
    // a module bands are configured to use (`in_use`).
    bool remove(const std::string& id, const std::string& version, bool in_use, std::string& error);

    struct Module {
        std::string id;
        std::string active;
        std::string origin;
        bool enabled = true;
        std::vector<ModuleManifest> versions;  // newest first
    };
    std::vector<Module> list() const;
    bool find(const std::string& id, Module& out) const;

    // What a band needs to run a module, with a lease on that version that
    // removal respects until release().
    struct Launch {
        std::string version;
        std::string executable;
        ModuleManifest manifest;
    };
    enum class Resolve { Ok, NotInstalled, Disabled, Broken };
    Resolve acquire(const std::string& id, Launch& out, std::string& error);
    void release(const std::string& id, const std::string& version);

private:
    bool read_module(const std::string& id, Module& out) const;
    std::string path(const std::string& id) const { return directory_ + "/" + id; }

    mutable std::mutex mutex_;
    std::string directory_;
    std::map<std::pair<std::string, std::string>, int> leases_;
};

}  // namespace fernsdr
