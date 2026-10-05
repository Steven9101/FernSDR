// The container image's start: which release runs, from what the volume
// keeps and what the image brings, in directories that stand in for
// /opt/fernsdr, the image's tree and the volume.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../src/update/container.h"
#include "../src/update/files.h"
#include "../src/update/release.h"
#include "../src/update/ustar.h"
#include "../src/util/ed25519.h"
#include "../src/util/password.h"
#include "test_util.h"

namespace {

std::string sha256_hex(const std::string& data) {
    fernsdr::Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return fernsdr::to_hex(digest, 32);
}

void put(const std::string& path, const std::string& text, mode_t mode = 0644) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return;
    if (::write(fd, text.data(), text.size()) < 0) {}
    ::close(fd);
    ::chmod(path.c_str(), mode);
}

std::string contents_of(const std::string& path) {
    std::string out;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return "<missing>";
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

bool exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

std::string link_of(const std::string& path) {
    char buffer[256];
    const ssize_t n = ::readlink(path.c_str(), buffer, sizeof(buffer));
    return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "";
}

bool said(const std::vector<std::string>& report, const std::string& text) {
    for (const std::string& line : report)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

// A container: the image's 0.1.2 as a tree, an empty /opt/fernsdr and a
// volume with a configuration.
struct Container {
    std::string root;
    fernsdr::ContainerSetup setup;
    uint8_t seed[32];

    Container() {
        char name[] = "/tmp/fernsdr-container-XXXXXX";
        root = ::mkdtemp(name) ? name : "";
        for (const std::string& dir : {"/opt", "/image", "/image/web", "/volume"})
            ::mkdir((root + dir).c_str(), 0755);
        ::chmod((root + "/volume").c_str(), 0700);
        put(root + "/image/fernsdr", "program 0.1.2", 0755);
        put(root + "/image/web/index.html", "page 0.1.2");
        put(root + "/volume/fernsdr.conf", "[site]\nname = here\n", 0600);
        setup.layout.install = root + "/opt";
        setup.layout.state = root + "/volume";
        setup.layout.update = root + "/volume/update";
        setup.layout.keep_releases = true;
        setup.image = root + "/image";
        setup.image_version = "0.1.2";
        setup.platform = "linux-x86_64";
        setup.state_may_be_this_users = true;
        for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(7 + i);
        fernsdr::ReleaseKey key;
        fernsdr::ed25519_public_key(seed, key.data());
        setup.keys = {key};
    }

    ~Container() {
        const int top = fernsdr::open_directory("/tmp");
        std::string error;
        if (top >= 0) fernsdr::remove_tree_at(top, root.substr(5), error);
        if (top >= 0) ::close(top);
    }

    bool prepare(std::vector<std::string>& report) {
        std::string error;
        const bool ok = fernsdr::prepare_container(setup, report, error);
        if (!ok) fprintf(stderr, "    prepare: %s\n", error.c_str());
        return ok;
    }

    // Keeps `version` in the volume as the updater would, signed with this
    // container's key unless `other_key`.
    void keep(const std::string& version, bool trusted, bool other_key = false) {
        std::string archive, error, text;
        CHECK(fernsdr::write_ustar({{"fernsdr", false, true, "program " + version},
                                    {"web", true, false, ""},
                                    {"web/index.html", false, false, "page " + version}},
                                   0, archive, error));
        fernsdr::ReleaseManifest manifest;
        manifest.version = version;
        manifest.date = "2026-10-01";
        manifest.channel = "stable";
        const std::string file = "fernsdr-" + version + "-linux-x86_64.tar";
        manifest.assets.push_back({"linux-x86_64", file, archive.size(), sha256_hex(archive)});
        CHECK(fernsdr::format_release_manifest(manifest, text, error));
        uint8_t key[32];
        for (int i = 0; i < 32; i++) key[i] = static_cast<uint8_t>(other_key ? 99 - i : seed[i]);
        const std::string message = fernsdr::release_signed_message(text);
        uint8_t signature[64];
        fernsdr::ed25519_sign(key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature);
        ::mkdir(setup.layout.update.c_str(), 0755);
        const int update = fernsdr::open_directory(setup.layout.update);
        CHECK(fernsdr::keep_release(update, version, text, std::string(reinterpret_cast<char*>(signature), 64), file,
                                    archive, error));
        if (trusted) CHECK(fernsdr::keep_trusted(update, version, error));
        ::close(update);
    }

    std::string kept(const std::string& version) const {
        return setup.layout.update + "/release/" + version + "/fernsdr-" + version + "-linux-x86_64.tar";
    }
    std::string current() const { return link_of(root + "/opt/current"); }
    std::string trusted() const { return link_of(root + "/opt/trusted"); }
    std::string running() const { return contents_of(root + "/opt/" + current() + "/fernsdr"); }
};

}  // namespace

TEST_CASE(container_first_start_runs_the_images_release) {
    Container container;
    std::vector<std::string> report;
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.2");
    CHECK_EQ_STR(container.trusted(), "releases/0.1.2");
    CHECK_EQ_STR(container.running(), "program 0.1.2");
    CHECK_EQ_STR(contents_of(container.root + "/opt/releases/0.1.2/web/index.html"), "page 0.1.2");
    CHECK(said(report, "the image's own release"));
    // The update directory in the volume is root's alone.
    const int update = fernsdr::open_directory(container.setup.layout.update);
    CHECK(fernsdr::held_by_this_user(update));
    ::close(update);
    // A second start changes nothing.
    report.clear();
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.2");
}

TEST_CASE(container_runs_a_newer_release_the_volume_keeps_once_it_checks_out) {
    Container container;
    container.keep("0.1.3", true);
    std::vector<std::string> report;
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.3");
    CHECK_EQ_STR(container.trusted(), "releases/0.1.3");
    CHECK_EQ_STR(container.running(), "program 0.1.3");
    CHECK(said(report, "kept in the volume"));
    CHECK(exists(container.kept("0.1.3")));
    // The image's own release is not unpacked for nothing.
    CHECK(!exists(container.root + "/opt/releases/0.1.2"));
}

TEST_CASE(container_refuses_a_kept_release_that_is_not_what_was_signed) {
    // A byte of the archive changed, and a release signed by another key.
    for (const bool other_key : {false, true}) {
        Container container;
        container.keep("0.1.3", true, other_key);
        if (!other_key) {
            std::string archive = contents_of(container.kept("0.1.3"));
            archive[600] ^= 1;
            put(container.kept("0.1.3"), archive);
        }
        std::vector<std::string> report;
        CHECK(container.prepare(report));
        CHECK_EQ_STR(container.current(), "releases/0.1.2");
        CHECK_EQ_STR(container.trusted(), "releases/0.1.2");
        CHECK(said(report, "0.1.3 kept in the volume was refused"));
        // What was refused is not kept for next time either.
        CHECK(!exists(container.setup.layout.update + "/release/0.1.3"));
        CHECK(!exists(container.setup.layout.update + "/release/trusted"));
        CHECK(!exists(container.root + "/opt/releases/0.1.3"));
    }
}

TEST_CASE(container_refuses_a_kept_release_after_a_restart_too) {
    // The release ran in this container already; its copy in the volume is
    // changed while it was down. The next start goes back to the image's.
    Container container;
    container.keep("0.1.3", true);
    std::vector<std::string> report;
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.3");
    put(container.setup.layout.update + "/release/0.1.3/fernsdr-release-v1.txt", "fernsdr-release 1\n");
    report.clear();
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.2");
    CHECK_EQ_STR(container.running(), "program 0.1.2");
    CHECK(!exists(container.root + "/opt/releases/0.1.3"));
}

TEST_CASE(container_image_newer_than_the_volume_wins) {
    for (const char* kept : {"0.1.1", "0.1.2"}) {
        Container container;
        container.keep(kept, true);
        std::vector<std::string> report;
        CHECK(container.prepare(report));
        CHECK_EQ_STR(container.current(), "releases/0.1.2");
        CHECK_EQ_STR(container.running(), "program 0.1.2");
        CHECK(said(report, "is not older than"));
        // And it is what the volume says from now on.
        CHECK(!exists(container.setup.layout.update + "/release/trusted"));
        CHECK(!exists(container.setup.layout.update + "/release/" + kept));
    }
}

TEST_CASE(container_does_not_believe_an_update_directory_another_user_could_have_made) {
    // One anyone can write to, and one that is a link: both made again.
    Container writable;
    writable.keep("0.1.3", true);
    ::chmod(writable.setup.layout.update.c_str(), 0777);
    std::vector<std::string> report;
    CHECK(writable.prepare(report));
    CHECK_EQ_STR(writable.current(), "releases/0.1.2");
    CHECK(said(report, "was not root's alone"));
    CHECK(!exists(writable.setup.layout.update + "/release"));
    struct stat info {};
    CHECK(::lstat(writable.setup.layout.update.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
          (info.st_mode & 0777) == 0755);

    Container linked;
    ::mkdir((linked.root + "/elsewhere").c_str(), 0755);
    put(linked.root + "/elsewhere/keep-me", "x");
    CHECK(::symlink((linked.root + "/elsewhere").c_str(), linked.setup.layout.update.c_str()) == 0);
    report.clear();
    CHECK(linked.prepare(report));
    CHECK(said(report, "was not root's alone"));
    CHECK(::lstat(linked.setup.layout.update.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
    CHECK_EQ_STR(contents_of(linked.root + "/elsewhere/keep-me"), "x");

    // As root: a directory of the receiver's, as it could make by renaming
    // the real one away.
    if (::geteuid() != 0) return;
    Container planted;
    planted.keep("0.1.3", true);
    CHECK(::chown(planted.setup.layout.update.c_str(), 65534, 65534) == 0);
    report.clear();
    CHECK(planted.prepare(report));
    CHECK_EQ_STR(planted.current(), "releases/0.1.2");
    CHECK(said(report, "was not root's alone"));
}

TEST_CASE(container_leaves_a_trial_to_the_boot_check_only_when_the_volume_vouches_for_it) {
    // The container went down while 0.1.4 was on trial after 0.1.3.
    const auto trial = [](Container& container) {
        container.keep("0.1.3", true);
        container.keep("0.1.4", false);
        const int update = fernsdr::open_directory(container.setup.layout.update);
        std::string error;
        CHECK(fernsdr::write_file_at(update, "trial", "old 0.1.3\nnew 0.1.4\nstarted 5\npresent fernsdr.conf\n", 0644,
                                     static_cast<uid_t>(-1), static_cast<gid_t>(-1), error));
        ::close(update);
    };
    Container vouched;
    trial(vouched);
    std::vector<std::string> report;
    CHECK(vouched.prepare(report));
    CHECK_EQ_STR(vouched.trusted(), "releases/0.1.3");
    CHECK_EQ_STR(vouched.current(), "releases/0.1.4");
    CHECK_EQ_STR(vouched.running(), "program 0.1.4");
    CHECK(exists(vouched.kept("0.1.4")));

    Container spoiled;
    trial(spoiled);
    std::string archive = contents_of(spoiled.kept("0.1.4"));
    archive[600] ^= 1;
    put(spoiled.kept("0.1.4"), archive);
    report.clear();
    CHECK(spoiled.prepare(report));
    CHECK_EQ_STR(spoiled.trusted(), "releases/0.1.3");
    CHECK_EQ_STR(spoiled.current(), "releases/0.1.3");
    CHECK(!exists(spoiled.root + "/opt/releases/0.1.4"));
    CHECK(said(report, "ends with 0.1.3"));
}

TEST_CASE(container_refuses_a_volume_of_roots_and_an_image_with_links) {
    Container roots;
    roots.setup.state_may_be_this_users = false;
    std::vector<std::string> report;
    std::string error;
    CHECK(!fernsdr::prepare_container(roots.setup, report, error));
    CHECK(error.find("belongs to root") != std::string::npos);

    Container linked;
    CHECK(::symlink("/etc/passwd", (linked.root + "/image/web/passwd").c_str()) == 0);
    CHECK(!fernsdr::prepare_container(linked.setup, report, error));
    CHECK(error.find("neither a file nor a directory") != std::string::npos);
    CHECK(!exists(linked.root + "/opt/current"));
}

TEST_CASE(container_makes_anew_an_update_directory_brought_back_while_it_ran) {
    Container container;
    std::vector<std::string> report;
    CHECK(container.prepare(report));
    // While it ran, the receiver renamed the update directory away and put
    // back another of root's, here one keeping a newer signed release.
    const std::string away = container.root + "/volume/update.away";
    CHECK(::rename(container.setup.layout.update.c_str(), away.c_str()) == 0);
    container.keep("0.1.3", true);
    report.clear();
    CHECK(container.prepare(report));
    CHECK(said(report, "was replaced while the container ran"));
    CHECK_EQ_STR(container.current(), "releases/0.1.2");
    CHECK(!exists(container.setup.layout.update + "/release"));
    // A container made again knows nothing of it, and takes the directory
    // it finds, if it is root's alone.
    ::unlink((container.root + "/opt/update-directory").c_str());
    container.keep("0.1.3", true);
    report.clear();
    CHECK(container.prepare(report));
    CHECK_EQ_STR(container.current(), "releases/0.1.3");
}
