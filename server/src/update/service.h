// The admin panel's side of updates: what this receiver runs, whether it can
// update itself, what is published, and how an update it asked for goes. The
// updating itself is the updater's, as root; this only asks it.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../util/json.h"
#include "autostart.h"
#include "release.h"

namespace fernsdr {

struct UpdateServiceOptions {
    std::string state;       // the receiver's directory, where requests go
    std::string update;      // the updater's, with its status.json
    std::string install;     // where releases are installed, /opt/fernsdr
    std::string executable;  // this program, as the kernel names it
    std::string running_version;
    std::string platform;
    std::vector<ReleaseKey> keys;
    std::string base_url;
    std::function<bool(const std::string& url, size_t limit, std::string& body, std::string& error)> fetch;
    // Whether the receiver starts with the computer; read_autostart().
    std::function<AutostartState()> autostart;
};

class UpdateService {
public:
    explicit UpdateService(UpdateServiceOptions options);
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // This machine's: the paths of install.sh's layout, with the receiver's
    // configuration in `state_directory`, and the release keys built in.
    static UpdateServiceOptions system(const std::string& state_directory);

    // Why this receiver cannot update itself, or "" when it can.
    const std::string& unavailable() const { return unavailable_; }

    // Looks at what is published, on a thread of its own; false when a look
    // is already under way or updates are not available here.
    bool check(std::string& error);
    // Asks the updater for `version`, which the last look has to have found,
    // signed and newer.
    bool start(const std::string& version, const std::vector<std::string>& running_bands, std::string& error);
    // Asks the updater to make the receiver start with the computer, or
    // not. Refused where the init cannot be switched from here, while an
    // update is under way or on trial, and while a request is waiting.
    bool request_autostart(bool on, std::string& error);
    Json snapshot() const;

    // For tests: waits for a look to finish.
    void wait();

private:
    void look();
    bool updating() const;
    // The request waiting for the updater, as the receiver wrote it, or "".
    std::string waiting_request() const;
    // Why starting with the computer cannot be switched just now, or "".
    std::string autostart_blocked() const;
    Json autostart_view() const;

    UpdateServiceOptions options_;
    std::string unavailable_;
    mutable std::mutex mutex_;
    std::string state_ = "idle";  // idle, checking, done, failed
    int64_t checked_ = 0;
    ReleaseManifest found_;
    bool newer_ = false;
    std::string error_;
    std::atomic<bool> looking_{false};
    std::thread worker_;
};

}  // namespace fernsdr
