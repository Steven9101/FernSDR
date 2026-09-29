// The updater's file handling, where another user may have planted a link,
// a second name or a FIFO: each is refused or replaced, never followed.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "../src/update/extract.h"
#include "../src/update/files.h"
#include "../src/update/ustar.h"
#include "test_util.h"

namespace {

struct Scratch {
    std::string path;
    int fd = -1;
    Scratch() {
        char name[] = "/tmp/fernsdr-update-files-XXXXXX";
        if (::mkdtemp(name)) path = name;
        fd = fernsdr::open_directory(path);
    }
    ~Scratch() {
        if (fd >= 0) ::close(fd);
        const size_t slash = path.rfind('/');
        const int parent = fernsdr::open_directory(path.substr(0, slash));
        std::string error;
        if (parent >= 0) fernsdr::remove_tree_at(parent, path.substr(slash + 1), error);
        if (parent >= 0) ::close(parent);
    }
    std::string at(const std::string& name) const { return path + "/" + name; }
};

void put(const std::string& path, const std::string& text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (::write(fd, text.data(), text.size()) < 0) {}
    ::close(fd);
}

std::string contents_of(const std::string& path) {
    std::string out;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return "<missing>";
    char buffer[256];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

mode_t mode_of(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 ? (info.st_mode & 07777) : 0;
}

}  // namespace

TEST_CASE(update_files_write_replaces_a_link_instead_of_following_it) {
    Scratch dir;
    CHECK(dir.fd >= 0);
    put(dir.at("elsewhere"), "someone else's");
    CHECK(::symlink(dir.at("elsewhere").c_str(), dir.at("fernsdr.conf").c_str()) == 0);
    std::string error;
    CHECK(fernsdr::write_file_at(dir.fd, "fernsdr.conf", "restored", 0600, static_cast<uid_t>(-1),
                                 static_cast<gid_t>(-1), error));
    CHECK_EQ_STR(contents_of(dir.at("elsewhere")), "someone else's");
    CHECK_EQ_STR(contents_of(dir.at("fernsdr.conf")), "restored");
    CHECK_EQ(mode_of(dir.at("fernsdr.conf")), 0600u);
    struct stat info {};
    CHECK(::lstat(dir.at("fernsdr.conf").c_str(), &info) == 0 && S_ISREG(info.st_mode));
    // Written again: replaced whole, nothing left beside it.
    CHECK(fernsdr::write_file_at(dir.fd, "fernsdr.conf", "again", 0644, static_cast<uid_t>(-1),
                                 static_cast<gid_t>(-1), error));
    CHECK_EQ_STR(contents_of(dir.at("fernsdr.conf")), "again");
    CHECK(!fernsdr::write_file_at(dir.fd, "../escape", "x", 0644, static_cast<uid_t>(-1), static_cast<gid_t>(-1),
                                  error));
}

TEST_CASE(update_files_read_refuses_links_second_names_fifos_and_strangers) {
    Scratch dir;
    std::string contents, error;
    bool missing = false;
    const uid_t me = ::getuid();
    put(dir.at("real"), "settings");
    CHECK(fernsdr::read_file_at(dir.fd, "real", 1024, me, contents, missing, error));
    CHECK_EQ_STR(contents, "settings");
    // Not there at all is told apart from refused.
    CHECK(!fernsdr::read_file_at(dir.fd, "absent", 1024, me, contents, missing, error));
    CHECK(missing);
    // A link, even to a readable file of the same owner.
    CHECK(::symlink(dir.at("real").c_str(), dir.at("link").c_str()) == 0);
    CHECK(!fernsdr::read_file_at(dir.fd, "link", 1024, me, contents, missing, error));
    CHECK(!missing && error.find("link") != std::string::npos);
    // A second name for a file.
    CHECK(::link(dir.at("real").c_str(), dir.at("second").c_str()) == 0);
    CHECK(!fernsdr::read_file_at(dir.fd, "second", 1024, me, contents, missing, error));
    CHECK(error.find("more than one name") != std::string::npos);
    ::unlink(dir.at("second").c_str());
    // A FIFO, which would otherwise hold the open until someone wrote to it.
    CHECK(::mkfifo(dir.at("fifo").c_str(), 0600) == 0);
    CHECK(!fernsdr::read_file_at(dir.fd, "fifo", 1024, me, contents, missing, error));
    CHECK(error.find("not a regular file") != std::string::npos);
    // Someone else's file, and one larger than asked for.
    CHECK(!fernsdr::read_file_at(dir.fd, "real", 1024, me + 1, contents, missing, error));
    CHECK(error.find("someone else") != std::string::npos);
    CHECK(!fernsdr::read_file_at(dir.fd, "real", 3, me, contents, missing, error));
    CHECK(error.find("larger") != std::string::npos);
}

TEST_CASE(update_files_links_are_swapped_whole_and_trees_removed_without_following) {
    Scratch dir;
    std::string error, target;
    CHECK(::mkdir(dir.at("a").c_str(), 0755) == 0 && ::mkdir(dir.at("b").c_str(), 0755) == 0);
    CHECK(fernsdr::replace_link_at(dir.fd, "current", "a", error));
    CHECK(fernsdr::read_link_at(dir.fd, "current", target) && target == "a");
    CHECK(fernsdr::replace_link_at(dir.fd, "current", "b", error));
    CHECK(fernsdr::read_link_at(dir.fd, "current", target) && target == "b");
    CHECK(!fernsdr::read_link_at(dir.fd, "a", target));

    // A tree with a link inside that points out of it: the link goes, what it
    // points at stays.
    put(dir.at("keep"), "outside");
    CHECK(::mkdir(dir.at("tree").c_str(), 0755) == 0 && ::mkdir(dir.at("tree/sub").c_str(), 0755) == 0);
    put(dir.at("tree/sub/file"), "x");
    CHECK(::symlink(dir.at("keep").c_str(), dir.at("tree/sub/out").c_str()) == 0);
    CHECK(::symlink(dir.path.c_str(), dir.at("tree/up").c_str()) == 0);
    CHECK(fernsdr::remove_tree_at(dir.fd, "tree", error));
    CHECK_EQ_STR(contents_of(dir.at("keep")), "outside");
    CHECK(::access(dir.at("tree").c_str(), F_OK) != 0);
    CHECK(::access(dir.at("a").c_str(), F_OK) == 0);
    // Gone already is done.
    CHECK(fernsdr::remove_tree_at(dir.fd, "tree", error));
    CHECK(fernsdr::remove_file_at(dir.fd, "nothing-here", error));
}

TEST_CASE(update_extract_writes_the_tree_with_its_own_modes) {
    std::string archive, error;
    CHECK(fernsdr::write_ustar({{"fernsdr", false, true, "program"},
                                {"web/assets/app.js", false, false, "script"},
                                {"web", true, false, ""},
                                {"LICENSE", false, false, "licence"}},
                               0, archive, error));
    std::vector<fernsdr::UstarEntry> entries;
    CHECK(fernsdr::read_ustar(archive, entries, error));
    Scratch dir;
    // Whatever umask the updater inherits, the modes come out the same.
    const mode_t old = ::umask(077);
    const bool extracted = fernsdr::extract_ustar(archive, entries, dir.fd, error);
    ::umask(old);
    CHECK(extracted);
    CHECK_EQ_STR(contents_of(dir.at("fernsdr")), "program");
    CHECK_EQ_STR(contents_of(dir.at("web/assets/app.js")), "script");
    CHECK_EQ(mode_of(dir.at("fernsdr")), 0755u);
    CHECK_EQ(mode_of(dir.at("LICENSE")), 0644u);
    CHECK_EQ(mode_of(dir.at("web")), 0755u);
    CHECK_EQ(mode_of(dir.at("web/assets")), 0755u);
}

TEST_CASE(update_extract_creates_every_entry_new) {
    std::string archive, error;
    CHECK(fernsdr::write_ustar({{"web/index.html", false, false, "page"}, {"fernsdr", false, true, "program"}}, 0,
                               archive, error));
    std::vector<fernsdr::UstarEntry> entries;
    CHECK(fernsdr::read_ustar(archive, entries, error));
    // Something in the way of a file, and a link in the way of a directory:
    // neither is written through.
    Scratch occupied;
    put(occupied.at("fernsdr"), "already here");
    CHECK(!fernsdr::extract_ustar(archive, entries, occupied.fd, error));
    CHECK_EQ_STR(contents_of(occupied.at("fernsdr")), "already here");
    Scratch linked, elsewhere;
    CHECK(::symlink(elsewhere.path.c_str(), linked.at("web").c_str()) == 0);
    CHECK(!fernsdr::extract_ustar(archive, entries, linked.fd, error));
    CHECK(::access(elsewhere.at("index.html").c_str(), F_OK) != 0);
}
