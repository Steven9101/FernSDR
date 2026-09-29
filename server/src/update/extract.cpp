#include "extract.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>

namespace fernsdr {

namespace {

struct Closer {
    void operator()(int* fd) const {
        if (*fd >= 0) ::close(*fd);
        delete fd;
    }
};
using Handle = std::unique_ptr<int, Closer>;

Handle handle(int fd) { return Handle(new int(fd)); }

}  // namespace

bool extract_ustar(const std::string& archive, const std::vector<UstarEntry>& entries, int dir, std::string& error) {
    // Directories by path, "" being `dir` itself, kept open to sync at the
    // end: a file's name is only durable once its directory is.
    std::map<std::string, Handle> directories;
    const int top = ::fcntl(dir, F_DUPFD_CLOEXEC, 0);
    if (top < 0) {
        error = std::string("cannot use the directory: ") + std::strerror(errno);
        return false;
    }
    directories.emplace("", handle(top));

    // The directory at `path`, made if the archive has not made it yet.
    const auto directory_at = [&](const std::string& path) -> int {
        std::string done;
        int parent = *directories.at("");
        size_t start = 0;
        while (start < path.size()) {
            size_t slash = path.find('/', start);
            if (slash == std::string::npos) slash = path.size();
            const std::string name = path.substr(start, slash - start);
            done = done.empty() ? name : done + "/" + name;
            auto known = directories.find(done);
            if (known == directories.end()) {
                if (::mkdirat(parent, name.c_str(), 0700) != 0 && errno != EEXIST) {
                    error = "cannot make " + done + ": " + std::strerror(errno);
                    return -1;
                }
                const int fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                // Made here, so it is ours; fchmod on the descriptor rather
                // than trusting the umask.
                if (fd < 0 || ::fchmod(fd, 0755) != 0) {
                    error = "cannot open " + done + ": " + std::strerror(errno);
                    if (fd >= 0) ::close(fd);
                    return -1;
                }
                known = directories.emplace(done, handle(fd)).first;
            }
            parent = *known->second;
            start = slash + 1;
        }
        return parent;
    };

    for (const UstarEntry& entry : entries) {
        if (entry.directory) {
            if (directory_at(entry.path) < 0) return false;
            continue;
        }
        const size_t slash = entry.path.rfind('/');
        const std::string parent_path = slash == std::string::npos ? "" : entry.path.substr(0, slash);
        const std::string name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
        const int parent = parent_path.empty() ? *directories.at("") : directory_at(parent_path);
        if (parent < 0) return false;
        Handle file = handle(::openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (*file < 0) {
            error = "cannot create " + entry.path + ": " + std::strerror(errno);
            return false;
        }
        size_t done = 0;
        while (done < entry.size) {
            const ssize_t n = ::write(*file, archive.data() + entry.offset + done, entry.size - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                error = "cannot write " + entry.path + ": " + std::strerror(errno);
                return false;
            }
            done += static_cast<size_t>(n);
        }
        if (::fchmod(*file, entry.executable ? 0755 : 0644) != 0 || ::fsync(*file) != 0) {
            error = "cannot finish " + entry.path + ": " + std::strerror(errno);
            return false;
        }
    }
    for (const auto& [path, fd] : directories) {
        if (::fsync(*fd) != 0) {
            error = "cannot sync " + (path.empty() ? std::string("the directory") : path) + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

}  // namespace fernsdr
