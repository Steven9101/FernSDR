#include "updater.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

#include "../util/json.h"
#include "../util/password.h"
#include "autostart.h"
#include "container.h"
#include "extract.h"
#include "files.h"
#include "ustar.h"

namespace fernsdr {

const char* const kUpdateSnapshotFiles[3] = {"fernsdr.conf", "fernsdr-settings.json", "fernsdr-theme.json"};

UpdateLayout UpdateLayout::standard() {
    const char* container = std::getenv("FERNSDR_CONTAINER");
    if (container && *container) return container_layout();
    return {"/opt/fernsdr", "/var/lib/fernsdr", "/var/lib/fernsdr-update"};
}

namespace {

constexpr size_t kRequestBytes = 256;
constexpr size_t kSnapshotBytes = 16u << 20;
constexpr size_t kTrialBytes = 4096;
constexpr const char* kReleases = "releases";
constexpr const char* kSnapshot = "snapshot";
constexpr const char* kLock = "lock";
constexpr const char* kStagingPrefix = ".staging-";
// Room to leave on the disk beyond the unpacked release, which is about the
// size of its archive.
constexpr uint64_t kSpareBytes = 32u << 20;

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
    int get() const { return fd_; }

private:
    int fd_;
};

bool valid_version(const std::string& version) {
    int order = 0;
    return compare_versions(version, version, order);
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

std::string sha256_hex(const std::string& data) {
    Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, sizeof(digest));
}

// The trial record: the switch that has been made and not yet committed, and
// which of the snapshot's files existed before it.
struct Trial {
    std::string old_version;
    std::string new_version;
    int64_t started_ms = 0;
    std::vector<std::string> present;
};

std::string format_trial(const Trial& trial) {
    std::string text = "old " + trial.old_version + "\nnew " + trial.new_version + "\nstarted " +
                       std::to_string(trial.started_ms) + "\npresent";
    for (const std::string& name : trial.present) text += " " + name;
    return text + "\n";
}

bool snapshot_name(const std::string& name) {
    return std::find_if(std::begin(kUpdateSnapshotFiles), std::end(kUpdateSnapshotFiles),
                        [&](const char* file) { return name == file; }) != std::end(kUpdateSnapshotFiles);
}

bool parse_trial(const std::string& text, Trial& trial) {
    trial = Trial{};
    size_t at = 0;
    bool started = false, present = false;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string value = space == std::string::npos ? "" : line.substr(space + 1);
        if (key == "old") {
            trial.old_version = value;
        } else if (key == "new") {
            trial.new_version = value;
        } else if (key == "started") {
            char* stop = nullptr;
            trial.started_ms = std::strtoll(value.c_str(), &stop, 10);
            started = !value.empty() && *stop == '\0';
        } else if (key == "present") {
            present = true;
            size_t from = 0;
            while (from < value.size()) {
                size_t to = value.find(' ', from);
                if (to == std::string::npos) to = value.size();
                const std::string name = value.substr(from, to - from);
                if (!snapshot_name(name)) return false;
                trial.present.push_back(name);
                from = to + 1;
            }
        } else {
            return false;
        }
    }
    return started && present && valid_version(trial.old_version) && valid_version(trial.new_version);
}

// "releases/0.1.1" to "0.1.1"; "" for anything else.
std::string version_of_target(const std::string& target) {
    const std::string prefix = std::string(kReleases) + "/";
    if (target.rfind(prefix, 0) != 0) return "";
    const std::string version = target.substr(prefix.size());
    return valid_version(version) ? version : "";
}

class Updater {
public:
    Updater(const UpdateLayout& layout, const UpdateEnvironment& environment)
        : layout_(layout), env_(environment) {}

    UpdateOutcome update();
    UpdateOutcome boot();

private:
    bool open(std::string& error);
    void status(const std::string& state, const std::string& version, const std::string& message);
    bool recover(bool restart, UpdateOutcome& outcome);
    // Puts the trusted version back. With `restore`, the configuration files
    // go back to the snapshot too: those it held are written, those that were
    // not there before are removed. Only a record that could be read says
    // which is which.
    bool roll_back(const Trial& trial, const std::string& trusted_version, bool restore, bool restart,
                   std::string& note, std::string& error);
    bool commit(const Trial& trial, std::string& error);
    void prune(const std::string& keep_a, const std::string& keep_b);
    bool take_snapshot(std::vector<std::string>& present, std::string& error);
    UpdateOutcome supervise(const Trial& trial);
    UpdateOutcome switch_autostart(bool on);
    bool release_present(const std::string& version) const {
        struct stat info {};
        return ::fstatat(releases_.get(), version.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(info.st_mode);
    }
    UpdateOutcome done(UpdateOutcome::Result result, const std::string& state, const std::string& version,
                       const std::string& message) {
        status(state, version, message);
        return {result, message};
    }

    const UpdateLayout& layout_;
    const UpdateEnvironment& env_;
    Fd install_, releases_, state_, update_, lock_;
    uid_t uid_ = 0;
    gid_t gid_ = 0;
    uid_t me_ = 0;
    // Set by commit() when the volume could not record the new version.
    std::string keep_note_;
};

bool Updater::open(std::string& error) {
    me_ = ::geteuid();
    install_.reset(open_directory(layout_.install));
    releases_.reset(install_.get() >= 0 ? open_directory_at(install_.get(), kReleases) : -1);
    state_.reset(open_directory(layout_.state));
    update_.reset(open_directory(layout_.update));
    if (install_.get() < 0 || releases_.get() < 0 || state_.get() < 0 || update_.get() < 0) {
        error = "this is not an install the updater knows: " + layout_.install + "/releases, " + layout_.state +
                " and " + layout_.update + " have to exist";
        return false;
    }
    // Root works in these by name. One that another user could change, or
    // put in place of the real one through a directory of theirs on the
    // way, as the receiver could with the update directory in a container's
    // volume, would let that user choose what root does there.
    if (!held_by_this_user(install_.get()) || !held_by_this_user(releases_.get()) ||
        !held_by_this_user(update_.get())) {
        error = layout_.install + ", its releases and " + layout_.update + " have to be root's alone";
        return false;
    }
    if (layout_.keep_releases && !update_directory_recorded(install_.get(), update_.get(), error)) return false;
    // The receiver's user is whoever owns its state directory: no user
    // database to ask, which a static program could not do reliably anyway.
    struct stat info {};
    if (::fstat(state_.get(), &info) != 0) {
        error = "cannot look at " + layout_.state;
        return false;
    }
    uid_ = info.st_uid;
    gid_ = info.st_gid;
    if (uid_ == 0 && !env_.state_may_be_roots) {
        error = layout_.state + " belongs to root; the receiver has to run as a user of its own";
        return false;
    }
    lock_.reset(::openat(update_.get(), kLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (lock_.get() < 0 || ::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
        error = "another update is running";
        return false;
    }
    return true;
}

void Updater::status(const std::string& state, const std::string& version, const std::string& message) {
    Json out = Json::make_object();
    out.set("state", state);
    out.set("version", version);
    out.set("message", message);
    out.set("time", static_cast<double>(env_.now_ms() / 1000));
    std::string error;
    // Read by the receiver's panel, so readable by all; nothing in it is
    // secret.
    write_file_at(update_.get(), kUpdateStatusFile, out.serialize() + "\n", 0644, static_cast<uid_t>(-1),
                  static_cast<gid_t>(-1), error);
}

bool Updater::recover(bool restart, UpdateOutcome& outcome) {
    std::string text, error;
    bool missing = false;
    if (!read_file_at(update_.get(), kUpdateTrialFile, kTrialBytes, me_, text, missing, error)) {
        if (missing) return true;
        outcome = done(UpdateOutcome::Result::Failed, "failed", "", "the trial record cannot be read: " + error);
        return false;
    }
    std::string trusted_target;
    const std::string trusted =
        read_link_at(install_.get(), "trusted", trusted_target) ? version_of_target(trusted_target) : "";
    if (trusted.empty()) {
        outcome = done(UpdateOutcome::Result::Failed, "failed", "", "the install has no trusted version to go back to");
        return false;
    }
    Trial trial;
    const bool readable = parse_trial(text, trial);
    if (!readable) trial = Trial{trusted, trusted, 0, {}};
    std::string confirmation;
    bool no_confirmation = false;
    // A new version whose directory is gone, as when a container is made
    // again during its trial and the volume could not vouch for it, cannot
    // be kept, whatever it said.
    const bool confirmed =
        readable && release_present(trial.new_version) &&
        read_file_at(state_.get(), kUpdateCommitFile, 64, uid_, confirmation, no_confirmation, error) &&
        trimmed(confirmation) == trial.new_version;
    if ((trial.new_version == trusted && trial.old_version != trusted) || confirmed) {
        // Committed and interrupted before the record was removed, or said to
        // work before the updater could read that: kept either way.
        if (!commit(trial, error)) {
            outcome = done(UpdateOutcome::Result::Failed, "failed", trusted, error);
            return false;
        }
        outcome = done(UpdateOutcome::Result::Updated, "updated", trial.new_version,
                       "Updated to " + trial.new_version + "." + keep_note_);
        return true;
    }
    const std::string why = "The update to " + trial.new_version +
                            " was interrupted before the new version said it works; " + trusted + " is back.";
    std::string note;
    if (!roll_back(trial, trusted, readable, restart, note, error)) {
        outcome = done(UpdateOutcome::Result::Failed, "failed", trial.new_version, error);
        return false;
    }
    outcome = done(UpdateOutcome::Result::RolledBack, "rolled-back", trial.new_version, why + note);
    return true;
}

bool Updater::roll_back(const Trial& trial, const std::string& trusted_version, bool restore, bool restart,
                        std::string& note, std::string& error) {
    note.clear();
    if (!replace_link_at(install_.get(), "current", std::string(kReleases) + "/" + trusted_version, error)) return false;
    // The configuration goes back file by file, and a file that cannot does
    // not stop the rest: whatever the receiver's user left in the way, a
    // rollback half done would leave the record behind and every later
    // update stuck on it.
    Fd snapshot(restore ? open_directory_at(update_.get(), kSnapshot) : -1);
    std::vector<std::string> kept_back;
    for (const char* name : kUpdateSnapshotFiles) {
        if (!restore) break;
        const bool was_there =
            std::find(trial.present.begin(), trial.present.end(), std::string(name)) != trial.present.end();
        std::string why;
        // A directory in the file's place: renaming a file over it would fail.
        // A link is replaced by the rename itself, not followed.
        struct stat info {};
        const bool directory = ::fstatat(state_.get(), name, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(info.st_mode);
        bool done_with = directory ? remove_tree_at(state_.get(), name, why) : true;
        if (done_with && !was_there) {
            // Made during the trial: the old version never had it.
            done_with = remove_file_at(state_.get(), name, why);
        } else if (done_with) {
            std::string contents;
            bool missing = false;
            done_with = snapshot.get() >= 0 &&
                        read_file_at(snapshot.get(), name, kSnapshotBytes, me_, contents, missing, why) &&
                        write_file_at(state_.get(), name, contents, 0600, uid_, gid_, why);
        }
        if (!done_with) kept_back.push_back(name);
    }
    std::string ignored;
    remove_file_at(state_.get(), kUpdateCommitFile, ignored);
    if (!remove_file_at(update_.get(), kUpdateTrialFile, error)) return false;
    if (kept_back.empty()) {
        remove_tree_at(update_.get(), kSnapshot, ignored);
    } else {
        note = " These could not be put back as they were, and are copied in " + layout_.update + "/snapshot:";
        for (const std::string& name : kept_back) note += " " + name;
        note += ".";
    }
    std::string why;
    if (restart && env_.restart && !env_.restart(why)) {
        error = "the old version is back in place, but its service did not restart: " + why;
        return false;
    }
    if (trial.new_version != trusted_version) {
        remove_tree_at(releases_.get(), trial.new_version, ignored);
        if (layout_.keep_releases) forget_release(update_.get(), trial.new_version);
    }
    return true;
}

bool Updater::commit(const Trial& trial, std::string& error) {
    // The volume first: a container made again between the two starts the
    // version that said it works either way. One the volume cannot record
    // is still kept here; only a new container would not know it.
    keep_note_.clear();
    std::string why;
    if (layout_.keep_releases && !keep_trusted(update_.get(), trial.new_version, why)) {
        keep_note_ = " The volume could not record it (" + why +
                     "), so a container made again starts the release it kept before, or the image's.";
    }
    if (!replace_link_at(install_.get(), "trusted", std::string(kReleases) + "/" + trial.new_version, error)) {
        return false;
    }
    if (!remove_file_at(update_.get(), kUpdateTrialFile, error)) return false;
    remove_tree_at(update_.get(), kSnapshot, error);
    remove_file_at(state_.get(), kUpdateCommitFile, error);
    prune(trial.new_version, trial.old_version);
    return true;
}

// Keeps the trusted version and the one before it, which is what an operator
// goes back to by hand; removes other versions and unfinished unpacking.
void Updater::prune(const std::string& keep_a, const std::string& keep_b) {
    std::vector<std::string> names;
    const int copy = ::fcntl(releases_.get(), F_DUPFD_CLOEXEC, 0);
    DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr;
    if (!listing) {
        if (copy >= 0) ::close(copy);
        return;
    }
    while (const dirent* entry = ::readdir(listing)) names.push_back(entry->d_name);
    ::closedir(listing);
    std::string error;
    for (const std::string& name : names) {
        if (name == "." || name == ".." || name == keep_a || name == keep_b) continue;
        if (valid_version(name) || name.rfind(kStagingPrefix, 0) == 0) remove_tree_at(releases_.get(), name, error);
    }
}

bool Updater::take_snapshot(std::vector<std::string>& present, std::string& error) {
    present.clear();
    std::string ignored;
    remove_tree_at(update_.get(), kSnapshot, ignored);
    if (::mkdirat(update_.get(), kSnapshot, 0700) != 0) {
        error = std::string("cannot make the snapshot directory: ") + std::strerror(errno);
        return false;
    }
    Fd snapshot(open_directory_at(update_.get(), kSnapshot));
    if (snapshot.get() < 0) {
        error = "cannot open the snapshot directory";
        return false;
    }
    for (const char* name : kUpdateSnapshotFiles) {
        std::string contents;
        bool missing = false;
        if (!read_file_at(state_.get(), name, kSnapshotBytes, uid_, contents, missing, error)) {
            if (missing) {
                error.clear();
                continue;
            }
            // A link or a second name here is not something to copy as
            // root, and restoring it later would copy it back.
            error = "the update will not copy " + std::string(name) + ": " + error;
            return false;
        }
        if (!write_file_at(snapshot.get(), name, contents, 0600, static_cast<uid_t>(-1), static_cast<gid_t>(-1),
                           error)) {
            return false;
        }
        present.push_back(name);
    }
    return true;
}

UpdateOutcome Updater::supervise(const Trial& trial) {
    std::string error;
    int baseline = 0;
    bool failed = false;
    const bool watched = env_.service && env_.service(baseline, failed);
    const int64_t deadline = env_.now_ms() + env_.trial_ms;
    while (true) {
        env_.sleep_ms(env_.poll_ms);
        std::string confirmation;
        bool missing = false;
        if (read_file_at(state_.get(), kUpdateCommitFile, 64, uid_, confirmation, missing, error) &&
            trimmed(confirmation) == trial.new_version) {
            if (!commit(trial, error)) return done(UpdateOutcome::Result::Failed, "failed", trial.new_version, error);
            return done(UpdateOutcome::Result::Updated, "updated", trial.new_version,
                        "Updated to " + trial.new_version + "." + keep_note_);
        }
        int restarts = 0;
        std::string why;
        if (watched && env_.service(restarts, failed) && (failed || restarts - baseline >= env_.restart_limit)) {
            why = "the new version kept stopping";
        } else if (env_.now_ms() >= deadline) {
            why = "the new version did not say it works within " +
                  (env_.trial_ms >= 120000 ? std::to_string(env_.trial_ms / 60000) + " minutes"
                                           : std::to_string(env_.trial_ms / 1000) + " seconds");
        }
        if (!why.empty()) {
            std::string note;
            if (!roll_back(trial, trial.old_version, true, true, note, error)) {
                return done(UpdateOutcome::Result::Failed, "failed", trial.new_version, error);
            }
            return done(UpdateOutcome::Result::RolledBack, "rolled-back", trial.new_version,
                        "Went back to " + trial.old_version + ": " + why + "." + note);
        }
    }
}

UpdateOutcome Updater::update() {
    std::string error;
    if (!open(error)) return {UpdateOutcome::Result::Failed, error};
    // What the receiver asked for, read as a file of the receiver's user and
    // removed before anything else happens: a request that stayed would wake
    // the updater again and again, whatever went wrong below.
    std::string request;
    bool missing = false;
    const bool read = read_file_at(state_.get(), kUpdateRequestFile, kRequestBytes, uid_, request, missing, error);
    std::string ignored;
    remove_file_at(state_.get(), kUpdateRequestFile, ignored);
    UpdateOutcome recovered;
    if (!recover(true, recovered)) return recovered;
    if (!read) {
        if (missing) return recovered;
        return done(UpdateOutcome::Result::Refused, "refused", "", "The update request was refused: " + error);
    }
    // Exactly one of the two lines and nothing else; anything near them is
    // a version request, which it then fails to be.
    if (request == std::string(kAutostartOn) + "\n" || request == std::string(kAutostartOff) + "\n") {
        return switch_autostart(request == std::string(kAutostartOn) + "\n");
    }
    const std::string version = trimmed(request);
    if (!valid_version(version)) {
        return done(UpdateOutcome::Result::Refused, "refused", "", "The update request does not name a version.");
    }
    std::string current_target, trusted_target;
    if (!read_link_at(install_.get(), "current", current_target) ||
        !read_link_at(install_.get(), "trusted", trusted_target) || current_target != trusted_target ||
        version_of_target(trusted_target).empty()) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The install is not in a state to update from: current and trusted differ.");
    }
    // The version to go back to is the one the link names, which has to be
    // this program: a build whose version was not raised would otherwise
    // roll back to a directory that is not there.
    const std::string trusted = version_of_target(trusted_target);
    if (trusted != env_.running_version) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The trusted version is " + trusted + ", but the program updating is " + env_.running_version +
                        ".");
    }
    int order = 0;
    if (!compare_versions(version, env_.running_version, order) || order <= 0) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "This receiver runs " + env_.running_version + ", which is not older than " + version + ".");
    }

    status("checking", version, "Fetching the release of " + version + ".");
    std::string manifest_text, signature;
    if (!env_.fetch(env_.base_url + kReleaseManifestName, 64 * 1024, manifest_text, error) ||
        !env_.fetch(env_.base_url + kReleaseSignatureName, 64, signature, error)) {
        return done(UpdateOutcome::Result::Failed, "failed", version, "The release could not be fetched: " + error);
    }
    ReleaseManifest manifest;
    if (!verify_release_manifest(manifest_text, signature, env_.keys, manifest, error)) {
        return done(UpdateOutcome::Result::Refused, "refused", version, "The release was refused: " + error + ".");
    }
    if (manifest.version != version) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The release offered is " + manifest.version + ", not " + version + ".");
    }
    if (manifest.channel != "stable") {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The release offered is a " + manifest.channel + " one.");
    }
    const ReleaseAsset* asset = manifest.asset_for(env_.platform);
    if (!asset) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The release has no archive for " + (env_.platform.empty() ? "this machine" : env_.platform) + ".");
    }

    status("downloading", version, "Downloading " + asset->file + ".");
    std::string archive;
    if (!env_.fetch(env_.base_url + asset->file, asset->size, archive, error)) {
        return done(UpdateOutcome::Result::Failed, "failed", version, "The release could not be downloaded: " + error);
    }
    if (archive.size() != asset->size || sha256_hex(archive) != asset->sha256) {
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The download is not the archive the signed release names.");
    }

    status("installing", version, "Unpacking " + version + ".");
    if (!install_release(releases_.get(), version, archive, error)) {
        return done(UpdateOutcome::Result::Failed, "failed", version, error);
    }
    const std::string directory = layout_.install + "/" + kReleases + "/" + version;
    std::string output;
    if (!env_.check(directory + "/fernsdr", layout_.state + "/fernsdr.conf", directory + "/web", uid_, gid_, output)) {
        remove_tree_at(releases_.get(), version, ignored);
        return done(UpdateOutcome::Result::Refused, "refused", version,
                    "The new version refuses this receiver's configuration: " + output);
    }
    // In a container, the release goes into the volume as it was published
    // before anything switches: a container made again during the trial,
    // or after it, finds it there to check and unpack.
    if (layout_.keep_releases &&
        !keep_release(update_.get(), version, manifest_text, signature, asset->file, archive, error)) {
        remove_tree_at(releases_.get(), version, ignored);
        return done(UpdateOutcome::Result::Failed, "failed", version,
                    "The new version could not be kept in the volume: " + error);
    }

    Trial trial{trusted, version, env_.now_ms(), {}};
    if (!take_snapshot(trial.present, error) ||
        !write_file_at(update_.get(), kUpdateTrialFile, format_trial(trial), 0644, static_cast<uid_t>(-1),
                       static_cast<gid_t>(-1), error)) {
        remove_tree_at(releases_.get(), version, ignored);
        return done(UpdateOutcome::Result::Failed, "failed", version, error);
    }
    remove_file_at(state_.get(), kUpdateCommitFile, ignored);
    std::string note;
    if (!replace_link_at(install_.get(), "current", std::string(kReleases) + "/" + version, error)) {
        roll_back(trial, trial.old_version, true, false, note, ignored);
        return done(UpdateOutcome::Result::Failed, "failed", version, error);
    }
    status("trial", version, "Starting " + version + " and waiting for it to say it works.");
    std::string why;
    if (!env_.restart(why)) {
        roll_back(trial, trial.old_version, true, true, note, ignored);
        return done(UpdateOutcome::Result::RolledBack, "rolled-back", version,
                    "Went back to " + trial.old_version + ": the service did not restart (" + why + ").");
    }
    return supervise(trial);
}

UpdateOutcome Updater::switch_autostart(bool on) {
    std::string why;
    bool ok = false;
    if (env_.autostart) {
        ok = env_.autostart(on, why);
    } else {
        why = "this updater cannot switch it";
    }
    const std::string message =
        ok ? (on ? "FernSDR starts with the computer from now on."
                 : "FernSDR no longer starts with the computer. It keeps running until it is stopped.")
           : std::string("Starting with the computer could not be switched ") + (on ? "on" : "off") + ": " + why + ".";
    Json out = Json::make_object();
    out.set("time", static_cast<double>(env_.now_ms() / 1000));
    out.set("ok", ok);
    out.set("enabled", on);
    out.set("message", message);
    std::string error;
    // Like status.json: read by the panel, and only shown.
    write_file_at(update_.get(), kAutostartResultFile, out.serialize() + "\n", 0644, static_cast<uid_t>(-1),
                  static_cast<gid_t>(-1), error);
    return {ok ? UpdateOutcome::Result::Nothing : UpdateOutcome::Result::Refused, message};
}

UpdateOutcome Updater::boot() {
    std::string error;
    if (!open(error)) return {UpdateOutcome::Result::Failed, error};
    UpdateOutcome outcome;
    recover(false, outcome);
    return outcome;
}

}  // namespace

bool install_release(int releases, const std::string& version, const std::string& archive, std::string& error) {
    std::vector<UstarEntry> entries;
    if (!read_ustar(archive, entries, error)) {
        error = "the release archive cannot be unpacked: " + error;
        return false;
    }
    const auto has = [&](const std::string& path, bool executable) {
        return std::any_of(entries.begin(), entries.end(), [&](const UstarEntry& e) {
            return e.path == path && !e.directory && (!executable || e.executable);
        });
    };
    if (!has("fernsdr", true) || !has("web/index.html", false)) {
        error = "the release archive does not hold the receiver and its pages";
        return false;
    }
    uint64_t free = 0;
    if (!free_bytes(releases, free) || free < archive.size() + kSpareBytes) {
        error = "there is not enough room on the disk to unpack the new version";
        return false;
    }
    std::string staging;
    for (int attempt = 0; attempt < 8; attempt++) {
        staging = std::string(kStagingPrefix) + random_hex(6);
        if (::mkdirat(releases, staging.c_str(), 0700) == 0) break;
        if (errno != EEXIST) {
            error = std::string("cannot make a directory to unpack into: ") + std::strerror(errno);
            return false;
        }
        staging.clear();
    }
    Fd into(staging.empty() ? -1 : open_directory_at(releases, staging));
    if (into.get() < 0) {
        error = "cannot open the directory to unpack into";
        return false;
    }
    std::string ignored;
    if (!extract_ustar(archive, entries, into.get(), error) || ::fchmod(into.get(), 0755) != 0) {
        error = "cannot unpack the new version: " + error;
        remove_tree_at(releases, staging, ignored);
        return false;
    }
    // A directory of this version from an attempt that did not finish; it
    // is neither current nor trusted, which the caller made sure of.
    remove_tree_at(releases, version, ignored);
    if (::renameat(releases, staging.c_str(), releases, version.c_str()) != 0 ||
        !sync_directory(releases)) {
        error = std::string("cannot put the new version in place: ") + std::strerror(errno);
        remove_tree_at(releases, staging, ignored);
        return false;
    }
    return true;
}

bool read_trial_versions(int update, std::string& old_version, std::string& new_version) {
    std::string text, error;
    bool missing = false;
    Trial trial;
    if (!read_file_at(update, kUpdateTrialFile, kTrialBytes, ::geteuid(), text, missing, error) ||
        !parse_trial(text, trial)) {
        return false;
    }
    old_version = trial.old_version;
    new_version = trial.new_version;
    return true;
}

UpdateOutcome run_update(const UpdateLayout& layout, const UpdateEnvironment& environment) {
    Updater updater(layout, environment);
    return updater.update();
}

UpdateOutcome run_boot_check(const UpdateLayout& layout, const UpdateEnvironment& environment) {
    Updater updater(layout, environment);
    return updater.boot();
}

UpdateTrial::UpdateTrial(std::string state_directory, std::string update_directory, std::string version)
    : state_(std::move(state_directory)), version_(std::move(version)) {
    Fd update(open_directory(update_directory));
    Fd state(open_directory(state_));
    if (update.get() < 0 || state.get() < 0) return;
    std::string text, error;
    bool missing = false;
    Trial trial;
    if (!read_file_at(update.get(), kUpdateTrialFile, kTrialBytes, static_cast<uid_t>(-1), text, missing, error) ||
        !parse_trial(text, trial) || trial.new_version != version_) {
        return;
    }
    on_trial_ = true;
    std::string bands;
    if (read_file_at(state.get(), kUpdateBandsFile, 64 * 1024, ::geteuid(), bands, missing, error)) {
        size_t at = 0;
        while (at < bands.size()) {
            size_t end = bands.find('\n', at);
            if (end == std::string::npos) end = bands.size();
            if (end > at) bands_.push_back(bands.substr(at, end - at));
            at = end + 1;
        }
    }
}

bool UpdateTrial::tick(int64_t now_ms, const std::vector<std::string>& running_bands, int64_t settle_ms) {
    if (!on_trial_ || committed_) return committed_;
    if (started_ms_ < 0) started_ms_ = now_ms;
    if (now_ms - started_ms_ < settle_ms) return false;
    for (const std::string& band : bands_) {
        if (std::find(running_bands.begin(), running_bands.end(), band) == running_bands.end()) return false;
    }
    Fd state(open_directory(state_));
    std::string error;
    if (state.get() < 0 || !write_file_at(state.get(), kUpdateCommitFile, version_ + "\n", 0600, static_cast<uid_t>(-1),
                                          static_cast<gid_t>(-1), error)) {
        return false;
    }
    committed_ = true;
    return true;
}

bool request_update(const std::string& state_directory, const std::string& version,
                    const std::vector<std::string>& running_bands, std::string& error) {
    int order = 0;
    if (!compare_versions(version, version, order)) {
        error = "not a version: " + version;
        return false;
    }
    Fd state(open_directory(state_directory));
    if (state.get() < 0) {
        error = "cannot open " + state_directory;
        return false;
    }
    std::string bands;
    for (const std::string& band : running_bands) bands += band + "\n";
    return write_file_at(state.get(), kUpdateBandsFile, bands, 0600, static_cast<uid_t>(-1), static_cast<gid_t>(-1),
                         error) &&
           write_file_at(state.get(), kUpdateRequestFile, version + "\n", 0600, static_cast<uid_t>(-1),
                         static_cast<gid_t>(-1), error);
}

}  // namespace fernsdr
