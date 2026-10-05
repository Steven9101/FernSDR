#include "modules.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "../util/log.h"
#include "../util/programs.h"
#include "../util/subprocess.h"
#include "../util/utf8.h"

namespace fernsdr {

namespace {

constexpr size_t kMaxQueuedJobs = 4;
constexpr size_t kFinishedJobsKept = 10;
constexpr size_t kReleaseListBytes = 2 * 1024 * 1024;
constexpr size_t kDeviceListBytes = 64 * 1024;

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool valid_tag(const std::string& tag) {
    if (tag.empty() || tag.size() > 64 || tag[0] == '.' || tag[0] == '-') return false;
    for (char c : tag) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

std::string first_line(const std::string& text) {
    return printable(text.substr(0, text.find('\n')), 200);
}

// The HTTP status in curl's "returned error: 404", or 0.
int http_status(const std::string& errors) {
    for (const char* marker : {"returned error: "}) {
        const size_t at = errors.find(marker);
        if (at == std::string::npos) continue;
        const int status = std::atoi(errors.c_str() + at + std::strlen(marker));
        if (status >= 100 && status < 600) return status;
    }
    return 0;
}

std::string describe_http_status(int status) {
    if (status == 404) {
        return "GitHub has nothing public at that address: the repository or release does not exist, or it is "
               "private";
    }
    if (status == 403 || status == 429) {
        return "GitHub refused for now. Without a token it allows 60 requests an hour from one address; wait and "
               "try again";
    }
    return "GitHub answered with HTTP " + std::to_string(status);
}

std::string describe_download_failure(const Subprocess::Exit& status, const std::string& errors) {
    std::string why;
    const int http = http_status(errors);
    if (http != 0) return describe_http_status(http);
    if (status.exited) {
        switch (status.code) {
            case 6: why = "the name could not be resolved; is this machine on the internet?"; break;
            case 7: why = "the connection was refused or timed out"; break;
            case 22: why = "GitHub answered with an error"; break;
            case 28: why = "it took too long"; break;
            case 35:
            case 60: why = "the secure connection failed; check the machine's clock and CA certificates"; break;
            case 63: why = "the file is larger than a module may be"; break;
            default: break;
        }
    }
    if (why.empty()) why = "curl " + status.describe();
    const std::string detail = first_line(errors);
    return detail.empty() ? why : why + " (" + detail + ")";
}

Json device_entry(const Json& device) {
    Json out = Json::make_object();
    if (device["index"].is_number()) out.set("index", device["index"].number());
    for (const char* key : {"name", "serial", "tuner", "error"}) {
        if (device[key].is_string()) out.set(key, printable(device[key].string(), 200));
    }
    out.set("usable", device["usable"].boolean(false));
    // What this one device tunes and at which rates, where it differs from
    // what the module's manifest says for all its devices: an Airspy R2 and
    // a Mini share a USB id and a module but not their rates.
    ModuleManifest::Tuning tuning;
    if (device.has("tuning") && parse_module_tuning(device["tuning"], tuning)) out.set("tuning", module_tuning_json(tuning));
    return out;
}

}  // namespace

bool valid_catalog_repository(const std::string& repository) {
    const size_t slash = repository.find('/');
    if (slash == std::string::npos || slash == 0 || slash > 39 || repository.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    const std::string owner = repository.substr(0, slash);
    const std::string name = repository.substr(slash + 1);
    if (owner[0] == '-' || name.empty() || name.size() > 100 || name == "." || name == "..") return false;
    for (char c : owner) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool parse_github_releases(const std::string& body, std::vector<CatalogRelease>& out, std::string& error) {
    out.clear();
    Json json;
    if (!Json::parse(body, json) || !json.is_array()) {
        error = "GitHub's answer is not a list of releases";
        return false;
    }
    for (const Json& release : json.elements()) {
        if (!release.is_object() || release["draft"].boolean(false)) continue;
        CatalogRelease entry;
        entry.tag = release["tag_name"].string();
        if (!valid_tag(entry.tag)) continue;
        entry.name = printable(release["name"].string(), 100);
        entry.published = printable(release["published_at"].string(), 32);
        entry.prerelease = release["prerelease"].boolean(false);
        for (const Json& asset : release["assets"].elements()) {
            PackageName parsed;
            const std::string name = asset["name"].string();
            if (!parse_package_name(name, parsed) || parsed.platform != module_platform()) continue;
            const double size = asset["size"].number(0);
            if (size <= 0 || size > static_cast<double>(kMaxModuleBytes + kMaxManifestBytes + 64)) continue;
            entry.asset = name;
            entry.size = static_cast<uint64_t>(size);
            entry.package = parsed;
            break;
        }
        out.push_back(std::move(entry));
        if (out.size() >= 30) break;
    }
    return true;
}

ModuleManager::ModuleManager(std::shared_ptr<ModuleStore> store, std::vector<std::string> catalog, bool threaded)
    : store_(std::move(store)), threaded_(threaded) {
    for (const std::string& repository : catalog) {
        if (valid_catalog_repository(repository)) {
            catalog_.push_back(repository);
        } else {
            LOG_WARN("modules", "catalog entry '%s' is not owner/name; ignored", repository.c_str());
        }
    }
    // curl only. It can be told to follow only HTTPS redirects, and wget
    // cannot outside recursive mode, which would let a plain HTTP hop swap a
    // package whose checksum only proves it matches its own manifest.
    downloader_ = find_program("curl");
    publish();
    if (threaded_) thread_ = std::thread([this] { run(); });
}

ModuleManager::~ModuleManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cancel_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool ModuleManager::queue(const std::string& kind, const std::string& summary, std::function<bool(std::string&)> work,
                          std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        error = "the receiver is shutting down";
        return false;
    }
    if (pending_.size() >= kMaxQueuedJobs) {
        error = "four module jobs are already waiting; try again when they have finished";
        return false;
    }
    Job job;
    job.number = next_job_++;
    job.kind = kind;
    job.summary = summary;
    job.work = std::move(work);
    pending_.push_back(std::move(job));
    wake_.notify_one();
    return true;
}

bool ModuleManager::refresh(std::string& error) {
    if (catalog_.empty()) {
        error = "no repositories are listed in [modules] catalog";
        return false;
    }
    return queue("refresh", "Checking the catalog for releases", [this](std::string& message) { return do_refresh(message); },
                 error);
}

bool ModuleManager::install(const std::string& repository, const std::string& tag, const std::string& asset,
                            bool activate, std::string& error) {
    PackageName name;
    if (std::find(catalog_.begin(), catalog_.end(), repository) == catalog_.end()) {
        error = repository + " is not in [modules] catalog";
        return false;
    }
    if (!valid_tag(tag) || !parse_package_name(asset, name) || name.platform != module_platform()) {
        error = "that is not a package for this platform";
        return false;
    }
    return queue("install", (activate ? "Updating to " : "Installing ") + name.id + " " + name.version,
                 [this, repository, tag, asset, activate](std::string& message) {
                     return do_install(repository, tag, asset, activate, message);
                 },
                 error);
}

bool ModuleManager::activate(const std::string& id, const std::string& version, std::string& error) {
    if (!valid_module_id(id) || !valid_module_version(version)) {
        error = "no such module version";
        return false;
    }
    return queue("activate", "Switching " + id + " to " + version,
                 [this, id, version](std::string& message) {
                     if (!store_->activate(id, version, message)) return false;
                     if (on_changed_) on_changed_(id);
                     message = id + " " + version + " is now the version bands use";
                     return true;
                 },
                 error);
}

bool ModuleManager::set_enabled(const std::string& id, bool enabled, std::string& error) {
    if (!valid_module_id(id)) {
        error = "no such module";
        return false;
    }
    return queue("enable", std::string(enabled ? "Switching on " : "Switching off ") + id,
                 [this, id, enabled](std::string& message) {
                     if (!store_->set_enabled(id, enabled, message)) return false;
                     if (on_changed_) on_changed_(id);
                     message = id + (enabled ? " is switched on" : " is switched off");
                     return true;
                 },
                 error);
}

bool ModuleManager::remove(const std::string& id, const std::string& version, std::string& error) {
    if (!valid_module_id(id) || !valid_module_version(version)) {
        error = "no such module version";
        return false;
    }
    return queue("remove", "Removing " + id + " " + version,
                 [this, id, version](std::string& message) {
                     const bool in_use = bands_using_ && !bands_using_(id).empty();
                     if (!store_->remove(id, version, in_use, message)) return false;
                     message = "removed " + id + " " + version;
                     return true;
                 },
                 error);
}

bool ModuleManager::list_devices(const std::string& id, std::string& error) {
    if (!valid_module_id(id)) {
        error = "no such module";
        return false;
    }
    return queue("devices", "Asking " + id + " which devices it sees",
                 [this, id](std::string& message) { return do_devices(id, message); }, error);
}

void ModuleManager::run_pending() {
    if (threaded_) return;
    while (true) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty()) return;
            job = std::move(pending_.front());
            pending_.pop_front();
        }
        execute(job);
    }
}

void ModuleManager::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            job = std::move(pending_.front());
            pending_.pop_front();
        }
        execute(job);
    }
}

void ModuleManager::execute(Job& job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = job;
        running_.work = nullptr;
        running_.state = "running";
    }
    std::string message;
    bool ok = false;
    try {
        ok = job.work(message);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    job.work = nullptr;
    job.state = ok ? "done" : "failed";
    job.finished_ms = wall_ms();
    job.message = printable(message, 600);
    if (ok) LOG_INFO("modules", "%s: %s", job.summary.c_str(), job.message.c_str());
    else LOG_WARN("modules", "%s failed: %s", job.summary.c_str(), job.message.c_str());
    publish();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = Job{};
    running_.number = 0;
    finished_.push_front(std::move(job));
    while (finished_.size() > kFinishedJobsKept) finished_.pop_back();
}

bool ModuleManager::fetch(const std::string& url, size_t limit, int seconds, std::string& body,
                          std::string& error) const {
    if (downloader_.empty()) {
        error = "curl is not installed (sudo apt install curl). Or copy the package to this machine and run "
                "fernsdr --install-module";
        return false;
    }
    // -q first: it makes curl ignore any .curlrc, so these arguments are the
    // whole of what it does.
    const std::vector<std::string> arguments = {
        "-q", "--silent", "--show-error", "--fail", "--location", "--max-redirs", "5",
        "--proto", "=https", "--proto-redir", "=https", "--max-time", std::to_string(seconds),
        "--max-filesize", std::to_string(limit), "--user-agent", "FernSDR",
        "--header", "Accept: application/vnd.github+json", "--output", "-", url};
    std::string errors;
    Subprocess::Exit status;
    if (!Subprocess::run(downloader_, arguments, network_helper_environment(), std::chrono::seconds(seconds + 5), limit,
                         body, errors, status, error, &cancel_)) {
        return false;
    }
    if (!status.exited || status.code != 0) {
        error = describe_download_failure(status, errors);
        return false;
    }
    return true;
}

bool ModuleManager::do_refresh(std::string& message) {
    int failures = 0;
    size_t releases = 0;
    for (const std::string& repository : catalog_) {
        RepositoryState state;
        std::string body, error;
        if (!fetch("https://api.github.com/repos/" + repository + "/releases?per_page=30", kReleaseListBytes, 20, body,
                   error) ||
            !parse_github_releases(body, state.releases, error)) {
            state.error = error;
            failures++;
        }
        releases += state.releases.size();
        state.fetched_ms = wall_ms();
        std::lock_guard<std::mutex> lock(state_mutex_);
        repositories_[repository] = std::move(state);
    }
    message = failures == 0 ? "found " + std::to_string(releases) + " release(s)"
                            : std::to_string(failures) + " of " + std::to_string(catalog_.size()) +
                                  " repositories could not be read";
    return failures == 0;
}

bool ModuleManager::do_install(const std::string& repository, const std::string& tag, const std::string& asset,
                               bool activate, std::string& message) {
    PackageName name;
    if (!parse_package_name(asset, name)) {
        message = "that is not a package name";
        return false;
    }
    ModuleStore::Module before;
    store_->find(name.id, before);
    // Built from the configured repository and validated parts: nothing that
    // came from a browser or from GitHub's answer is used as a URL.
    const std::string url = "https://github.com/" + repository + "/releases/download/" + tag + "/" + asset;
    std::string package;
    if (!fetch(url, kMaxModuleBytes + kMaxManifestBytes + 64, 180, package, message)) return false;
    ModuleManifest installed;
    if (!store_->install(package, repository, &name, installed, message)) return false;
    if (activate && !store_->activate(installed.id, installed.version, message)) return false;
    ModuleStore::Module after;
    store_->find(installed.id, after);
    if (after.active != before.active && on_changed_) on_changed_(installed.id);
    message = "installed " + installed.id + " " + installed.version +
              (after.active == installed.version ? "; bands use it now" : "");
    return true;
}

bool ModuleManager::do_devices(const std::string& id, std::string& message) {
    Devices devices;
    devices.fetched_ms = wall_ms();
    ModuleStore::Module module;
    if (!store_->find(id, module) || module.active.empty()) {
        message = id + " is not installed";
        devices.error = message;
        std::lock_guard<std::mutex> lock(state_mutex_);
        devices_[id] = devices;
        return false;
    }
    // Only an input module is asked. A decoder runs confined when it
    // decodes, and asking it here would start it unconfined, with the
    // receiver's own access, on a click.
    const auto active = std::find_if(module.versions.begin(), module.versions.end(),
                                     [&](const ModuleManifest& m) { return m.version == module.active; });
    if (active == module.versions.end() || active->kind != "input") {
        message = id + " is not a radio module";
        devices.error = message;
        std::lock_guard<std::mutex> lock(state_mutex_);
        devices_[id] = devices;
        return false;
    }
    const std::string executable = store_->directory() + "/" + id + "/" + module.active + "/module";
    std::string output, errors, error;
    Subprocess::Exit status;
    if (!Subprocess::run(executable, {"--list-devices"}, module_environment(), std::chrono::seconds(10),
                         kDeviceListBytes, output, errors, status, error, &cancel_)) {
        devices.error = error;
    } else if (!status.exited || status.code != 0) {
        const std::string detail = first_line(errors);
        devices.error = id + " --list-devices " + status.describe() + (detail.empty() ? "" : ": " + detail);
    } else {
        Json parsed;
        if (!Json::parse(output, parsed) || !parsed["devices"].is_array()) {
            devices.error = id + " --list-devices did not print a device list";
        } else {
            devices.list = Json::make_array();
            for (const Json& device : parsed["devices"].elements()) {
                if (devices.list.size() >= 32) break;
                if (device.is_object()) devices.list.push_back(device_entry(device));
            }
        }
    }
    message = devices.error.empty() ? "found " + std::to_string(devices.list.size()) + " device(s)" : devices.error;
    const bool ok = devices.error.empty();
    std::lock_guard<std::mutex> lock(state_mutex_);
    devices_[id] = std::move(devices);
    return ok;
}

void ModuleManager::publish() {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    Json out = Json::make_object();
    out.set("directory", store_->directory());
    out.set("platform", module_platform());
    out.set("downloader", downloader_.substr(downloader_.find_last_of('/') + 1));
    Json catalog = Json::make_array();
    for (const std::string& repository : catalog_) catalog.push_back(repository);
    out.set("catalog", catalog);

    Json installed = Json::make_array();
    for (const ModuleStore::Module& module : store_->list()) {
        Json entry = Json::make_object();
        entry.set("id", module.id);
        entry.set("active", module.active);
        entry.set("enabled", module.enabled);
        entry.set("origin", module.origin);
        Json versions = Json::make_array();
        for (const ModuleManifest& manifest : module.versions) {
            Json version = manifest.to_json();
            if (manifest.version != module.active) version.set("settings", Json::make_array());
            versions.push_back(version);
        }
        entry.set("versions", versions);
        Json bands = Json::make_array();
        if (bands_using_) {
            for (const std::string& band : bands_using_(module.id)) bands.push_back(band);
        }
        entry.set("bands", bands);
        // The newest release of this module from the repository it came
        // from, when that is newer than what bands use.
        std::string update, update_tag, update_asset;
        const auto source = repositories_.find(module.origin);
        if (source != repositories_.end()) {
            for (const CatalogRelease& release : source->second.releases) {
                if (release.prerelease || release.asset.empty() || release.package.id != module.id) continue;
                if (compare_module_versions(release.package.version, module.active) <= 0) continue;
                if (update.empty() || compare_module_versions(release.package.version, update) > 0) {
                    update = release.package.version;
                    update_tag = release.tag;
                    update_asset = release.asset;
                }
            }
        }
        if (!update.empty()) {
            Json offer = Json::make_object();
            offer.set("version", update);
            offer.set("repository", module.origin);
            offer.set("tag", update_tag);
            offer.set("asset", update_asset);
            entry.set("update", offer);
        }
        installed.push_back(entry);
    }
    out.set("installed", installed);

    Json available = Json::make_array();
    for (const std::string& repository : catalog_) {
        Json entry = Json::make_object();
        entry.set("repository", repository);
        const auto found = repositories_.find(repository);
        if (found != repositories_.end()) {
            entry.set("fetched_ms", static_cast<double>(found->second.fetched_ms));
            if (!found->second.error.empty()) entry.set("error", found->second.error);
            Json releases = Json::make_array();
            for (const CatalogRelease& release : found->second.releases) {
                Json item = Json::make_object();
                item.set("tag", release.tag);
                item.set("name", release.name);
                item.set("published", release.published);
                item.set("prerelease", release.prerelease);
                if (!release.asset.empty()) {
                    item.set("asset", release.asset);
                    item.set("size", static_cast<double>(release.size));
                    item.set("id", release.package.id);
                    item.set("version", release.package.version);
                }
                releases.push_back(item);
            }
            entry.set("releases", releases);
        }
        available.push_back(entry);
    }
    out.set("available", available);

    Json devices = Json::make_object();
    for (const auto& [id, found] : devices_) {
        Json entry = Json::make_object();
        entry.set("fetched_ms", static_cast<double>(found.fetched_ms));
        if (!found.error.empty()) entry.set("error", found.error);
        entry.set("devices", found.list.is_array() ? found.list : Json::make_array());
        devices.set(id, entry);
    }
    out.set("devices", devices);

    std::lock_guard<std::mutex> lock(mutex_);
    base_ = std::move(out);
}

std::string ModuleManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = base_;
    Json jobs = Json::make_array();
    auto describe = [](const Job& job) {
        Json entry = Json::make_object();
        entry.set("number", static_cast<double>(job.number));
        entry.set("kind", job.kind);
        entry.set("summary", job.summary);
        entry.set("state", job.state);
        if (!job.message.empty()) entry.set("message", job.message);
        if (job.finished_ms) entry.set("finished_ms", static_cast<double>(job.finished_ms));
        return entry;
    };
    if (running_.number != 0) jobs.push_back(describe(running_));
    for (const Job& job : pending_) jobs.push_back(describe(job));
    for (const Job& job : finished_) jobs.push_back(describe(job));
    out.set("jobs", jobs);
    return out.serialize();
}

}  // namespace fernsdr
