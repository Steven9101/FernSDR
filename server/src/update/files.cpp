#include "files.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "../util/password.h"

namespace fernsdr {

namespace {

// One name in a directory: nothing that climbs or crosses into another.
bool plain_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos;
}

std::string reason(const std::string& what, const std::string& name) {
    return what + " " + name + ": " + std::strerror(errno);
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() {
        if (fd_ >= 0) ::close(fd_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

bool write_all(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

// A name no one else will have made: a leading dot, the name it stands in
// for, and random letters, created with O_EXCL so a name someone did make is
// never used.
std::string temporary_name(const std::string& name) {
    const std::string random = random_hex(6);
    return "." + name + "." + (random.empty() ? std::to_string(::getpid()) : random);
}

bool remove_tree_below(int dir, const std::string& name, int depth, std::string& error) {
    struct stat info {};
    if (::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        error = reason("cannot look at", name);
        return false;
    }
    if (S_ISDIR(info.st_mode)) {
        if (depth > 64) {
            error = "too deep to remove: " + name;
            return false;
        }
        Descriptor sub(::openat(dir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (sub.get() < 0) {
            error = reason("cannot open", name);
            return false;
        }
        std::vector<std::string> children;
        {
            // fdopendir takes the descriptor over, so it gets a copy.
            const int copy = ::fcntl(sub.get(), F_DUPFD_CLOEXEC, 0);
            DIR* listing = copy >= 0 ? ::fdopendir(copy) : nullptr;
            if (!listing) {
                if (copy >= 0) ::close(copy);
                error = reason("cannot list", name);
                return false;
            }
            while (const dirent* entry = ::readdir(listing)) {
                const std::string child = entry->d_name;
                if (child != "." && child != "..") children.push_back(child);
            }
            ::closedir(listing);
        }
        for (const std::string& child : children) {
            if (!remove_tree_below(sub.get(), child, depth + 1, error)) return false;
        }
        if (::unlinkat(dir, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
            error = reason("cannot remove", name);
            return false;
        }
        return true;
    }
    if (::unlinkat(dir, name.c_str(), 0) != 0 && errno != ENOENT) {
        error = reason("cannot remove", name);
        return false;
    }
    return true;
}

}  // namespace

int open_directory(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int open_directory_at(int dir, const std::string& name) {
    if (!plain_name(name)) {
        errno = EINVAL;
        return -1;
    }
    return ::openat(dir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

bool held_by_this_user(int dir) {
    struct stat info {};
    return dir >= 0 && ::fstat(dir, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() &&
           (info.st_mode & 022) == 0;
}

bool read_file_at(int dir, const std::string& name, size_t limit, uid_t owner, std::string& contents, bool& missing,
                  std::string& error) {
    contents.clear();
    missing = false;
    if (!plain_name(name)) {
        error = "not a file name: " + name;
        return false;
    }
    // O_NONBLOCK: a FIFO in the file's place would otherwise hold the open.
    Descriptor fd(::openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
        if (errno == ENOENT) {
            missing = true;
            error = name + " is not there";
        } else if (errno == ELOOP) {
            error = name + " is a link, which is not followed";
        } else {
            error = reason("cannot open", name);
        }
        return false;
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = name + " is not a regular file";
        return false;
    }
    // A second name could be another user's file linked in under this one.
    if (info.st_nlink != 1) {
        error = name + " has more than one name";
        return false;
    }
    if (owner != static_cast<uid_t>(-1) && info.st_uid != owner) {
        error = name + " belongs to someone else";
        return false;
    }
    if (static_cast<uint64_t>(info.st_size) > limit) {
        error = name + " is larger than it can be";
        return false;
    }
    char buffer[65536];
    while (true) {
        const ssize_t n = ::read(fd.get(), buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            error = reason("cannot read", name);
            return false;
        }
        if (n == 0) break;
        contents.append(buffer, static_cast<size_t>(n));
        if (contents.size() > limit) {
            error = name + " is larger than it can be";
            return false;
        }
    }
    return true;
}

bool write_file_at(int dir, const std::string& name, const std::string& contents, mode_t mode, uid_t uid, gid_t gid,
                   std::string& error) {
    if (!plain_name(name)) {
        error = "not a file name: " + name;
        return false;
    }
    std::string temporary;
    int raw = -1;
    for (int attempt = 0; attempt < 8 && raw < 0; attempt++) {
        temporary = temporary_name(name);
        raw = ::openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (raw < 0 && errno != EEXIST) break;
    }
    if (raw < 0) {
        error = reason("cannot create a file beside", name);
        return false;
    }
    Descriptor fd(raw);
    const bool written = write_all(fd.get(), contents) && ::fchmod(fd.get(), mode) == 0 &&
                         ((uid == static_cast<uid_t>(-1) && gid == static_cast<gid_t>(-1)) ||
                          ::fchown(fd.get(), uid, gid) == 0) &&
                         ::fsync(fd.get()) == 0;
    if (!written || ::close(fd.release()) != 0) {
        error = reason("cannot write", name);
        ::unlinkat(dir, temporary.c_str(), 0);
        return false;
    }
    if (::renameat(dir, temporary.c_str(), dir, name.c_str()) != 0) {
        error = reason("cannot put in place", name);
        ::unlinkat(dir, temporary.c_str(), 0);
        return false;
    }
    if (!sync_directory(dir)) {
        error = reason("cannot sync the directory of", name);
        return false;
    }
    return true;
}

bool replace_link_at(int dir, const std::string& name, const std::string& target, std::string& error) {
    if (!plain_name(name)) {
        error = "not a link name: " + name;
        return false;
    }
    std::string temporary;
    int result = -1;
    for (int attempt = 0; attempt < 8 && result != 0; attempt++) {
        temporary = temporary_name(name);
        result = ::symlinkat(target.c_str(), dir, temporary.c_str());
        if (result != 0 && errno != EEXIST) break;
    }
    if (result != 0) {
        error = reason("cannot make a link beside", name);
        return false;
    }
    if (::renameat(dir, temporary.c_str(), dir, name.c_str()) != 0) {
        error = reason("cannot put in place", name);
        ::unlinkat(dir, temporary.c_str(), 0);
        return false;
    }
    if (!sync_directory(dir)) {
        error = reason("cannot sync the directory of", name);
        return false;
    }
    return true;
}

bool read_link_at(int dir, const std::string& name, std::string& target) {
    if (!plain_name(name)) return false;
    char buffer[4096];
    const ssize_t n = ::readlinkat(dir, name.c_str(), buffer, sizeof(buffer));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buffer)) return false;
    target.assign(buffer, static_cast<size_t>(n));
    return true;
}

bool remove_tree_at(int dir, const std::string& name, std::string& error) {
    if (!plain_name(name)) {
        error = "not a name to remove: " + name;
        return false;
    }
    return remove_tree_below(dir, name, 0, error);
}

bool remove_file_at(int dir, const std::string& name, std::string& error) {
    if (!plain_name(name)) {
        error = "not a file name: " + name;
        return false;
    }
    if (::unlinkat(dir, name.c_str(), 0) != 0 && errno != ENOENT) {
        error = reason("cannot remove", name);
        return false;
    }
    return true;
}

bool sync_directory(int dir) { return ::fsync(dir) == 0; }

bool free_bytes(int dir, uint64_t& bytes) {
    struct statvfs info {};
    if (::fstatvfs(dir, &info) != 0) return false;
    bytes = static_cast<uint64_t>(info.f_bavail) * static_cast<uint64_t>(info.f_frsize);
    return true;
}

}  // namespace fernsdr
