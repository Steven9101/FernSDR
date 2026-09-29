// The admin panel's side of modules: what is installed, what the configured
// catalog offers, what each module can see, and the jobs that change them.
//
// Every job runs on one thread of its own. Hashing a 32 MiB program, writing
// it to an SD card or waiting on GitHub must never happen on the network
// thread, where it would stall audio for everyone listening. The panel reads
// a snapshot published after each job and never waits for one.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "module_store.h"

namespace fernsdr {

bool valid_catalog_repository(const std::string& repository);

// One published release, reduced to what this receiver could install.
struct CatalogRelease {
    std::string tag;
    std::string name;
    std::string published;
    bool prerelease = false;
    std::string asset;  // the package for this platform, empty when it has none
    uint64_t size = 0;
    PackageName package;
};

// Reads GitHub's release list, keeping only what is safe to act on: tags
// that can go into a URL and packages named for this platform.
bool parse_github_releases(const std::string& body, std::vector<CatalogRelease>& out, std::string& error);

class ModuleManager {
public:
    // Without `threaded`, nothing runs until run_pending() is called; tests
    // and the command line use that.
    ModuleManager(std::shared_ptr<ModuleStore> store, std::vector<std::string> catalog, bool threaded = true);
    ~ModuleManager();
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    const std::shared_ptr<ModuleStore>& store() const { return store_; }
    const std::vector<std::string>& catalog() const { return catalog_; }

    // Set before the first job. Called on the job thread after a job changed
    // which program a band configured with `id` would run.
    void set_changed_callback(std::function<void(const std::string&)> callback) { on_changed_ = std::move(callback); }
    // The bands configured with a module, for the panel and for removal.
    void set_bands_callback(std::function<std::vector<std::string>(const std::string&)> callback) {
        bands_using_ = std::move(callback);
    }

    // Each queues a job and returns at once. False, with `error`, when the
    // request is refused outright: an unknown repository, a full queue.
    bool refresh(std::string& error);
    bool install(const std::string& repository, const std::string& tag, const std::string& asset, bool activate,
                 std::string& error);
    bool activate(const std::string& id, const std::string& version, std::string& error);
    bool set_enabled(const std::string& id, bool enabled, std::string& error);
    bool remove(const std::string& id, const std::string& version, std::string& error);
    bool list_devices(const std::string& id, std::string& error);

    // Everything the panel shows, as it stood after the last job.
    std::string snapshot() const;
    // Rebuilds that view, for when something outside a job changed it.
    void publish();
    // Runs queued work on the calling thread, when not threaded.
    void run_pending();

private:
    struct Job {
        uint64_t number = 0;
        std::string kind;
        std::string summary;
        std::string state = "queued";
        std::string message;
        int64_t finished_ms = 0;
        std::function<bool(std::string&)> work;
    };
    struct RepositoryState {
        std::vector<CatalogRelease> releases;
        std::string error;
        int64_t fetched_ms = 0;
    };
    struct Devices {
        Json list;
        std::string error;
        int64_t fetched_ms = 0;
    };

    bool queue(const std::string& kind, const std::string& summary, std::function<bool(std::string&)> work,
               std::string& error);
    void run();
    void execute(Job& job);
    bool fetch(const std::string& url, size_t limit, int seconds, std::string& body, std::string& error) const;
    bool do_refresh(std::string& message);
    bool do_install(const std::string& repository, const std::string& tag, const std::string& asset, bool activate,
                    std::string& message);
    bool do_devices(const std::string& id, std::string& message);

    std::shared_ptr<ModuleStore> store_;
    std::vector<std::string> catalog_;
    std::string downloader_;  // absolute path of curl, empty when it is not installed
    std::function<void(const std::string&)> on_changed_;
    std::function<std::vector<std::string>(const std::string&)> bands_using_;

    bool threaded_ = true;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> pending_;
    std::deque<Job> finished_;
    Job running_;  // `number` is 0 while nothing runs
    uint64_t next_job_ = 1;
    bool stopping_ = false;
    // Stops a download in progress when the receiver shuts down.
    std::atomic<bool> cancel_{false};
    // Written by jobs, read by publish(), which any thread may call.
    mutable std::mutex state_mutex_;
    std::map<std::string, RepositoryState> repositories_;
    std::map<std::string, Devices> devices_;
    Json base_;  // guarded by mutex_
    std::thread thread_;
};

}  // namespace fernsdr
