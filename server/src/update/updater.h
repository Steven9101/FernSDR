// The update itself: what `fernsdr --update-run` does as root when the
// receiver asks for a version, and `fernsdr --update-boot` before the
// receiver starts. The receiver's side, which asks and later says that the
// new version works, is at the end.
//
// A release is unpacked into releases/VERSION and becomes `current`, the
// version the receiver's service runs, on trial. Only once the receiver
// running it writes that it works does it become `trusted` too, the version
// whose program does the next update. Everything the switch could leave
// half done is recorded before it is done: a trial that is interrupted, by a
// crash or a restart of the machine, ends with the trusted version back.
#pragma once
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "release.h"

namespace fernsdr {

struct UpdateLayout {
    std::string install;  // /opt/fernsdr: releases/, current, trusted; root's
    std::string state;    // /var/lib/fernsdr: the receiver's user's
    std::string update;   // /var/lib/fernsdr-update: root's, readable by all
    // In a container, where /opt/fernsdr is lost when the container is made
    // again: the signed release `trusted` names is kept here as it was
    // published, in the volume, for the next container to check and unpack
    // (see container.h). Empty elsewhere.
    bool keep_releases = false;

    // install.sh's layout, or the container's when FERNSDR_CONTAINER is
    // set: /opt/fernsdr in the container, and the update directory in the
    // volume, as /var/lib/fernsdr/update.
    static UpdateLayout standard();
};

// In the state directory, written by the receiver.
constexpr const char* kUpdateRequestFile = "update-request";  // the version asked for
constexpr const char* kUpdateBandsFile = "update-bands";      // bands running when it was asked
constexpr const char* kUpdateCommitFile = "update-commit";    // written by a new version that works
// In the update directory, written by the updater.
constexpr const char* kUpdateStatusFile = "status.json";
constexpr const char* kUpdateTrialFile = "trial";
// How the last switch of starting with the computer went: time, ok, enabled
// (what was asked for) and message. Shown, never acted on.
constexpr const char* kAutostartResultFile = "autostart.json";

// Configuration files the receiver writes beside its config, copied before a
// switch and put back when the new version is rolled back: it may have
// rewritten them in a form the old one cannot read.
extern const char* const kUpdateSnapshotFiles[3];

// What the updater needs from outside, so a test can stand in for it.
struct UpdateEnvironment {
    std::vector<ReleaseKey> keys;
    std::string platform;         // release_platform()
    std::string running_version;  // this program's version, the trusted one
    std::string base_url;         // where the release assets are, ending in '/'
    int64_t trial_ms = 5 * 60 * 1000;
    int64_t poll_ms = 1000;
    // How many automatic restarts of the receiver's service during a trial
    // count as a crash loop.
    int restart_limit = 3;
    // The receiver never runs as root, so a state directory of root's is a
    // mistake the updater refuses; only a test running as root sets this.
    bool state_may_be_roots = false;

    std::function<bool(const std::string& url, size_t limit, std::string& body, std::string& error)> fetch;
    // Makes the receiver start with the computer, or not (set_autostart).
    std::function<bool(bool on, std::string& error)> autostart;
    // Runs `program --check config --root web` as uid:gid; false with its
    // output when it refuses.
    std::function<bool(const std::string& program, const std::string& config, const std::string& web, uid_t uid,
                       gid_t gid, std::string& output)>
        check;
    std::function<bool(std::string& error)> restart;
    // The receiver's service: its automatic restarts so far and whether it
    // has given up.
    std::function<bool(int& restarts, bool& failed)> service;
    std::function<int64_t()> now_ms;
    std::function<void(int64_t ms)> sleep_ms;
};

struct UpdateOutcome {
    enum class Result { Nothing, Updated, Refused, RolledBack, Failed };
    Result result = Result::Nothing;
    std::string message;
};

// Acts on a request the receiver left, if there is one: a version to update
// to, or exactly kAutostartOn or kAutostartOff and a newline.
UpdateOutcome run_update(const UpdateLayout& layout, const UpdateEnvironment& environment);

// Before the receiver starts: ends a trial that was interrupted.
UpdateOutcome run_boot_check(const UpdateLayout& layout, const UpdateEnvironment& environment);

// Unpacks a release's archive, already checked against its signed manifest,
// into releases/<version> below `releases`, by way of a directory of its own
// that is renamed into place once complete. The archive has to hold the
// receiver and its pages.
bool install_release(int releases, const std::string& version, const std::string& archive, std::string& error);

// The versions a trial record in the update directory `update` names; false
// when there is none or it does not read.
bool read_trial_versions(int update, std::string& old_version, std::string& new_version);

// The receiver's side. At start, whether this program is the new version of
// a trial; later, once it has served for `settle_ms` with every band that ran
// before the update running again, it writes the commit file, once.
class UpdateTrial {
public:
    UpdateTrial(std::string state_directory, std::string update_directory, std::string version);

    bool on_trial() const { return on_trial_; }
    // Bands to wait for, from the request.
    const std::vector<std::string>& bands() const { return bands_; }
    // True once the commit file is written.
    bool tick(int64_t now_ms, const std::vector<std::string>& running_bands, int64_t settle_ms = 60 * 1000);

private:
    std::string state_;
    std::string version_;
    bool on_trial_ = false;
    bool committed_ = false;
    int64_t started_ms_ = -1;
    std::vector<std::string> bands_;
};

// Asks the updater for `version`: the bands running now, then the request,
// whose writing wakes the updater.
bool request_update(const std::string& state_directory, const std::string& version,
                    const std::vector<std::string>& running_bands, std::string& error);

}  // namespace fernsdr
