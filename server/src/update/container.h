// Updates inside the container image, without the Docker socket: the same
// signed releases and the same updater as on a machine, run by `fernsdr
// --supervise` as root inside the container.
//
// The container has one volume, /var/lib/fernsdr, which the receiver's user
// owns. Programs never run from it. /opt/fernsdr, in the container's own
// file system, holds releases/, current and trusted as install.sh lays them
// out, and the image's own release as a tree in /opt/fernsdr/image. What
// survives the container being made again is kept in the volume, in a
// directory of root's, /var/lib/fernsdr/update: the updater's records, and
// the trusted release exactly as it was published (manifest, signature,
// archive) in release/<version>, with release/trusted naming it.
//
// The receiver owns the volume's top directory, so it can rename `update`
// away and put a directory of its own there, or bring back an older one of
// root's. Nothing read from the volume is therefore believed on the strength
// of where it is: a directory has to be root's alone once opened, and a kept
// release is unpacked only after its manifest's signature, checked with the
// keys in the image's own program, and the archive's hash hold. What such a
// receiver can still do is have the next container fall back to the image's
// release, or start a signed release older than the one it ran but not older
// than the image's (see DEPLOYMENT.md, "In a container").
#pragma once
#include <string>
#include <vector>

#include "release.h"
#include "updater.h"

namespace fernsdr {

// The image's own release, and the update directory's name in the volume.
constexpr const char* kContainerImageDirectory = "/opt/fernsdr/image";
constexpr const char* kContainerUpdateName = "update";
// In the update directory: release/<version>/ and the link release/trusted.
constexpr const char* kKeptReleasesName = "release";
// The same random token, drawn at each start, in /opt/fernsdr, out of the
// receiver's reach, and in the update directory it set up: an older update
// directory of root's brought back carries an older token. A token rather
// than the directory's device and inode, which a host's reboot can change
// on btrfs, ZFS or LVM.
constexpr const char* kUpdateIdentityName = "update-directory";

UpdateLayout container_layout();

// As the updater, in a container: keeps the release it fetched in
// release/<version>, as published. False with the reason.
bool keep_release(int update, const std::string& version, const std::string& manifest_text,
                  const std::string& signature, const std::string& archive_name, const std::string& archive,
                  std::string& error);
// Makes the kept `version` the one a new container starts, and removes the
// others.
bool keep_trusted(int update, const std::string& version, std::string& error);
// Removes a kept version that was not kept after all.
void forget_release(int update, const std::string& version);

// Draws a new token and writes it in `update`, then in `install`.
bool record_update_directory(int install, int update, std::string& error);
// Whether `update` is the update directory this container's start recorded
// in `install`. While the container runs, the receiver could rename it away
// and bring back an older one of root's; the updater works only in the one
// the start set up.
bool update_directory_recorded(int install, int update, std::string& error);

// Reads the kept `version` and checks it: the manifest's signature by one of
// `keys`, the manifest naming `version` on the stable channel, and the
// archive for `platform` being exactly what the manifest names.
bool read_kept_release(int update, const std::string& version, const std::vector<ReleaseKey>& keys,
                       const std::string& platform, std::string& archive, std::string& error);

struct ContainerSetup {
    UpdateLayout layout;        // container_layout(), or a test's
    std::string image;          // the image's release, as a tree
    std::string image_version;  // its version: this program's
    std::vector<ReleaseKey> keys;
    std::string platform;
    // The receiver never runs as root, so a volume of root's is refused;
    // only a test, whose volume is its own, sets this.
    bool state_may_be_this_users = false;
};

// As root, each time the container starts and before the supervisor does:
// makes the update directory in the volume root's if it is not, picks the
// release to run (the kept trusted one if it checks out and is newer than
// the image's, else the image's), unpacks it into /opt/fernsdr/releases
// where it is not there yet, points `trusted` and `current` at it, and
// removes what neither the supervisor's boot check nor a later rollback
// needs. An update directory other than the one the last start recorded,
// in a container that only restarted, was put there while it ran, and is
// made anew. A trial the container went down in is left for the boot check,
// with its new version unpacked only if it too checks out. `report` gets a
// line for the log for each decision. False, with the reason, when the
// container cannot start at all.
bool prepare_container(const ContainerSetup& setup, std::vector<std::string>& report, std::string& error);

}  // namespace fernsdr
