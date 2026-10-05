#include "system.h"
#include "supervisor.h"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../util/programs.h"
#include "../util/subprocess.h"
#include "../version.h"
#include "autostart.h"
#include "release_keys.h"

namespace fernsdr {

namespace {

std::string first_line(const std::string& text) { return text.substr(0, std::min(text.find('\n'), size_t{200})); }

}  // namespace

bool fetch_release_file(const std::string& url, size_t limit, std::string& body, std::string& error) {
    const std::string curl = find_program("curl");
    const std::string wget = curl.empty() ? find_program("wget") : "";
    if (curl.empty() && wget.empty()) {
        error = "neither curl nor wget is installed";
        return false;
    }
    // A release archive of some megabytes on a slow line gets ten minutes.
    const int seconds = limit > (1u << 20) ? 600 : 60;
    std::vector<std::string> arguments;
    if (!curl.empty()) {
        // -q first: it makes curl ignore any .curlrc, so these arguments are
        // the whole of what it does.
        arguments = {"-q", "--silent", "--show-error", "--fail", "--location", "--max-redirs", "5",
                     "--proto", "=https", "--proto-redir", "=https", "--max-time", std::to_string(seconds),
                     "--max-filesize", std::to_string(limit), "--user-agent", "FernSDR", "--output", "-", url};
    } else {
        // What GNU wget and BusyBox's both take. Neither can be held to HTTPS
        // across redirects; the signature and the hash vouch for what
        // arrives, however it came.
        arguments = {"-q", "-O", "-", "-T", std::to_string(seconds), url};
    }
    std::string errors;
    Subprocess::Exit status;
    if (!Subprocess::run(curl.empty() ? wget : curl, arguments, network_helper_environment(),
                         std::chrono::seconds(seconds + 5), limit, body, errors, status, error)) {
        return false;
    }
    if (!status.exited || status.code != 0) {
        const std::string detail = first_line(errors);
        error = std::string(curl.empty() ? "wget " : "curl ") + status.describe() +
                (detail.empty() ? "" : " (" + detail + ")");
        return false;
    }
    return true;
}

// The new version's own check of the configuration, run as the receiver's
// user: it reads that user's files and nothing else, and root is gone from
// the process for good before the program starts. It runs inside the update
// unit's sandbox, which the receiver's is no looser than.
bool check_configuration_as(const std::string& program, const std::string& config, const std::string& web,
                            uid_t uid, gid_t gid, std::string& output) {
    output.clear();
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        output = "cannot make a pipe";
        return false;
    }
    // Everything the child needs is built before fork: after it, only calls
    // that are safe in a copy of this process are made.
    const std::string check = "--check", root = "--root";
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>(check.c_str()),
                          const_cast<char*>(config.c_str()), const_cast<char*>(root.c_str()),
                          const_cast<char*>(web.c_str()), nullptr};
    static char path_variable[] = "PATH=/usr/bin:/bin";
    static char lang_variable[] = "LANG=C.UTF-8";
    char* const envp[] = {path_variable, lang_variable, nullptr};
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        output = "cannot start the check";
        return false;
    }
    if (pid == 0) {
        const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null < 0 || ::dup2(null, 0) < 0 || ::dup2(pipe_fds[1], 1) < 0 || ::dup2(pipe_fds[1], 2) < 0) _exit(126);
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) _exit(126);
        if (::setuid(0) == 0) _exit(126);
        ::execve(program.c_str(), argv, envp);
        _exit(127);
    }
    ::close(pipe_fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool killed = false;
    char buffer[4096];
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd ready{pipe_fds[0], POLLIN, 0};
        const int n = ::poll(&ready, 1, static_cast<int>(std::min<int64_t>(left.count(), 1000)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) continue;
        const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (output.size() < 8192) output.append(buffer, static_cast<size_t>(got));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    if (killed) {
        output = "the check did not finish within a minute";
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 126 && output.empty()) output = "cannot become the receiver's user";
    if (output.empty()) output = "the check failed";
    return false;
}

namespace {

bool systemctl(const std::vector<std::string>& arguments, std::string& output, std::string& error) {
    const std::string program = find_program("systemctl");
    if (program.empty()) {
        error = "systemctl is not installed";
        return false;
    }
    std::string errors;
    Subprocess::Exit status;
    if (!Subprocess::run(program, arguments, {"PATH=/usr/bin:/bin", "LANG=C.UTF-8"}, std::chrono::seconds(120),
                         64 * 1024, output, errors, status, error)) {
        return false;
    }
    if (!status.exited || status.code != 0) {
        error = "systemctl " + status.describe() + (errors.empty() ? "" : " (" + first_line(errors) + ")");
        return false;
    }
    return true;
}

}  // namespace

bool release_base_url(std::string& url, std::string& error) {
    url = kReleaseBaseUrl;
    if (const char* set = std::getenv("FERNSDR_UPDATE_URL")) {
        const std::string value = set;
        if (value.rfind("https://", 0) != 0 || value.back() != '/') {
            error = "FERNSDR_UPDATE_URL has to be an https:// address ending in /";
            return false;
        }
        url = value;
    }
    return true;
}

bool system_update_environment(UpdateEnvironment& environment, std::string& error) {
    environment.keys = release_keys();
    environment.platform = release_platform();
    environment.running_version = kVersion;
    if (!release_base_url(environment.base_url, error)) return false;
    if (const char* seconds = std::getenv("FERNSDR_UPDATE_TRIAL_SECONDS")) {
        char* end = nullptr;
        const long value = std::strtol(seconds, &end, 10);
        // Not under the minute a new version serves before it says it works,
        // and well within the update unit's own time limit.
        if (!*seconds || *end || value < 90 || value > 900) {
            error = "FERNSDR_UPDATE_TRIAL_SECONDS has to be from 90 to 900";
            return false;
        }
        environment.trial_ms = value * 1000;
    }
    environment.fetch = fetch_release_file;
    environment.check = check_configuration_as;
    environment.autostart = set_autostart;
    // Started by `fernsdr --supervise` rather than by systemd: the
    // supervisor runs the receiver and answers for it.
    if (const int supervisor = take_supervisor_fd(); supervisor >= 0) {
        environment.restart = [supervisor](std::string& why) { return supervisor_restart(supervisor, why); };
        environment.service = [supervisor](int& restarts, bool& failed) {
            return supervisor_service(supervisor, restarts, failed);
        };
    } else {
        environment.restart = [](std::string& why) {
            // A version that crashed over and over during its trial can leave the
            // service at systemd's start limit, which a restart alone would not
            // get past.
            std::string output;
            return systemctl({"reset-failed", "fernsdr.service"}, output, why) &&
                   systemctl({"restart", "fernsdr.service"}, output, why);
        };
        environment.service = [](int& restarts, bool& failed) {
            std::string output, why;
            if (!systemctl({"show", "fernsdr.service", "--property=NRestarts,ActiveState"}, output, why)) return false;
            restarts = 0;
            failed = false;
            size_t at = 0;
            while (at < output.size()) {
                size_t end = output.find('\n', at);
                if (end == std::string::npos) end = output.size();
                const std::string line = output.substr(at, end - at);
                if (line.rfind("NRestarts=", 0) == 0) restarts = std::atoi(line.c_str() + 10);
                if (line == "ActiveState=failed") failed = true;
                at = end + 1;
            }
            return true;
        };
    }
    environment.now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    environment.sleep_ms = [](int64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    return true;
}

}  // namespace fernsdr
