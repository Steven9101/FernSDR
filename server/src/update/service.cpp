#include "service.h"

#include <unistd.h>

#include <chrono>
#include <cstdlib>

#include "../version.h"
#include "files.h"
#include "release_keys.h"
#include "system.h"
#include "updater.h"

namespace fernsdr {

namespace {

int64_t wall_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// States in which the updater is at work and a second request would only be
// refused by it.
bool busy_state(const std::string& state) {
    return state == "checking" || state == "downloading" || state == "installing" || state == "trial";
}

// Where the panel cannot switch it, how it is switched by hand.
std::string switched_by_hand(const std::string& init) {
    const std::string start = "The admin panel switches this on a receiver installed by install.sh. Here: ";
    if (init == "systemd") return start + "sudo systemctl enable fernsdr, or disable.";
    if (init == "openrc") return start + "sudo rc-update add fernsdr default, or del.";
    if (init == "runit") return start + "a file named down in the service's directory keeps it from starting.";
    return start + "sudo update-rc.d fernsdr enable, or disable.";
}

}  // namespace

UpdateServiceOptions UpdateService::system(const std::string& state_directory) {
    const UpdateLayout layout = UpdateLayout::standard();
    UpdateServiceOptions options;
    options.state = state_directory;
    const char* update = std::getenv("FERNSDR_UPDATE_DIR");
    options.update = update && *update ? update : layout.update;
    options.install = layout.install;
    char path[4096];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) options.executable.assign(path, static_cast<size_t>(n));
    options.running_version = kVersion;
    options.platform = release_platform();
    options.keys = release_keys();
    std::string error;
    // A wrong FERNSDR_UPDATE_URL leaves the default in place; the updater,
    // which reads it too, refuses to run with it and says so.
    if (!release_base_url(options.base_url, error)) options.base_url = kReleaseBaseUrl;
    options.fetch = fetch_release_file;
    options.autostart = [] { return read_autostart(); };
    return options;
}

UpdateService::UpdateService(UpdateServiceOptions options) : options_(std::move(options)) {
    if (!options_.autostart) options_.autostart = [] { return read_autostart(); };
    const std::string releases = options_.install + "/releases/";
    const int update = open_directory(options_.update);
    if (options_.keys.empty()) {
        unavailable_ = "This build carries no release key, so it cannot check a release: update it the way it was "
                       "built.";
    } else if (options_.platform.empty()) {
        unavailable_ = "No releases are built for this kind of machine.";
    } else if (options_.executable.rfind(releases, 0) != 0) {
        // The image sets FERNSDR_CONTAINER; there the program is part of the
        // image, and a new image is the update.
        const char* container = std::getenv("FERNSDR_CONTAINER");
        unavailable_ = container && *container
                           ? "This receiver runs in a container: update it by pulling the new image and starting "
                             "the container again from it. Its configuration and everything it keeps stay in the "
                             "volume."
                           : "This receiver was not installed from a release by install.sh: update it the way it was "
                             "installed.";
    } else if (update < 0) {
        unavailable_ = "The updater is not set up: " + options_.update + " is missing.";
    }
    if (update >= 0) ::close(update);
}

UpdateService::~UpdateService() {
    if (worker_.joinable()) worker_.join();
}

bool UpdateService::check(std::string& error) {
    if (!unavailable_.empty()) {
        error = unavailable_;
        return false;
    }
    if (looking_.exchange(true)) {
        error = "A look for updates is already under way.";
        return false;
    }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = "checking";
        error_.clear();
    }
    worker_ = std::thread([this] { look(); });
    return true;
}

void UpdateService::look() {
    std::string manifest_text, signature, error;
    ReleaseManifest manifest;
    bool ok = options_.fetch(options_.base_url + kReleaseManifestName, 64 * 1024, manifest_text, error) &&
              options_.fetch(options_.base_url + kReleaseSignatureName, 64, signature, error);
    if (!ok) {
        error = "What is published could not be fetched: " + error;
    } else if (!verify_release_manifest(manifest_text, signature, options_.keys, manifest, error)) {
        ok = false;
        error = "What is published was refused: " + error;
    } else if (manifest.channel != "stable") {
        ok = false;
        error = "What is published is a " + manifest.channel + " release.";
    } else if (!manifest.asset_for(options_.platform)) {
        ok = false;
        error = "The release published has no archive for " + options_.platform + ".";
    }
    int order = 0;
    const bool newer = ok && compare_versions(manifest.version, options_.running_version, order) && order > 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ok ? "done" : "failed";
        checked_ = wall_seconds();
        found_ = ok ? manifest : ReleaseManifest{};
        newer_ = newer;
        error_ = ok ? "" : error;
    }
    looking_.store(false);
}

void UpdateService::wait() {
    while (looking_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

bool UpdateService::updating() const {
    const int dir = open_directory(options_.update);
    if (dir < 0) return false;
    std::string text, error;
    bool missing = false;
    const bool read = read_file_at(dir, kUpdateStatusFile, 64 * 1024, static_cast<uid_t>(-1), text, missing, error);
    ::close(dir);
    Json status;
    return read && Json::parse(text, status) && busy_state(status["state"].string());
}

bool UpdateService::start(const std::string& version, const std::vector<std::string>& running_bands,
                          std::string& error) {
    if (!unavailable_.empty()) {
        error = unavailable_;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "done" || !newer_ || found_.version != version) {
            error = "Look for updates first: " + version + " is not what the last look found.";
            return false;
        }
    }
    if (updating()) {
        error = "An update is already under way.";
        return false;
    }
    if (!waiting_request().empty()) {
        error = "A request is already waiting for the updater.";
        return false;
    }
    return request_update(options_.state, version, running_bands, error);
}

std::string UpdateService::waiting_request() const {
    const int dir = open_directory(options_.state);
    if (dir < 0) return "";
    std::string text, error;
    bool missing = false;
    const bool read = read_file_at(dir, kUpdateRequestFile, 256, ::geteuid(), text, missing, error);
    ::close(dir);
    // Something there that is not a file of this user's still wakes the
    // updater, which then refuses it.
    if (!read) return missing ? "" : "?";
    return text.empty() ? "?" : text;
}

std::string UpdateService::autostart_blocked() const {
    if (updating()) return "An update is under way; this can be switched once it is done.";
    const std::string waiting = waiting_request();
    if (!waiting.empty() && waiting != std::string(kAutostartOn) + "\n" &&
        waiting != std::string(kAutostartOff) + "\n") {
        return "An update has been asked for; this can be switched once it is done.";
    }
    // The trusted version's program is the updater. While a new version is
    // on trial that is the version before, which may not know this request.
    char target[256];
    const ssize_t n = ::readlink((options_.install + "/trusted").c_str(), target, sizeof(target) - 1);
    if (n <= 0 || std::string(target, static_cast<size_t>(n)) != "releases/" + options_.running_version) {
        return "This version is still on trial; this can be switched once it is kept.";
    }
    return "";
}

bool UpdateService::request_autostart(bool on, std::string& error) {
    if (!unavailable_.empty()) {
        error = unavailable_;
        return false;
    }
    const AutostartState state = options_.autostart();
    if (!state.changeable) {
        error = state.note.empty() ? "This machine's init cannot be switched from the admin panel." : state.note;
        return false;
    }
    if (const std::string blocked = autostart_blocked(); !blocked.empty()) {
        error = blocked;
        return false;
    }
    if (!waiting_request().empty()) {
        error = "A request is already waiting for the updater.";
        return false;
    }
    const int dir = open_directory(options_.state);
    if (dir < 0) {
        error = "cannot open " + options_.state;
        return false;
    }
    const bool written = write_file_at(dir, kUpdateRequestFile, std::string(on ? kAutostartOn : kAutostartOff) + "\n",
                                       0600, static_cast<uid_t>(-1), static_cast<gid_t>(-1), error);
    ::close(dir);
    return written;
}

Json UpdateService::autostart_view() const {
    const AutostartState state = options_.autostart();
    Json out = Json::make_object();
    out.set("init", state.init);
    out.set("enabled", state.enabled);
    const bool changeable = state.changeable && unavailable_.empty();
    out.set("changeable", changeable);
    if (state.changeable && !unavailable_.empty()) {
        out.set("note", switched_by_hand(state.init));
    } else if (!state.note.empty()) {
        out.set("note", state.note);
    }
    if (changeable) {
        const std::string waiting = waiting_request();
        if (waiting == std::string(kAutostartOn) + "\n") out.set("pending", "on");
        if (waiting == std::string(kAutostartOff) + "\n") out.set("pending", "off");
        if (const std::string blocked = autostart_blocked(); !blocked.empty()) out.set("blocked", blocked);
    }
    // The updater's account of the last switch, written as root; shown,
    // never acted on.
    const int dir = open_directory(options_.update);
    std::string text, error;
    bool missing = false;
    Json result;
    if (dir >= 0 && read_file_at(dir, kAutostartResultFile, 64 * 1024, static_cast<uid_t>(-1), text, missing, error) &&
        Json::parse(text, result) && result.is_object() && result["time"].is_number() && result["ok"].is_bool() &&
        result["message"].is_string()) {
        Json shown = Json::make_object();
        shown.set("time", result["time"].number());
        shown.set("ok", result["ok"].boolean());
        shown.set("message", result["message"].string());
        out.set("result", shown);
    }
    if (dir >= 0) ::close(dir);
    return out;
}

Json UpdateService::snapshot() const {
    Json out = Json::make_object();
    out.set("running", options_.running_version);
    out.set("platform", options_.platform);
    out.set("available", unavailable_.empty());
    if (!unavailable_.empty()) out.set("unavailable", unavailable_);
    Json check = Json::make_object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check.set("state", state_);
        if (checked_ > 0) check.set("checked", static_cast<double>(checked_));
        if (state_ == "done") {
            check.set("version", found_.version);
            check.set("date", found_.date);
            check.set("notes", found_.notes);
            check.set("newer", newer_);
        }
        if (!error_.empty()) check.set("error", error_);
    }
    out.set("check", check);
    // The updater's own account, written as root; shown, never acted on.
    Json status;
    const int dir = open_directory(options_.update);
    std::string text, error;
    bool missing = false;
    if (dir >= 0 && read_file_at(dir, kUpdateStatusFile, 64 * 1024, static_cast<uid_t>(-1), text, missing, error) &&
        Json::parse(text, status) && status.is_object()) {
        Json shown = Json::make_object();
        for (const char* key : {"state", "version", "message"}) {
            if (status[key].is_string()) shown.set(key, status[key].string());
        }
        if (status["time"].is_number()) shown.set("time", status["time"].number());
        out.set("status", shown);
    }
    if (dir >= 0) ::close(dir);
    out.set("autostart", autostart_view());
    return out;
}

}  // namespace fernsdr
