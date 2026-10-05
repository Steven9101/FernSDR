#include "supervisor.h"

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>

#include "../version.h"
#include "container.h"
#include "files.h"
#include "release_keys.h"
#include "updater.h"

namespace fernsdr {

// --- RestartPolicy --------------------------------------------------------------

void RestartPolicy::started(int64_t now_ms, bool automatic) {
    if (automatic) ++restarts_;
    starts_.push_back(now_ms);
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](int64_t at) { return at <= now_ms - settings_.window_ms; }),
                  starts_.end());
    running_since_ = now_ms;
}

int64_t RestartPolicy::stopped(int64_t now_ms) {
    running_since_ = -1;
    // As systemd counts: would the next start, after the delay, be one more
    // than the limit allows within the window before it? With a two-second
    // delay the window of ten seconds never holds five, so this takes a
    // receiver that dies at once, not one that only keeps failing.
    const int64_t next = now_ms + settings_.delay_ms;
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](int64_t at) { return at <= next - settings_.window_ms; }),
                  starts_.end());
    if (static_cast<int>(starts_.size()) >= settings_.burst) failed_ = true;
    return failed_ ? now_ms + settings_.failed_retry_ms : next;
}

void RestartPolicy::tick(int64_t now_ms) {
    if (failed_ && running_since_ >= 0 && now_ms - running_since_ >= settings_.stable_ms) failed_ = false;
}

void RestartPolicy::reset() {
    // systemd starts NRestarts over on a start that is not its own, and the
    // updater takes its baseline after the restart it asked for anyway.
    starts_.clear();
    restarts_ = 0;
    failed_ = false;
}

// --- the updater's side of the socket ---------------------------------------------

namespace {

bool send_line(int fd, const std::string& line) {
    const std::string text = line + "\n";
    size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// One line, within `timeout_ms`. A restart waits for the receiver to stop,
// which may take the whole stop time and then the modules' cleanup.
bool read_line(int fd, int64_t timeout_ms, std::string& line) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd ready{fd, POLLIN, 0};
        const int n = ::poll(&ready, 1, static_cast<int>(std::min<int64_t>(left.count(), 1000)));
        if (n < 0 && errno != EINTR) return false;
        if (n <= 0) continue;
        char c;
        const ssize_t got = ::read(fd, &c, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        if (c == '\n') return true;
        if (line.size() >= 4096) return false;
        line.push_back(c);
    }
}

}  // namespace

int take_supervisor_fd() {
    const char* value = std::getenv(kSupervisorFdVariable);
    if (!value) return -1;
    char* end = nullptr;
    const long fd = std::strtol(value, &end, 10);
    ::unsetenv(kSupervisorFdVariable);
    if (!*value || *end || fd < 3 || fd > 1024) return -1;
    struct stat info {};
    if (::fstat(static_cast<int>(fd), &info) != 0 || !S_ISSOCK(info.st_mode)) return -1;
    ::fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC);
    return static_cast<int>(fd);
}

bool supervisor_restart(int fd, std::string& error) {
    std::string answer;
    if (!send_line(fd, "restart") || !read_line(fd, 120000, answer)) {
        error = "the supervisor did not answer";
        return false;
    }
    if (answer == "ok") return true;
    error = answer.rfind("error ", 0) == 0 ? answer.substr(6) : "the supervisor answered: " + answer;
    return false;
}

bool parse_service_answer(const std::string& line, int& restarts, bool& failed) {
    unsigned count = 0;
    char state = 0;
    char extra = 0;
    if (std::sscanf(line.c_str(), "service %u %c%c", &count, &state, &extra) != 2) return false;
    if (state != '0' && state != '1') return false;
    if (count > 1000000) return false;
    restarts = static_cast<int>(count);
    failed = state == '1';
    return true;
}

bool supervisor_service(int fd, int& restarts, bool& failed) {
    std::string answer;
    return send_line(fd, "service") && read_line(fd, 10000, answer) && parse_service_answer(answer, restarts, failed);
}

// --- the supervisor -------------------------------------------------------------

namespace {

int g_signal_pipe = -1;

void on_signal(int number) {
    const int saved = errno;
    const unsigned char byte = static_cast<unsigned char>(number);
    (void)!::write(g_signal_pipe, &byte, 1);
    errno = saved;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// In a child, before exec: what the supervisor changed about signals must
// not reach the program. A handler goes back to the default on exec by
// itself, but a blocked mask would stay, and the receiver would never see
// the SIGTERM that asks it to stop its modules.
void reset_signals() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    for (int number : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE}) ::sigaction(number, &action, nullptr);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
}

// Closes every descriptor from `lowest` up. The supervisor is one thread, so
// reading /proc after fork is safe where close_range is missing (before
// Linux 5.9).
void close_from(int lowest) {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned>(lowest), ~0U, 0U) == 0) return;
#endif
    if (DIR* directory = ::opendir("/proc/self/fd")) {
        const int own = ::dirfd(directory);
        std::vector<int> open;
        while (dirent* entry = ::readdir(directory)) {
            char* end = nullptr;
            const long fd = std::strtol(entry->d_name, &end, 10);
            if (end != entry->d_name && *end == '\0' && fd >= lowest && fd != own) open.push_back(static_cast<int>(fd));
        }
        ::closedir(directory);
        for (int fd : open) ::close(fd);
        return;
    }
    for (int fd = lowest; fd < 65536; ++fd) ::close(fd);
}

// Waits up to `limit_ms` for `pid`; false when it has not ended by then.
bool wait_for_exit(pid_t pid, int64_t limit_ms, int& status) {
    const int64_t deadline = now_ms() + limit_ms;
    while (true) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid || (done < 0 && errno == ECHILD)) return true;
        if (now_ms() >= deadline) return false;
        ::usleep(20000);
    }
}

std::string describe_status(int status) {
    if (WIFEXITED(status)) return "exit status " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::string("signal ") + strsignal(WTERMSIG(status));
    return "status " + std::to_string(status);
}

// The log: the supervisor's own lines with a time, and whatever the
// receiver and the updater write, line by line, in a file of root's that
// becomes <name>.1 past the limit. The children write into pipes, never
// into the file: they cannot grow it past what the supervisor lets through,
// and the receiver has no way to the file itself.
class Log {
public:
    void open(const std::string& directory, size_t limit) {
        limit_ = limit;
        if (directory.empty()) return;
        ::mkdir(directory.c_str(), 0755);
        directory_ = open_directory(directory);
        struct stat info {};
        if (directory_ >= 0 && (::fstat(directory_, &info) != 0 || info.st_uid != ::geteuid())) {
            ::close(directory_);
            directory_ = -1;
        }
        reopen();
        if (file_ < 0) note("cannot write the log in " + directory + "; it goes to standard error");
    }

    void note(const std::string& text) {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm local {};
        ::localtime_r(&now, &local);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        write(std::string(stamp) + " supervise: " + text + "\n");
    }

    void write(const std::string& text) {
        if (file_ >= 0 && size_ + text.size() > limit_) rotate();
        const int fd = file_ >= 0 ? file_ : 2;
        size_t done = 0;
        while (done < text.size()) {
            const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        size_ += done;
    }

private:
    void reopen() {
        if (file_ >= 0) ::close(file_);
        file_ = directory_ < 0 ? -1
                               : ::openat(directory_, "fernsdr.log",
                                          O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0640);
        struct stat info {};
        if (file_ >= 0 && (::fstat(file_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)) {
            ::close(file_);
            file_ = -1;
        }
        size_ = file_ >= 0 ? static_cast<size_t>(info.st_size) : 0;
    }

    void rotate() {
        ::renameat(directory_, "fernsdr.log", directory_, "fernsdr.log.1");
        reopen();
    }

    int directory_ = -1;
    int file_ = -1;
    size_t size_ = 0;
    size_t limit_ = 1 << 20;
};

// A pipe a child writes its output into, read line by line into the log.
struct Output {
    int fd = -1;
    std::string partial;
};

class Supervisor {
public:
    Supervisor(const SupervisorOptions& options) : options_(options), policy_(options.restart) {}

    int run(std::string& error);

private:
    struct Spawned {
        pid_t pid = -1;
        int output = -1;   // read end of its stdout and stderr
        int control = -1;  // the updater's socket
    };

    bool setup(std::string& error);
    bool spawn(const std::string& program, const std::vector<std::string>& arguments,
               const std::vector<std::string>& environment, bool as_receiver, bool with_control, Spawned& spawned,
               std::string& error);
    void boot_check();
    bool start_receiver(bool automatic, std::string& error);
    void stop_receiver();
    void cleanup_orphans();
    void reap();
    void on_receiver_stopped(int status);
    void on_updater_stopped(int status);
    void start_updater();
    void stop_updater();
    void read_output(Output& output);
    void read_control();
    void answer(const std::string& request);
    bool request_waiting() const;
    void drain_signals();

    const SupervisorOptions& options_;
    RestartPolicy policy_;
    Log log_;
    int lock_ = -1;
    int signals_[2] = {-1, -1};
    bool terminate_ = false;
    uid_t uid_ = 0;
    gid_t gid_ = 0;

    pid_t receiver_ = -1;
    int64_t next_start_ = -1;
    pid_t updater_ = -1;
    int control_ = -1;
    std::string control_buffer_;
    int64_t updater_deadline_ = 0;
    int64_t next_request_look_ = 0;
    int64_t requests_blocked_until_ = 0;
    int64_t backoff_ms_ = 0;
    std::vector<Output> outputs_;
};

bool Supervisor::setup(std::string& error) {
    log_.open(options_.log_directory, options_.log_limit);
    const int update = open_directory(options_.update);
    if (update < 0) {
        error = options_.update + " is missing: install.sh sets it up";
        return false;
    }
    if (!held_by_this_user(update)) {
        ::close(update);
        error = options_.update + " has to be root's alone";
        return false;
    }
    lock_ = ::openat(update, "supervise.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    ::close(update);
    if (lock_ < 0 || ::flock(lock_, LOCK_EX | LOCK_NB) != 0) {
        error = "another fernsdr --supervise is running";
        return false;
    }
    // The receiver's user is whoever owns its state directory, as for the
    // updater: no user database, which a static program cannot ask.
    const int state = open_directory(options_.state);
    struct stat info {};
    if (state < 0 || ::fstat(state, &info) != 0) {
        if (state >= 0) ::close(state);
        error = "cannot open " + options_.state;
        return false;
    }
    ::close(state);
    uid_ = info.st_uid;
    gid_ = info.st_gid;
    if (uid_ == 0 && !options_.state_may_be_roots) {
        error = options_.state + " belongs to root; the receiver has to run as a user of its own";
        return false;
    }
    if (gid_ == 0 && options_.refuse_root_group) {
        error = options_.state + " belongs to root's group; the receiver has to run in a group of its own";
        return false;
    }
    if (::pipe2(signals_, O_CLOEXEC | O_NONBLOCK) != 0) {
        error = "cannot make a pipe";
        return false;
    }
    g_signal_pipe = signals_[1];
    struct sigaction action {};
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGCHLD, SIGTERM, SIGINT}) ::sigaction(number, &action, nullptr);
    // A terminal that goes away is no reason to stop. Children get SIGHUP
    // back to its default before they exec.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGHUP, &ignore, nullptr);
    // A write to a reader that went away, such as the program that started
    // `--daemon` and was interrupted, must fail, not end the supervisor.
    ::sigaction(SIGPIPE, &ignore, nullptr);
    // Modules the receiver starts are its children, each in a process group
    // of its own. When the receiver dies they become this process's, not
    // init's, so that they can be stopped before the next receiver wants
    // the same radio.
    ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    return true;
}

bool Supervisor::spawn(const std::string& program, const std::vector<std::string>& arguments,
                       const std::vector<std::string>& environment, bool as_receiver, bool with_control,
                       Spawned& spawned, std::string& error) {
    int output[2] = {-1, -1};
    int status[2] = {-1, -1};
    int control[2] = {-1, -1};
    auto close_all = [&] {
        for (int fd : {output[0], output[1], status[0], status[1], control[0], control[1]})
            if (fd >= 0) ::close(fd);
    };
    if (::pipe2(output, O_CLOEXEC) != 0 || ::pipe2(status, O_CLOEXEC) != 0 ||
        (with_control && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0)) {
        close_all();
        error = std::string("cannot make a pipe: ") + std::strerror(errno);
        return false;
    }
    // Everything the child needs is built before fork.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : environment) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    const std::string directory = as_receiver ? options_.state : "/";
    const uid_t uid = uid_;
    const gid_t gid = gid_;
    const std::vector<gid_t>& groups = options_.receiver_groups;
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        error = std::string("cannot start a process: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        reset_signals();
        const int null = ::open("/dev/null", O_RDONLY);
        if (null < 0 || ::dup2(null, 0) < 0 || ::dup2(output[1], 1) < 0 || ::dup2(output[1], 2) < 0) _exit(126);
        // The updater's socket goes to 3, the exec status pipe right above
        // what the program is meant to have. Both are first copied out of the
        // way, since either may sit on the number the other is going to.
        const int status_copy = ::fcntl(status[1], F_DUPFD_CLOEXEC, 64);
        const int control_copy = with_control ? ::fcntl(control[1], F_DUPFD_CLOEXEC, 64) : -1;
        if (status_copy < 0 || (with_control && control_copy < 0)) _exit(126);
        int status_fd = 3;
        if (with_control) {
            if (::dup2(control_copy, 3) < 0) _exit(126);  // dup2 clears close-on-exec
            status_fd = 4;
        }
        if (::dup2(status_copy, status_fd) < 0 || ::fcntl(status_fd, F_SETFD, FD_CLOEXEC) != 0) _exit(126);
        close_from(status_fd + 1);
        auto fail = [&](int code) {
            (void)!::write(status_fd, &code, sizeof code);
            _exit(127);
        };
        if (as_receiver && uid != 0) {
            // Root goes for good: groups, then group, then user, in that
            // order, since each needs the privilege the next one drops.
            if (::setgroups(groups.size(), groups.empty() ? nullptr : groups.data()) != 0 ||
                ::setresgid(gid, gid, gid) != 0 || ::setresuid(uid, uid, uid) != 0)
                fail(errno);
            if (::setuid(0) == 0) fail(EPERM);
        }
        // After the change of user, which clears it; and if the supervisor
        // is already gone, there is nobody to stop this child later.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
        if (::getppid() != parent) _exit(0);
        if (as_receiver) ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (::chdir(directory.c_str()) != 0) fail(errno);
        ::execve(program.c_str(), argv.data(), envp.data());
        fail(errno);
    }
    ::close(output[1]);
    ::close(status[1]);
    if (with_control) ::close(control[1]);
    int code = 0;
    ssize_t got;
    do {
        got = ::read(status[0], &code, sizeof code);
    } while (got < 0 && errno == EINTR);
    ::close(status[0]);
    if (got > 0) {
        int ignored;
        while (::waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {
        }
        ::close(output[0]);
        if (with_control) ::close(control[0]);
        error = "cannot run " + program + ": " + std::strerror(code);
        return false;
    }
    ::fcntl(output[0], F_SETFL, O_NONBLOCK);
    spawned.pid = pid;
    spawned.output = output[0];
    spawned.control = with_control ? control[0] : -1;
    return true;
}

// What fernsdr-update-boot.service does before the receiver's unit starts.
void Supervisor::boot_check() {
    const std::string program = options_.install + "/trusted/fernsdr";
    if (::access(program.c_str(), X_OK) != 0) return;
    Spawned child;
    std::string error;
    // In a container, the boot check finds the update directory in the
    // volume by FERNSDR_CONTAINER; the URL and trial length are the
    // updater's alone.
    std::vector<std::string> environment = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8"};
    for (const std::string& entry : options_.updater_environment)
        if (entry.rfind("FERNSDR_CONTAINER=", 0) == 0) environment.push_back(entry);
    if (!spawn(program, {"--update-boot"}, environment, false, false, child, error)) {
        log_.note("the update boot check did not run: " + error);
        return;
    }
    Output output{child.output, {}};
    const int64_t deadline = now_ms() + options_.boot_limit_ms;
    while (output.fd >= 0 && now_ms() < deadline && !terminate_) {
        pollfd ready{output.fd, POLLIN, 0};
        if (::poll(&ready, 1, 1000) > 0) read_output(output);
        drain_signals();
    }
    if (output.fd >= 0) {
        ::kill(child.pid, SIGKILL);
        ::close(output.fd);
        log_.note(terminate_ ? "stopped during the update boot check" : "the update boot check took too long and was stopped");
    }
    int status = 0;
    if (!wait_for_exit(child.pid, 5000, status)) {
        log_.note("the update boot check does not end even when killed");
        return;
    }
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
        log_.note("the update boot check ended with " + describe_status(status));
}

bool Supervisor::start_receiver(bool automatic, std::string& error) {
    const std::string current = options_.install + "/current";
    // FERNSDR_SUPERVISED tells the receiver it is started again when it
    // exits, so the admin panel may offer to restart it.
    std::vector<std::string> environment = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                                            "LANG=C.UTF-8", "HOME=" + options_.state, "FERNSDR_SUPERVISED=1"};
    for (const std::string& entry : options_.updater_environment)
        if (entry.rfind("FERNSDR_UPDATE_URL=", 0) == 0) environment.push_back(entry);
    for (const std::string& entry : options_.receiver_environment) {
        const std::string name = entry.substr(0, entry.find('='));
        const bool set_here = std::any_of(environment.begin(), environment.end(), [&](const std::string& own) {
            return own.compare(0, name.size() + 1, name + "=") == 0;
        });
        if (!set_here && name != kSupervisorFdVariable) environment.push_back(entry);
    }
    Spawned child;
    std::vector<std::string> arguments = {options_.state + "/fernsdr.conf", "--root", current + "/web"};
    arguments.insert(arguments.end(), options_.receiver_arguments.begin(), options_.receiver_arguments.end());
    const bool started = spawn(current + "/fernsdr", arguments, environment, true, false, child, error);
    const int64_t now = now_ms();
    policy_.started(now, automatic);
    next_start_ = -1;
    if (!started) {
        log_.note("the receiver did not start: " + error);
        next_start_ = policy_.stopped(now);
        return false;
    }
    receiver_ = child.pid;
    outputs_.push_back(Output{child.output, {}});
    return true;
}

// SIGTERM, which the receiver answers by stopping its modules, and SIGKILL
// once the stop time is up; then whatever it left behind.
void Supervisor::stop_receiver() {
    if (receiver_ <= 0) return;
    ::kill(receiver_, SIGTERM);
    const int64_t deadline = now_ms() + options_.stop_ms;
    int status = 0;
    while (true) {
        const pid_t done = ::waitpid(receiver_, &status, WNOHANG);
        if (done == receiver_ || (done < 0 && errno != EINTR)) break;
        if (now_ms() >= deadline) {
            log_.note("the receiver did not stop within " + std::to_string(options_.stop_ms / 1000) +
                      " seconds and was killed");
            ::kill(receiver_, SIGKILL);
            if (!wait_for_exit(receiver_, 5000, status))
                log_.note("the receiver does not end even when killed; it is left to the kernel");
            break;
        }
        ::usleep(20000);
    }
    receiver_ = -1;
    cleanup_orphans();
}

// Processes that became this one's when the receiver died: its modules, or
// whatever they started. The updater is the only child meant to outlive the
// receiver.
void Supervisor::cleanup_orphans() {
    auto orphans = [&] {
        std::set<pid_t> found;
        DIR* directory = ::opendir("/proc");
        if (!directory) return found;
        const pid_t self = ::getpid();
        while (dirent* entry = ::readdir(directory)) {
            char* end = nullptr;
            const long pid = std::strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end || pid <= 0 || pid == updater_ || pid == receiver_) continue;
            char path[64];
            std::snprintf(path, sizeof path, "/proc/%ld/stat", pid);
            FILE* file = std::fopen(path, "re");
            if (!file) continue;
            char buffer[512];
            const size_t n = std::fread(buffer, 1, sizeof buffer - 1, file);
            std::fclose(file);
            buffer[n] = '\0';
            // pid (comm) state ppid ...; comm may hold anything but ends at
            // the last parenthesis.
            const char* close = std::strrchr(buffer, ')');
            char state = 0;
            long parent = 0;
            if (close && std::sscanf(close + 1, " %c %ld", &state, &parent) == 2 && parent == self)
                found.insert(static_cast<pid_t>(pid));
        }
        ::closedir(directory);
        return found;
    };
    auto reap_listed = [](std::set<pid_t>& listed) {
        for (auto it = listed.begin(); it != listed.end();) {
            int status;
            const pid_t done = ::waitpid(*it, &status, WNOHANG);
            it = (done == *it || (done < 0 && errno == ECHILD)) ? listed.erase(it) : std::next(it);
        }
    };
    std::set<pid_t> left = orphans();
    if (left.empty()) return;
    for (pid_t pid : left) ::kill(pid, SIGTERM);
    const int64_t deadline = now_ms() + options_.orphan_stop_ms;
    while (now_ms() < deadline) {
        reap_listed(left);
        for (pid_t pid : orphans())
            if (left.insert(pid).second) ::kill(pid, SIGTERM);
        if (left.empty()) return;
        ::usleep(20000);
    }
    log_.note("stopped " + std::to_string(left.size()) + " process(es) the receiver left behind, by force");
    for (pid_t pid : left) ::kill(pid, SIGKILL);
    // One stuck in the kernel, on a USB device that hangs, is not waited
    // for past a few seconds: reap() collects it whenever it ends.
    const int64_t killed_by = now_ms() + 3000;
    while (!left.empty() && now_ms() < killed_by) {
        reap_listed(left);
        if (!left.empty()) ::usleep(20000);
    }
    if (!left.empty()) log_.note(std::to_string(left.size()) + " of them do not end even when killed");
}

void Supervisor::on_receiver_stopped(int status) {
    receiver_ = -1;
    cleanup_orphans();
    const bool was_failed = policy_.failed();
    next_start_ = policy_.stopped(now_ms());
    log_.note("the receiver stopped (" + describe_status(status) + "); starting it again in " +
              std::to_string((next_start_ - now_ms() + 999) / 1000) + " s");
    if (policy_.failed() && !was_failed)
        log_.note("it started " + std::to_string(options_.restart.burst) + " times within " +
                  std::to_string(options_.restart.window_ms / 1000) + " s: from now on it is tried every " +
                  std::to_string(options_.restart.failed_retry_ms / 1000) + " s until a start lasts");
}

bool Supervisor::request_waiting() const {
    struct stat info {};
    return ::lstat((options_.state + "/" + kUpdateRequestFile).c_str(), &info) == 0;
}

void Supervisor::start_updater() {
    std::vector<std::string> environment = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8",
                                            std::string(kSupervisorFdVariable) + "=3"};
    for (const std::string& entry : options_.updater_environment) environment.push_back(entry);
    Spawned child;
    std::string error;
    if (!spawn(options_.install + "/trusted/fernsdr", {"--update-run"}, environment, false, true, child, error)) {
        log_.note("the updater did not start: " + error);
        backoff_ms_ = backoff_ms_ == 0 ? options_.request_backoff_ms : std::min(backoff_ms_ * 2, options_.request_backoff_ms * 16);
        requests_blocked_until_ = now_ms() + backoff_ms_;
        return;
    }
    log_.note("an update was asked for; the updater runs");
    updater_ = child.pid;
    control_ = child.control;
    control_buffer_.clear();
    ::fcntl(control_, F_SETFL, O_NONBLOCK);
    outputs_.push_back(Output{child.output, {}});
    updater_deadline_ = now_ms() + options_.updater_limit_ms;
}

void Supervisor::on_updater_stopped(int status) {
    updater_ = -1;
    if (control_ >= 0) ::close(control_);
    control_ = -1;
    log_.note("the updater ended (" + describe_status(status) + ")");
    // It takes the request away before anything else. One that is still
    // there means the updater could not even begin, and would fail the same
    // way if started again at once.
    if (request_waiting()) {
        backoff_ms_ = backoff_ms_ == 0 ? options_.request_backoff_ms : std::min(backoff_ms_ * 2, options_.request_backoff_ms * 16);
        requests_blocked_until_ = now_ms() + backoff_ms_;
        log_.note("the updater left the request in place; it is tried again in " +
                  std::to_string((backoff_ms_ + 999) / 1000) + " s");
    } else {
        backoff_ms_ = 0;
    }
}

void Supervisor::stop_updater() {
    if (updater_ <= 0) return;
    ::kill(updater_, SIGTERM);
    const int64_t deadline = now_ms() + 10000;
    int status = 0;
    while (true) {
        const pid_t done = ::waitpid(updater_, &status, WNOHANG);
        if (done == updater_ || (done < 0 && errno != EINTR)) break;
        if (now_ms() >= deadline) {
            ::kill(updater_, SIGKILL);
            if (!wait_for_exit(updater_, 5000, status)) log_.note("the updater does not end even when killed");
            break;
        }
        ::usleep(20000);
    }
    on_updater_stopped(status);
}

void Supervisor::reap() {
    int status = 0;
    pid_t pid;
    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == receiver_) {
            on_receiver_stopped(status);
        } else if (pid == updater_) {
            on_updater_stopped(status);
        }
        // Anything else is an orphan that ended by itself.
    }
}

void Supervisor::read_output(Output& output) {
    char buffer[8192];
    const ssize_t got = ::read(output.fd, buffer, sizeof buffer);
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (got <= 0) {
        if (!output.partial.empty()) log_.write(output.partial + "\n");
        ::close(output.fd);
        output.fd = -1;
        return;
    }
    output.partial.append(buffer, static_cast<size_t>(got));
    const size_t end = output.partial.rfind('\n');
    if (end != std::string::npos) {
        log_.write(output.partial.substr(0, end + 1));
        output.partial.erase(0, end + 1);
    }
    if (output.partial.size() > 4096) {
        log_.write(output.partial + "\n");
        output.partial.clear();
    }
}

void Supervisor::answer(const std::string& request) {
    if (request == "restart") {
        policy_.reset();
        next_start_ = -1;
        stop_receiver();
        std::string error;
        if (start_receiver(false, error)) {
            log_.note("the receiver was restarted for the updater");
            send_line(control_, "ok");
        } else {
            send_line(control_, "error " + error);
        }
    } else if (request == "service") {
        send_line(control_, "service " + std::to_string(policy_.restarts()) + " " + (policy_.failed() ? "1" : "0"));
    } else {
        send_line(control_, "error unknown request");
    }
}

void Supervisor::read_control() {
    char buffer[512];
    const ssize_t got = ::read(control_, buffer, sizeof buffer);
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (got <= 0) {
        ::close(control_);
        control_ = -1;
        return;
    }
    control_buffer_.append(buffer, static_cast<size_t>(got));
    size_t end;
    while (control_ >= 0 && (end = control_buffer_.find('\n')) != std::string::npos) {
        const std::string request = control_buffer_.substr(0, end);
        control_buffer_.erase(0, end + 1);
        answer(request);
    }
    if (control_buffer_.size() > 4096) {
        ::close(control_);
        control_ = -1;
    }
}

void Supervisor::drain_signals() {
    unsigned char numbers[64];
    ssize_t got;
    while ((got = ::read(signals_[0], numbers, sizeof numbers)) > 0) {
        for (ssize_t i = 0; i < got; ++i)
            if (numbers[i] == SIGTERM || numbers[i] == SIGINT) terminate_ = true;
    }
}

int Supervisor::run(std::string& error) {
    if (!setup(error)) return 1;
    log_.note("starting");
    boot_check();
    std::string start_error;
    if (!terminate_) start_receiver(false, start_error);
    if (options_.ready) options_.ready();

    while (!terminate_) {
        const int64_t now = now_ms();
        int64_t wait = options_.poll_ms;
        if (next_start_ >= 0) wait = std::min(wait, std::max<int64_t>(0, next_start_ - now));
        if (updater_ > 0) wait = std::min(wait, std::max<int64_t>(0, updater_deadline_ - now));

        std::vector<pollfd> fds;
        fds.push_back({signals_[0], POLLIN, 0});
        if (control_ >= 0) fds.push_back({control_, POLLIN, 0});
        const size_t first_output = fds.size();
        for (const Output& output : outputs_) fds.push_back({output.fd, POLLIN, 0});
        const int n = ::poll(fds.data(), fds.size(), static_cast<int>(wait));
        if (n < 0 && errno != EINTR) {
            error = std::string("poll failed: ") + std::strerror(errno);
            break;
        }
        if (n > 0) {
            if (fds[0].revents) drain_signals();
            for (size_t i = first_output; i < fds.size(); ++i)
                if (fds[i].revents) read_output(outputs_[i - first_output]);
            outputs_.erase(std::remove_if(outputs_.begin(), outputs_.end(), [](const Output& o) { return o.fd < 0; }),
                           outputs_.end());
            if (control_ >= 0 && fds[1].fd == control_ && fds[1].revents) read_control();
        }
        reap();
        if (terminate_) break;

        const int64_t later = now_ms();
        policy_.tick(later);
        if (next_start_ >= 0 && later >= next_start_ && receiver_ <= 0) {
            std::string ignored;
            start_receiver(true, ignored);
        }
        if (updater_ > 0 && later >= updater_deadline_) {
            log_.note("the updater ran longer than " + std::to_string(options_.updater_limit_ms / 60000) +
                      " minutes and was stopped");
            stop_updater();
        }
        if (updater_ <= 0 && later >= next_request_look_) {
            next_request_look_ = later + options_.poll_ms;
            if (later >= requests_blocked_until_ && request_waiting()) start_updater();
        }
    }

    log_.note("stopping");
    // The updater first, so that it cannot start a receiver just stopped.
    // A trial it leaves unfinished is what the boot check ends next time,
    // with the version before: unlike systemd, which keeps the update's own
    // unit running while the receiver's restarts, a restart of this service
    // ends the update.
    if (updater_ > 0) log_.note("an update is under way; it ends now, and the next start puts the trusted version back");
    stop_updater();
    stop_receiver();
    // What is left in the pipes; a writer that is somehow still there does
    // not hold the stop up for more than a second.
    const int64_t drain_until = now_ms() + 1000;
    for (Output& output : outputs_) {
        while (output.fd >= 0 && now_ms() < drain_until) {
            pollfd ready{output.fd, POLLIN, 0};
            if (::poll(&ready, 1, 100) > 0) read_output(output);
        }
        if (output.fd >= 0) ::close(output.fd);
    }
    log_.note("stopped");
    return error.empty() ? 0 : 1;
}

}  // namespace

int run_supervisor(const SupervisorOptions& options, std::string& error) {
    Supervisor supervisor(options);
    return supervisor.run(error);
}

namespace {

// systemd gives a service a hard limit of 524288 open files; sysvinit and
// OpenRC often leave 4096, which the receiver, raising only its soft limit,
// could not get past. Raised here, as root, before the capability for it
// goes with the bounding set below.
void raise_file_limit() {
    long wanted = 1 << 20;
    if (FILE* file = std::fopen("/proc/sys/fs/nr_open", "re")) {
        long most = 0;
        if (std::fscanf(file, "%ld", &most) == 1 && most > 0) wanted = std::min(wanted, most);
        std::fclose(file);
    }
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return;
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max >= static_cast<rlim_t>(wanted)) return;
    limit.rlim_max = static_cast<rlim_t>(wanted);
    limit.rlim_cur = std::min(limit.rlim_cur, limit.rlim_max);
    ::setrlimit(RLIMIT_NOFILE, &limit);
}

// What a program this process starts as root can have: the updater unit's
// CapabilityBoundingSet. It is the bounding set that an exec by root takes
// its capabilities from; this process keeps its own until it ends.
void limit_capabilities() {
    const int keep[] = {0 /* CHOWN */, 1 /* DAC_OVERRIDE */, 2 /* DAC_READ_SEARCH */, 3 /* FOWNER */,
                        5 /* KILL */,  6 /* SETGID */,       7 /* SETUID */};
    for (int capability = 0; capability < 64; ++capability) {
        if (std::find(std::begin(keep), std::end(keep), capability) != std::end(keep)) continue;
        if (::prctl(PR_CAPBSET_DROP, capability, 0, 0, 0) != 0 && errno == EINVAL) break;
    }
}

}  // namespace

int prepare_container_command() {
    // Only in the image: on a machine it would point `trusted` at this
    // program's version and remove the other releases.
    const char* container = std::getenv("FERNSDR_CONTAINER");
    struct stat image {};
    if (::geteuid() != 0 || !container || !*container || ::lstat(kContainerImageDirectory, &image) != 0 ||
        !S_ISDIR(image.st_mode)) {
        std::fprintf(stderr, "fernsdr: --prepare-container runs as root in the container image, from its entrypoint\n");
        return 2;
    }
    ::umask(022);
    ContainerSetup setup;
    setup.layout = container_layout();
    setup.image = kContainerImageDirectory;
    setup.image_version = kVersion;
    setup.keys = release_keys();
    setup.platform = release_platform();
    std::vector<std::string> report;
    std::string error;
    const bool prepared = prepare_container(setup, report, error);
    for (const std::string& line : report) std::printf("FernSDR: %s\n", line.c_str());
    std::fflush(stdout);
    if (!prepared) {
        std::fprintf(stderr, "fernsdr: updates from the admin panel cannot be set up: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

int supervise_command(bool daemon, const std::string& pidfile, const std::vector<std::string>& receiver_arguments) {
    ::signal(SIGPIPE, SIG_IGN);
    if (::geteuid() != 0) {
        std::fprintf(stderr, "fernsdr: --supervise runs as root, started by the machine's init at boot\n");
        return 2;
    }
    ::umask(022);
    raise_file_limit();
    limit_capabilities();

    SupervisorOptions options;
    options.receiver_arguments = receiver_arguments;
    for (const char* name : {"FERNSDR_UPDATE_URL", "FERNSDR_UPDATE_TRIAL_SECONDS"})
        if (const char* value = std::getenv(name)) options.updater_environment.push_back(std::string(name) + "=" + value);
    if (const char* container = std::getenv("FERNSDR_CONTAINER"); container && *container) {
        // The entrypoint ran --prepare-container; the layout is the
        // container's, and so is everything around the receiver.
        const UpdateLayout layout = container_layout();
        options.install = layout.install;
        options.state = layout.state;
        options.update = layout.update;
        options.log_directory.clear();
        options.refuse_root_group = true;
        options.updater_environment.push_back(std::string("FERNSDR_CONTAINER=") + container);
        for (char** entry = environ; entry && *entry; ++entry) options.receiver_environment.push_back(*entry);
        if (const char* usb = std::getenv("FERNSDR_USB_GID"); usb && *usb) {
            char* end = nullptr;
            const unsigned long gid = std::strtoul(usb, &end, 10);
            if (*end == '\0' && gid > 0 && gid < 4294967295UL) options.receiver_groups.push_back(static_cast<gid_t>(gid));
        }
    }

    // With --daemon the program the init started returns once the
    // supervisor runs, or with its reason when it cannot: an init that looks
    // for the pidfile right after finds it written.
    int ready[2] = {-1, -1};
    if (daemon) {
        if (::pipe2(ready, O_CLOEXEC) != 0) {
            std::perror("fernsdr: pipe");
            return 1;
        }
        const pid_t first = ::fork();
        if (first < 0) {
            std::perror("fernsdr: fork");
            return 1;
        }
        if (first > 0) {
            ::close(ready[1]);
            std::string said;
            char buffer[512];
            ssize_t got;
            while ((got = ::read(ready[0], buffer, sizeof buffer)) != 0) {
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) break;
                said.append(buffer, static_cast<size_t>(got));
                if (said == "r") break;
            }
            int status;
            ::waitpid(first, &status, 0);
            if (said == "r") return 0;
            std::fprintf(stderr, "%s\n", said.empty() ? "fernsdr: the supervisor ended at once" : said.c_str() + 1);
            return 1;
        }
        ::close(ready[0]);
        ::setsid();
        const pid_t second = ::fork();
        if (second < 0) _exit(1);
        if (second > 0) _exit(0);
        if (::chdir("/") != 0) _exit(1);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 1);
            ::dup2(null, 2);
            if (null > 2) ::close(null);
        }
    }
    auto report = [&](const std::string& text) {
        if (ready[1] >= 0) {
            const std::string message = "e" + text;
            (void)!::write(ready[1], message.data(), message.size());
            ::close(ready[1]);
            ready[1] = -1;
        } else {
            std::fprintf(stderr, "%s\n", text.c_str());
        }
    };

    int pid_fd = -1;
    if (!pidfile.empty()) {
        pid_fd = ::open(pidfile.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (pid_fd < 0 || ::flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
            report("fernsdr: " + pidfile + (pid_fd < 0 ? ": " + std::string(std::strerror(errno)) : " is held by a supervisor that runs"));
            return 1;
        }
        const std::string text = std::to_string(::getpid()) + "\n";
        if (::ftruncate(pid_fd, 0) != 0 || ::write(pid_fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
            report("fernsdr: cannot write " + pidfile);
            return 1;
        }
    }
    options.ready = [&] {
        if (ready[1] >= 0) {
            (void)!::write(ready[1], "r", 1);
            ::close(ready[1]);
            ready[1] = -1;
        }
    };
    std::string error;
    const int result = run_supervisor(options, error);
    if (result != 0) report("fernsdr: " + error);
    if (pid_fd >= 0) {
        ::unlink(pidfile.c_str());
        ::close(pid_fd);
    }
    return result;
}

}  // namespace fernsdr
