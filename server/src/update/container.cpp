#include "container.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "../util/password.h"
#include "files.h"
#include "ustar.h"

namespace fernsdr {

namespace {

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

// a > b, for two versions known to be valid.
bool newer(const std::string& a, const std::string& b) {
    int order = 0;
    return compare_versions(a, b, order) && order > 0;
}

std::string sha256_hex(const std::string& data) {
    Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, sizeof(digest));
}

bool present_at(int dir, const std::string& name) {
    struct stat info {};
    return ::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0;
}

// `name` in `dir` as a directory of this user's alone, made if it is not
// there. Anything else in its place, which only another user can have put
// there, is removed first, following no link.
int own_directory_at(int dir, const std::string& name, std::string& removed, std::string& error) {
    Fd sub(open_directory_at(dir, name));
    if (sub.get() >= 0 && held_by_this_user(sub.get())) return ::fcntl(sub.get(), F_DUPFD_CLOEXEC, 0);
    sub.reset();
    if (present_at(dir, name)) {
        if (!remove_tree_at(dir, name, error)) return -1;
        removed = name;
    }
    if (::mkdirat(dir, name.c_str(), 0755) != 0) {
        error = "cannot make " + name + ": " + std::strerror(errno);
        return -1;
    }
    sub.reset(open_directory_at(dir, name));
    if (sub.get() < 0 || ::fchmod(sub.get(), 0755) != 0 || !held_by_this_user(sub.get())) {
        error = name + " is not this user's alone even when just made";
        return -1;
    }
    const int fd = ::fcntl(sub.get(), F_DUPFD_CLOEXEC, 0);
    if (fd < 0) error = std::string("cannot keep ") + name + " open: " + std::strerror(errno);
    return fd;
}

// The kept releases' directory, or -1 when there is none that is this
// user's alone.
int kept_directory(int update) {
    Fd kept(open_directory_at(update, kKeptReleasesName));
    if (kept.get() < 0 || !held_by_this_user(kept.get())) return -1;
    return ::fcntl(kept.get(), F_DUPFD_CLOEXEC, 0);
}

// Every file and directory below `dir`, as the archive it would be. The
// image's tree holds nothing else; a link or a device is refused.
bool collect_tree(int dir, const std::string& relative, int depth, std::vector<UstarInput>& out, std::string& error) {
    if (depth > 16) {
        error = "the image's release is nested too deep";
        return false;
    }
    std::vector<std::string> names;
    {
        const int copy = ::fcntl(dir, F_DUPFD_CLOEXEC, 0);
        DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr;
        if (!listing) {
            if (copy >= 0) ::close(copy);
            error = "cannot list the image's release";
            return false;
        }
        while (const dirent* entry = ::readdir(listing)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        ::closedir(listing);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string path = relative.empty() ? name : relative + "/" + name;
        struct stat info {};
        if (::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            error = "cannot look at " + path + " in the image's release";
            return false;
        }
        if (S_ISDIR(info.st_mode)) {
            out.push_back({path, true, false, ""});
            Fd sub(open_directory_at(dir, name));
            if (sub.get() < 0 || !collect_tree(sub.get(), path, depth + 1, out, error)) {
                if (error.empty()) error = "cannot open " + path + " in the image's release";
                return false;
            }
        } else if (S_ISREG(info.st_mode)) {
            UstarInput file{path, false, (info.st_mode & S_IXUSR) != 0, ""};
            bool missing = false;
            if (!read_file_at(dir, name, 256u << 20, static_cast<uid_t>(-1), file.contents, missing, error)) {
                return false;
            }
            out.push_back(std::move(file));
        } else {
            error = path + " in the image's release is neither a file nor a directory";
            return false;
        }
    }
    return true;
}

}  // namespace

bool record_update_directory(int install, int update, std::string& error) {
    const std::string token = random_hex(16);
    if (token.empty()) {
        error = "no source of randomness for the update directory's token";
        return false;
    }
    const uid_t me = static_cast<uid_t>(-1);
    const gid_t group = static_cast<gid_t>(-1);
    return write_file_at(update, kUpdateIdentityName, token + "\n", 0644, me, group, error) &&
           write_file_at(install, kUpdateIdentityName, token + "\n", 0644, me, group, error);
}

bool update_directory_recorded(int install, int update, std::string& error) {
    std::string recorded, found;
    bool missing = false;
    if (!read_file_at(install, kUpdateIdentityName, 64, ::geteuid(), recorded, missing, error)) {
        error = "the container's start recorded no update directory: " + error;
        return false;
    }
    if (!read_file_at(update, kUpdateIdentityName, 64, ::geteuid(), found, missing, error) || found != recorded) {
        error = "the update directory is not the one this container's start set up";
        return false;
    }
    return true;
}

UpdateLayout container_layout() {
    UpdateLayout layout;
    layout.install = "/opt/fernsdr";
    layout.state = "/var/lib/fernsdr";
    layout.update = layout.state + "/" + kContainerUpdateName;
    layout.keep_releases = true;
    return layout;
}

bool keep_release(int update, const std::string& version, const std::string& manifest_text,
                  const std::string& signature, const std::string& archive_name, const std::string& archive,
                  std::string& error) {
    std::string removed;
    Fd kept(own_directory_at(update, kKeptReleasesName, removed, error));
    if (kept.get() < 0) return false;
    std::string ignored;
    remove_tree_at(kept.get(), version, ignored);
    if (::mkdirat(kept.get(), version.c_str(), 0755) != 0) {
        error = "cannot make a directory for " + version + ": " + std::strerror(errno);
        return false;
    }
    Fd into(open_directory_at(kept.get(), version));
    const uid_t me = static_cast<uid_t>(-1);
    const gid_t group = static_cast<gid_t>(-1);
    if (into.get() < 0 || !held_by_this_user(into.get()) ||
        !write_file_at(into.get(), kReleaseManifestName, manifest_text, 0644, me, group, error) ||
        !write_file_at(into.get(), kReleaseSignatureName, signature, 0644, me, group, error) ||
        !write_file_at(into.get(), archive_name, archive, 0644, me, group, error)) {
        if (error.empty()) error = "cannot open the directory for " + version;
        remove_tree_at(kept.get(), version, ignored);
        return false;
    }
    return true;
}

bool keep_trusted(int update, const std::string& version, std::string& error) {
    Fd kept(kept_directory(update));
    if (kept.get() < 0 || !present_at(kept.get(), version)) {
        error = version + " is not kept in " + std::string(kKeptReleasesName);
        return false;
    }
    if (!replace_link_at(kept.get(), "trusted", version, error)) return false;
    std::vector<std::string> names;
    const int copy = ::fcntl(kept.get(), F_DUPFD_CLOEXEC, 0);
    if (DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr) {
        while (const dirent* entry = ::readdir(listing)) names.push_back(entry->d_name);
        ::closedir(listing);
    } else if (copy >= 0) {
        ::close(copy);
    }
    std::string ignored;
    for (const std::string& name : names) {
        if (name != version && valid_version(name)) remove_tree_at(kept.get(), name, ignored);
    }
    return true;
}

void forget_release(int update, const std::string& version) {
    Fd kept(kept_directory(update));
    std::string target, ignored;
    // The trusted one stays, whatever the caller thought.
    if (kept.get() < 0 || (read_link_at(kept.get(), "trusted", target) && target == version)) return;
    remove_tree_at(kept.get(), version, ignored);
}

bool read_kept_release(int update, const std::string& version, const std::vector<ReleaseKey>& keys,
                       const std::string& platform, std::string& archive, std::string& error) {
    archive.clear();
    Fd kept(kept_directory(update));
    Fd release(kept.get() >= 0 && valid_version(version) ? open_directory_at(kept.get(), version) : -1);
    if (release.get() < 0 || !held_by_this_user(release.get())) {
        error = "it is not kept in a directory of root's alone";
        return false;
    }
    std::string manifest_text, signature;
    bool missing = false;
    const uid_t me = ::geteuid();
    if (!read_file_at(release.get(), kReleaseManifestName, 64 * 1024, me, manifest_text, missing, error) ||
        !read_file_at(release.get(), kReleaseSignatureName, 64, me, signature, missing, error)) {
        return false;
    }
    ReleaseManifest manifest;
    if (!verify_release_manifest(manifest_text, signature, keys, manifest, error)) return false;
    if (manifest.version != version) {
        error = "its signed manifest is for " + manifest.version;
        return false;
    }
    if (manifest.channel != "stable") {
        error = "it is a " + manifest.channel + " release";
        return false;
    }
    const ReleaseAsset* asset = manifest.asset_for(platform);
    if (!asset) {
        error = "its manifest has no archive for " + platform;
        return false;
    }
    if (!read_file_at(release.get(), asset->file, asset->size, me, archive, missing, error)) return false;
    if (archive.size() != asset->size || sha256_hex(archive) != asset->sha256) {
        archive.clear();
        error = "its archive is not the one its signed manifest names";
        return false;
    }
    return true;
}

bool prepare_container(const ContainerSetup& setup, std::vector<std::string>& report, std::string& error) {
    const UpdateLayout& layout = setup.layout;
    Fd state(open_directory(layout.state));
    struct stat info {};
    if (state.get() < 0 || ::fstat(state.get(), &info) != 0) {
        error = "cannot open " + layout.state;
        return false;
    }
    if (info.st_uid == ::geteuid() && !setup.state_may_be_this_users) {
        error = layout.state + " belongs to root; the receiver has to run as a user of its own";
        return false;
    }
    Fd install(open_directory(layout.install));
    if (install.get() < 0 || !held_by_this_user(install.get())) {
        error = layout.install + " is missing or not root's alone";
        return false;
    }
    // In the volume, beside the receiver's files: the directory it is in is
    // the receiver's, so whatever is found under the name is checked.
    const size_t slash = layout.update.rfind('/');
    Fd parent(slash == std::string::npos || slash == 0 ? -1 : open_directory(layout.update.substr(0, slash)));
    if (parent.get() < 0) {
        error = "cannot open the directory " + layout.update + " is in";
        return false;
    }
    const std::string update_name = layout.update.substr(slash + 1);
    std::string removed;
    Fd update(own_directory_at(parent.get(), update_name, removed, error));
    if (update.get() < 0) return false;
    if (!removed.empty()) {
        report.push_back(layout.update + " was not root's alone, and was removed: whatever was kept in it is not "
                         "believed.");
    }
    // Restarted rather than made again, this container knows which update
    // directory it set up; another one of root's was brought back while it
    // ran.
    std::string why;
    bool missing = false;
    std::string recorded;
    if (read_file_at(install.get(), kUpdateIdentityName, 64, ::geteuid(), recorded, missing, why) &&
        !update_directory_recorded(install.get(), update.get(), why)) {
        update.reset();
        if (!remove_tree_at(parent.get(), update_name, error)) return false;
        update.reset(own_directory_at(parent.get(), update_name, removed, error));
        if (update.get() < 0) return false;
        report.push_back(layout.update + " was replaced while the container ran, and was made anew: whatever was "
                         "kept in it is not believed.");
    }
    if (!record_update_directory(install.get(), update.get(), error)) return false;
    if (!present_at(install.get(), "releases") && ::mkdirat(install.get(), "releases", 0755) != 0) {
        error = "cannot make " + layout.install + "/releases: " + std::strerror(errno);
        return false;
    }
    Fd releases(open_directory_at(install.get(), "releases"));
    if (releases.get() < 0 || !held_by_this_user(releases.get())) {
        error = layout.install + "/releases is not root's alone";
        return false;
    }

    // The release kept in the volume, if one is kept and checks out.
    std::string kept_version, kept_archive;
    {
        Fd kept(kept_directory(update.get()));
        std::string named, refused;
        if (kept.get() >= 0 && read_link_at(kept.get(), "trusted", named)) {
            if (!valid_version(named)) {
                report.push_back("The volume names no release that could be checked; the image's " +
                                 setup.image_version + " runs.");
            } else if (read_kept_release(update.get(), named, setup.keys, setup.platform, kept_archive, refused)) {
                kept_version = named;
            } else {
                report.push_back("The release " + named + " kept in the volume was refused: " + refused +
                                 ". The image's " + setup.image_version + " runs instead.");
            }
        }
    }
    std::string target = setup.image_version;
    if (!kept_version.empty() && newer(kept_version, setup.image_version)) {
        target = kept_version;
    } else if (!kept_version.empty()) {
        report.push_back("The image's " + setup.image_version + " is not older than " + kept_version +
                         ", kept in the volume, and replaces it.");
        kept_version.clear();
    }
    if (!present_at(releases.get(), target)) {
        std::string archive;
        if (target == setup.image_version) {
            Fd image(open_directory(setup.image));
            std::vector<UstarInput> inputs;
            if (image.get() < 0 || !collect_tree(image.get(), "", 0, inputs, error) ||
                !write_ustar(inputs, 0, archive, error)) {
                if (error.empty()) error = "cannot open " + setup.image;
                error = "the image's own release cannot be read: " + error;
                return false;
            }
        } else {
            archive = kept_archive;
        }
        if (!install_release(releases.get(), target, archive, error)) return false;
    }

    // A trial the container went down in: its new version stays only if
    // the volume vouches for it, so that the boot check can keep it.
    std::string trial_old, trial_new, running = target;
    if (read_trial_versions(update.get(), trial_old, trial_new) && trial_new != target) {
        std::string archive, why;
        if (newer(trial_new, target) &&
            read_kept_release(update.get(), trial_new, setup.keys, setup.platform, archive, why) &&
            (present_at(releases.get(), trial_new) || install_release(releases.get(), trial_new, archive, why))) {
            running = trial_new;
        } else {
            std::string ignored;
            remove_tree_at(releases.get(), trial_new, ignored);
            report.push_back("The update to " + trial_new + " that was on trial ends with " + target + ".");
        }
    }

    const std::string prefix = "releases/";
    if (!replace_link_at(install.get(), "trusted", prefix + target, error) ||
        !replace_link_at(install.get(), "current", prefix + running, error)) {
        return false;
    }
    // What nothing points at any more, in the container and in the volume.
    std::vector<std::string> names;
    {
        const int copy = ::fcntl(releases.get(), F_DUPFD_CLOEXEC, 0);
        if (DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr) {
            while (const dirent* entry = ::readdir(listing)) names.push_back(entry->d_name);
            ::closedir(listing);
        } else if (copy >= 0) {
            ::close(copy);
        }
    }
    std::string ignored;
    for (const std::string& name : names) {
        if (name == target || name == running) continue;
        if (valid_version(name) || name.rfind(".staging-", 0) == 0) remove_tree_at(releases.get(), name, ignored);
    }
    if (Fd kept(kept_directory(update.get())); kept.get() >= 0) {
        names.clear();
        const int copy = ::fcntl(kept.get(), F_DUPFD_CLOEXEC, 0);
        if (DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr) {
            while (const dirent* entry = ::readdir(listing)) names.push_back(entry->d_name);
            ::closedir(listing);
        } else if (copy >= 0) {
            ::close(copy);
        }
        if (kept_version.empty()) remove_file_at(kept.get(), "trusted", ignored);
        for (const std::string& name : names) {
            // The kept trusted release, and the one on trial if it stays.
            if (name == kept_version || (running != target && name == running) || name == "." || name == "..") continue;
            if (name != "trusted") remove_tree_at(kept.get(), name, ignored);
        }
    } else if (present_at(update.get(), kKeptReleasesName)) {
        remove_tree_at(update.get(), kKeptReleasesName, ignored);
    }
    report.push_back(target == setup.image_version
                         ? "Running " + target + ", the image's own release."
                         : "Running " + target + ", kept in the volume and checked against its signature.");
    return true;
}

}  // namespace fernsdr
