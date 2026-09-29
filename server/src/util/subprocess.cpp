#include "subprocess.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

namespace fernsdr {

namespace {

std::atomic<bool> g_shutdown{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the shutdown flag is set from a signal handler");
std::atomic<bool> g_force_listing{false};

using Clock = std::chrono::steady_clock;

// Moves a descriptor to 10 or above. The child's ends are later duplicated
// onto 0 to 3, and with some C libraries one that already sat on its target
// would keep its close-on-exec flag and vanish at exec.
int lift(int fd) {
    const int lifted = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return lifted;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Sends to the whole group the child leads, and to the child itself should it
// have left that group.
void signal_child(pid_t pid, int signal_number) {
    if (::kill(-pid, signal_number) != 0) ::kill(pid, signal_number);
}

}  // namespace

void request_shutdown() { g_shutdown.store(true, std::memory_order_relaxed); }
bool shutdown_requested() { return g_shutdown.load(std::memory_order_relaxed); }

void Subprocess::force_descriptor_listing(bool force) { g_force_listing.store(force); }

std::string Subprocess::Exit::describe() const {
    if (abandoned) return "did not end even after SIGKILL; it is stuck in the kernel, usually in a USB driver";
    if (exited) return "exited with status " + std::to_string(code);
    if (signal > 0) {
        const char* name = ::strsignal(signal);
        return std::string("was ended by signal ") + std::to_string(signal) + (name ? std::string(" (") + name + ")" : "");
    }
    return "ended";
}

Subprocess::~Subprocess() {
    if (pid_ > 0 && !reaped_) {
        terminate({0, 1, 2, 3}, std::chrono::milliseconds(500), std::chrono::milliseconds(1000));
    }
    close_all();
}

void Subprocess::close_all() {
    for (int& fd : ends_) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
}

ssize_t Subprocess::write_to(size_t child_fd, const char* data, size_t size) {
    const int fd = this->fd(child_fd);
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    // Blocked on this thread only, and a SIGPIPE this write raised is taken
    // back off the queue before unblocking, unless one was already pending.
    sigset_t pipe_signal, previous, pending;
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    sigpending(&pending);
    const bool already_pending = sigismember(&pending, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);
    const ssize_t written = ::write(fd, data, size);
    const int saved = errno;
    if (written < 0 && saved == EPIPE && !already_pending) {
        const timespec zero{0, 0};
        while (sigtimedwait(&pipe_signal, nullptr, &zero) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = saved;
    return written;
}

void Subprocess::close_fd(size_t child_fd) {
    if (child_fd < ends_.size() && ends_[child_fd] >= 0) {
        ::close(ends_[child_fd]);
        ends_[child_fd] = -1;
    }
}

bool Subprocess::start(const Options& options, std::string& error) {
    if (pid_ > 0) {
        error = "this process was already started";
        return false;
    }
    if (options.path.empty() || options.path[0] != '/') {
        error = "a program to run needs an absolute path, not '" + options.path + "'";
        return false;
    }
    if (shutdown_requested()) {
        error = "the receiver is shutting down";
        return false;
    }

    const size_t count = options.streams.size();
    std::vector<int> child(count, -1);
    ends_.assign(count, -1);
    auto give_up = [&](const std::string& what) {
        const int reason = errno;
        error = what + ": " + std::strerror(reason);
        for (int& fd : child) {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }
        close_all();
        errno = reason;  // for a caller telling a passing shortage from a lasting fault
        return false;
    };

    for (size_t i = 0; i < count; i++) {
        if (options.streams[i] == Stream::Null) {
            child[i] = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (child[i] < 0) return give_up("cannot open /dev/null");
        } else {
            int pipe_fds[2];
            if (::pipe2(pipe_fds, O_CLOEXEC) != 0) return give_up("cannot create a pipe");
            const bool to_child = options.streams[i] == Stream::ToChild;
            child[i] = to_child ? pipe_fds[0] : pipe_fds[1];
            ends_[i] = to_child ? pipe_fds[1] : pipe_fds[0];
            if (!set_nonblocking(ends_[i])) return give_up("cannot set a pipe non-blocking");
        }
        child[i] = lift(child[i]);
        if (child[i] < 0) return give_up("cannot move a descriptor");
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (size_t i = 0; i < count; i++) {
        posix_spawn_file_actions_adddup2(&actions, child[i], static_cast<int>(i));
    }
    bool closed_above = false;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (!g_force_listing.load()) {
        closed_above = posix_spawn_file_actions_addclosefrom_np(&actions, static_cast<int>(count)) == 0;
    }
#endif
    if (!closed_above) {
        // Older C libraries cannot close a range in the child, so close what
        // is open now, one by one. Anything opened between this listing and
        // the spawn is close-on-exec, as is every descriptor this server
        // opens; the listing covers what a library opened without asking.
        if (DIR* directory = ::opendir("/proc/self/fd")) {
            const int own = ::dirfd(directory);
            while (const dirent* entry = ::readdir(directory)) {
                char* end = nullptr;
                const long fd = std::strtol(entry->d_name, &end, 10);
                if (end == entry->d_name || *end != '\0') continue;
                if (fd < static_cast<long>(count) || fd == own) continue;
                posix_spawn_file_actions_addclose(&actions, static_cast<int>(fd));
            }
            ::closedir(directory);
        }
    }

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t every, none;
    sigfillset(&every);
    sigemptyset(&none);
    // This server ignores SIGPIPE, and an ignored signal stays ignored across
    // exec. A module has to die of a broken pipe like any other program.
    posix_spawnattr_setsigdefault(&attributes, &every);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(options.path.c_str()));
    for (const std::string& argument : options.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : options.environment) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int result = ::posix_spawn(&pid, options.path.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    for (int& fd : child) {
        ::close(fd);
        fd = -1;
    }
    if (result != 0) {
        error = "cannot run " + options.path + ": " + std::strerror(result);
        close_all();
        errno = result;
        return false;
    }
    pid_ = pid;
    reaped_ = false;
    exit_ = Exit{};
    return true;
}

bool Subprocess::poll_exit(Exit& out) {
    if (pid_ <= 0 || reaped_) {
        out = exit_;
        return reaped_;
    }
    // Looked at without collecting it first. While the child is an
    // uncollected zombie its process group still exists, so anything it
    // started, such as a helper holding the USB device, can be ended with
    // it; once collected, the group id could belong to someone else.
    siginfo_t info{};
    int looked;
    do {
        looked = ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT);
    } while (looked < 0 && errno == EINTR);
    if (looked == 0 && info.si_pid == 0) return false;
    if (looked == 0) ::kill(-pid_, SIGKILL);
    int status = 0;
    pid_t got;
    do {
        got = ::waitpid(pid_, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == 0) return false;
    Exit result;
    if (got == pid_) {
        if (WIFEXITED(status)) {
            result.exited = true;
            result.code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.signal = WTERMSIG(status);
        }
    }
    // ECHILD means someone else reaped it, which nothing here does; either
    // way it is gone and its pid must never be signalled again.
    exit_ = result;
    reaped_ = true;
    out = exit_;
    return true;
}

Subprocess::Exit Subprocess::terminate(const std::vector<size_t>& close_first, std::chrono::milliseconds grace,
                                       std::chrono::milliseconds kill_wait, const std::function<void()>& pump) {
    for (size_t fd : close_first) close_fd(fd);
    Exit status;
    if (pid_ <= 0) return exit_;
    auto wait_for = [&](std::chrono::milliseconds limit) {
        const auto deadline = Clock::now() + limit;
        while (true) {
            if (poll_exit(status)) return true;
            if (Clock::now() >= deadline) return false;
            if (pump) pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    if (!wait_for(grace)) {
        signal_child(pid_, SIGTERM);
        if (!wait_for(grace)) {
            signal_child(pid_, SIGKILL);
            if (!wait_for(kill_wait)) {
                const pid_t pid = pid_;
                // Someone has to collect it when the kernel finally lets go,
                // or it stays a zombie for the life of the receiver.
                try {
                    std::thread([pid] {
                        int ignored = 0;
                        while (::waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {
                        }
                    }).detach();
                } catch (const std::system_error&) {
                    // Without a thread it stays a zombie until the receiver
                    // exits, which is still better than waiting for it here.
                }
                exit_ = Exit{};
                exit_.abandoned = true;
                reaped_ = true;
                status = exit_;
            }
        }
    }
    close_all();
    return status;
}

bool Subprocess::run(const std::string& path, const std::vector<std::string>& arguments,
                     const std::vector<std::string>& environment, std::chrono::milliseconds timeout,
                     size_t limit, std::string& output, std::string& errors, Exit& status, std::string& error,
                     const std::atomic<bool>* cancel, const std::string* input) {
    Subprocess child;
    Options options;
    options.path = path;
    options.arguments = arguments;
    options.environment = environment;
    options.streams = {input ? Stream::ToChild : Stream::Null, Stream::FromChild, Stream::FromChild};
    output.clear();
    errors.clear();
    // Set below only by this run, and read at the end to tell whether it
    // failed; whatever the caller left in it is not this run's.
    error.clear();
    if (!child.start(options, error)) return false;

    const auto deadline = Clock::now() + timeout;
    bool overflow = false;
    bool timed_out = false;
    bool cancelled = false;
    char buffer[16384];
    // The input goes in as the pipe takes it, in the same loop that reads
    // what the program prints, so one that answers while still reading
    // cannot leave both sides waiting on a full pipe.
    size_t offset = 0;
    if (input && input->empty()) child.close_fd(0);
    while (child.fd(0) >= 0 || child.fd(1) >= 0 || child.fd(2) >= 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            timed_out = true;
            break;
        }
        if ((cancel && cancel->load()) || shutdown_requested()) {
            cancelled = true;
            break;
        }
        pollfd descriptors[3];
        size_t which[3];
        int used = 0;
        for (size_t stream = 0; stream <= 2; stream++) {
            if (child.fd(stream) < 0) continue;
            descriptors[used] = {child.fd(stream), static_cast<short>(stream == 0 ? POLLOUT : POLLIN), 0};
            which[used] = stream;
            used++;
        }
        const int ready = ::poll(descriptors, static_cast<nfds_t>(used),
                                 static_cast<int>(std::min<long long>(remaining.count(), 100)));
        if (ready < 0 && errno != EINTR) {
            error = std::string("poll: ") + std::strerror(errno);
            break;
        }
        for (int i = 0; i < used && !overflow; i++) {
            if (which[i] == 0) {
                if (!(descriptors[i].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
                const ssize_t written = child.write_to(0, input->data() + offset, input->size() - offset);
                if (written > 0) offset += static_cast<size_t>(written);
                // Written whole, or the program stopped reading: either way
                // it gets the end of its input.
                if (offset == input->size() ||
                    (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    child.close_fd(0);
                }
                continue;
            }
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            std::string& into = which[i] == 1 ? output : errors;
            while (true) {
                const ssize_t got = ::read(child.fd(which[i]), buffer, sizeof(buffer));
                if (got > 0) {
                    if (into.size() + static_cast<size_t>(got) > limit) {
                        overflow = true;
                        break;
                    }
                    into.append(buffer, static_cast<size_t>(got));
                    continue;
                }
                if (got == 0) child.close_fd(which[i]);
                else if (errno == EINTR) continue;
                else if (errno != EAGAIN && errno != EWOULDBLOCK) child.close_fd(which[i]);
                break;
            }
        }
        if (overflow) break;
    }

    // All three pipes are closed, so it has finished or is about to; give it
    // what is left of its time to be collected.
    while (!overflow && !timed_out && !cancelled && !child.poll_exit(status)) {
        if (Clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        if ((cancel && cancel->load()) || shutdown_requested()) {
            cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (overflow || timed_out || cancelled) {
        status = child.terminate({0, 1, 2}, std::chrono::milliseconds(100), std::chrono::milliseconds(1000));
        error = overflow ? path + " printed more than " + std::to_string(limit) + " bytes"
              : cancelled ? path + " was stopped because the receiver is shutting down"
                          : path + " did not finish within " + std::to_string(timeout.count() / 1000) + " s";
        return false;
    }
    if (!error.empty()) return false;
    return true;
}

}  // namespace fernsdr
