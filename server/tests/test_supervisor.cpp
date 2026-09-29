// `fernsdr --supervise`: when it starts the receiver again, what it answers
// the updater, and, as root, the whole of it against a stand-in receiver
// and updater: shell scripts that say what they were given.
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#include "../src/update/files.h"
#include "../src/update/supervisor.h"
#include "test_util.h"

using fernsdr::RestartPolicy;

TEST_CASE(supervisor_restarts_after_two_seconds_and_counts_only_its_own_starts) {
    RestartPolicy policy;
    policy.started(0, false);
    CHECK_EQ(policy.restarts(), 0);
    CHECK_EQ(policy.stopped(60000), 62000);
    policy.started(62000, true);
    CHECK_EQ(policy.restarts(), 1);
    CHECK(!policy.failed());
}

TEST_CASE(supervisor_counts_the_start_limit_as_systemd_does) {
    // Two seconds apart, five starts never fall within ten seconds.
    RestartPolicy policy;
    int64_t now = 0;
    policy.started(now, false);
    for (int i = 0; i < 10; ++i) {
        now = policy.stopped(now + 100);
        policy.started(now, true);
    }
    CHECK(!policy.failed());
    CHECK_EQ(policy.restarts(), 10);
}

TEST_CASE(supervisor_fails_past_the_start_limit_and_keeps_trying_slowly) {
    RestartPolicy::Settings settings;
    settings.delay_ms = 1000;
    RestartPolicy policy(settings);
    int64_t now = 0;
    policy.started(now, false);
    // Five starts within ten seconds are allowed; the sixth is not.
    for (int i = 0; i < 4; ++i) {
        now = policy.stopped(now + 100);
        CHECK(!policy.failed());
        policy.started(now, true);
    }
    const int64_t stopped_at = now + 100;
    CHECK_EQ(policy.stopped(stopped_at), stopped_at + 30000);
    CHECK(policy.failed());
    // Still failed after a slow retry that stops again at once.
    policy.started(stopped_at + 30000, true);
    CHECK_EQ(policy.stopped(stopped_at + 30100), stopped_at + 60100);
    CHECK(policy.failed());
    // A start that lasts ten seconds clears it.
    policy.started(stopped_at + 60100, true);
    policy.tick(stopped_at + 65000);
    CHECK(policy.failed());
    policy.tick(stopped_at + 70100);
    CHECK(!policy.failed());
    CHECK_EQ(policy.restarts(), 6);
}

TEST_CASE(supervisor_explicit_restart_starts_the_history_over) {
    RestartPolicy::Settings settings;
    settings.delay_ms = 1000;
    RestartPolicy policy(settings);
    policy.started(0, false);
    for (int i = 0; i < 5; ++i) policy.started(policy.stopped(i * 100 + 50), true);
    CHECK(policy.failed());
    policy.reset();
    CHECK(!policy.failed());
    CHECK_EQ(policy.restarts(), 0);
    policy.started(1000, false);
    CHECK_EQ(policy.stopped(1100), 2100);
}

TEST_CASE(supervisor_service_answers_parse_strictly) {
    int restarts = -1;
    bool failed = true;
    CHECK(fernsdr::parse_service_answer("service 3 0", restarts, failed));
    CHECK_EQ(restarts, 3);
    CHECK(!failed);
    CHECK(fernsdr::parse_service_answer("service 0 1", restarts, failed));
    CHECK(failed);
    CHECK(!fernsdr::parse_service_answer("service 3", restarts, failed));
    CHECK(!fernsdr::parse_service_answer("service 3 2", restarts, failed));
    CHECK(!fernsdr::parse_service_answer("service -1 0", restarts, failed));
    CHECK(!fernsdr::parse_service_answer("service 3 01", restarts, failed));
    CHECK(!fernsdr::parse_service_answer("restarts 3 0", restarts, failed));
}

TEST_CASE(supervisor_client_speaks_the_line_protocol) {
    int pair[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    std::thread server([&] {
        char buffer[64];
        std::string got;
        while (got.find('\n') == std::string::npos) {
            const ssize_t n = ::read(pair[1], buffer, sizeof buffer);
            if (n <= 0) return;
            got.append(buffer, static_cast<size_t>(n));
        }
        const char* reply = got == "service\n" ? "service 2 1\n" : "nonsense\n";
        (void)!::write(pair[1], reply, std::strlen(reply));
        got.clear();
        while (got.find('\n') == std::string::npos) {
            const ssize_t n = ::read(pair[1], buffer, sizeof buffer);
            if (n <= 0) return;
            got.append(buffer, static_cast<size_t>(n));
        }
        const char* answer = got == "restart\n" ? "error the receiver did not start\n" : "?\n";
        (void)!::write(pair[1], answer, std::strlen(answer));
    });
    int restarts = 0;
    bool failed = false;
    CHECK(fernsdr::supervisor_service(pair[0], restarts, failed));
    CHECK_EQ(restarts, 2);
    CHECK(failed);
    std::string error;
    CHECK(!fernsdr::supervisor_restart(pair[0], error));
    CHECK_EQ_STR(error, "the receiver did not start");
    server.join();
    ::close(pair[1]);
    // Nobody at the other end: an answer that never comes is a failure, not a hang.
    CHECK(!fernsdr::supervisor_restart(pair[0], error));
    ::close(pair[0]);
}

namespace {

std::string read_all(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void write_text(const std::string& path, const std::string& text, mode_t mode) {
    std::ofstream(path) << text;
    ::chmod(path.c_str(), mode);
}

bool wait_for(const std::function<bool()>& done, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

int count_lines(const std::string& text) {
    int lines = 0;
    for (char c : text) lines += c == '\n';
    return lines;
}

std::string last_line(const std::string& text) {
    const size_t end = text.find_last_not_of('\n');
    if (end == std::string::npos) return "";
    const size_t start = text.rfind('\n', end);
    return text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

bool alive(pid_t pid) { return pid > 0 && ::kill(pid, 0) == 0; }

// The receiver: says who it runs as and what it has open, starts a module
// that ignores SIGTERM the way a module stuck in a USB call would, and
// ends on SIGTERM without stopping it. With `crash` in its directory it
// fails at once.
const char* kProgram = R"SH(#!/bin/sh
state=${STATE_FOR_TEST:-$HOME}
case "$1" in
--update-boot)
    echo "boot check ran"
    exit 0 ;;
--update-run)
    ls /proc/$$/fd > "$state/updater-fds.raw"
    awk '$1 < 10' "$state/updater-fds.raw" | sort -n | tr '\n' ' ' > "$state/updater-fds"
    echo "updater environment $FERNSDR_SUPERVISOR_FD $FERNSDR_UPDATE_URL"
    echo service >&3; read answer <&3; echo "updater got $answer"
    echo restart >&3; read answer <&3; echo "updater restart $answer"
    echo service >&3; read answer <&3; echo "updater then got $answer"
    [ -e "$state/keep-request" ] || rm -f "$state/update-request"
    exit 0 ;;
esac
[ -e "$state/crash" ] && exit 1
# Listed before any pipeline: while bash sets one up, the shell itself holds
# the pipes' ends for a moment, and ls could see them. "started" comes last,
# so that a test which sees it finds the list complete.
ls /proc/$$/fd > "$state/fds.raw"
awk '$1 < 10' "$state/fds.raw" | sort -n | tr '\n' ' ' > "$state/fds"
echo "$(id -u) $$ $(pwd) $HOME $FERNSDR_UPDATE_URL" >> "$state/started"
sh -c 'trap "" TERM; while :; do sleep 1; done' &
echo $! > "$state/module"
trap 'exit 0' TERM
while :; do sleep 0.05; done
)SH";

}  // namespace

TEST_CASE(supervisor_runs_the_receiver_restarts_it_and_answers_the_updater) {
    if (::geteuid() != 0) return;
    const int failures_before = test::failures();
    // qemu-user (make release-test on ARM) refuses PR_SET_CHILD_SUBREAPER,
    // so the modules a killed receiver leaves go to init there, where the
    // supervisor cannot see them; Linux on ARM hands them to it as here.
    const bool adopts_orphans = !std::getenv("FERNSDR_TEST_UNDER_QEMU");
    char name[] = "/tmp/fernsdr-supervise-XXXXXX";
    CHECK(::mkdtemp(name) != nullptr);
    const std::string root = name;
    ::chmod(name, 0755);
    const std::string install = root + "/install", state = root + "/state", update = root + "/update",
                      logs = root + "/log";
    for (const std::string& d : {install, install + "/releases", install + "/releases/1", install + "/releases/1/web",
                                 state, update})
        ::mkdir(d.c_str(), 0755);
    CHECK(::symlink("releases/1", (install + "/current").c_str()) == 0);
    CHECK(::symlink("releases/1", (install + "/trusted").c_str()) == 0);
    write_text(install + "/releases/1/fernsdr", kProgram, 0755);
    CHECK(::chown(state.c_str(), 65534, 65534) == 0);

    fernsdr::SupervisorOptions options;
    options.install = install;
    options.state = state;
    options.update = update;
    options.log_directory = logs;
    options.restart.delay_ms = 100;
    options.restart.window_ms = 1000;
    options.restart.burst = 3;
    options.restart.failed_retry_ms = 500;
    options.restart.stable_ms = 400;
    options.poll_ms = 50;
    options.stop_ms = 2000;
    options.orphan_stop_ms = 300;
    options.request_backoff_ms = 400;
    options.updater_environment = {"FERNSDR_UPDATE_URL=https://releases.test/"};

    int ready[2];
    CHECK(::pipe(ready) == 0);
    const pid_t supervisor = ::fork();
    if (supervisor == 0) {
        ::close(ready[0]);
        // The receiver finds its directory as HOME; the updater, which gets
        // no HOME, is told it here.
        options.updater_environment.push_back("STATE_FOR_TEST=" + state);
        options.ready = [&] {
            (void)!::write(ready[1], "r", 1);
            ::close(ready[1]);
        };
        std::string error;
        const int result = fernsdr::run_supervisor(options, error);
        if (result != 0) std::fprintf(stderr, "supervisor: %s\n", error.c_str());
        _exit(result);
    }
    ::close(ready[1]);
    char byte = 0;
    CHECK(::read(ready[0], &byte, 1) == 1 && byte == 'r');
    ::close(ready[0]);

    const std::string started = state + "/started";
    CHECK(wait_for([&] { return count_lines(read_all(started)) >= 1; }, 3000));
    const std::string log = logs + "/fernsdr.log";
    CHECK(read_all(log).find("boot check ran") != std::string::npos);

    std::string first = last_line(read_all(started));
    unsigned uid = 0;
    int pid = 0;
    char directory[512] = {0}, home[512] = {0}, url[512] = {0};
    CHECK(std::sscanf(first.c_str(), "%u %d %511s %511s %511s", &uid, &pid, directory, home, url) == 5);
    CHECK_EQ_STR(std::string(url), "https://releases.test/");
    CHECK_EQ(uid, 65534u);
    CHECK_EQ_STR(std::string(directory), state);
    CHECK_EQ_STR(std::string(home), state);
    CHECK_EQ_STR(read_all(state + "/fds"), "0 1 2 ");

    // Killed: the module it left is stopped, and it is back.
    CHECK(wait_for([&] { return !read_all(state + "/module").empty(); }, 2000));
    const pid_t module = std::atoi(read_all(state + "/module").c_str());
    CHECK(alive(module));
    ::kill(pid, SIGKILL);
    CHECK(wait_for([&] { return count_lines(read_all(started)) >= 2; }, 8000));
    if (adopts_orphans) CHECK(wait_for([&] { return !alive(module); }, 1000));

    // An update request: the updater starts from trusted, with the socket at
    // 3 and nothing else, is told how the receiver fares, has it restarted,
    // and the restart it asked for is not counted as the receiver's.
    write_text(state + "/update-request", "0.1.1\n", 0644);
    CHECK(wait_for([&] { return read_all(log).find("updater then got") != std::string::npos; }, 8000));
    const std::string text = read_all(log);
    CHECK_EQ_STR(read_all(state + "/updater-fds"), "0 1 2 3 ");
    CHECK(text.find("updater environment 3 https://releases.test/") != std::string::npos);
    CHECK(text.find("updater got service 1 0") != std::string::npos);
    CHECK(text.find("updater restart ok") != std::string::npos);
    CHECK(text.find("updater then got service 0 0") != std::string::npos);
    CHECK(wait_for([&] { return count_lines(read_all(started)) >= 3; }, 3000));
    CHECK(wait_for([&] { return read_all(log).find("the updater ended (exit status 0)") != std::string::npos; }, 3000));

    // An updater that leaves the request is not started again at once.
    write_text(state + "/keep-request", "", 0644);
    write_text(state + "/update-request", "0.1.1\n", 0644);
    CHECK(wait_for([&] { return read_all(log).find("tried again in") != std::string::npos; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const size_t runs = [&] {
        size_t count = 0, at = 0;
        const std::string all = read_all(log);
        while ((at = all.find("an update was asked for", at)) != std::string::npos) ++count, ++at;
        return count;
    }();
    CHECK_EQ(runs, 2u);
    ::unlink((state + "/update-request").c_str());
    ::unlink((state + "/keep-request").c_str());

    // A receiver that keeps failing is started less often, and comes back
    // once it can.
    write_text(state + "/crash", "", 0644);
    std::sscanf(last_line(read_all(started)).c_str(), "%u %d", &uid, &pid);
    ::kill(pid, SIGKILL);
    CHECK(wait_for([&] { return read_all(log).find("from now on it is tried every") != std::string::npos; }, 5000));
    const int before = count_lines(read_all(started));
    ::unlink((state + "/crash").c_str());
    CHECK(wait_for([&] { return count_lines(read_all(started)) > before; }, 3000));

    // SIGTERM: the receiver and what it started are gone, and it returns 0.
    CHECK(wait_for([&] { return !read_all(state + "/module").empty(); }, 2000));
    const pid_t last_module = std::atoi(read_all(state + "/module").c_str());
    std::sscanf(last_line(read_all(started)).c_str(), "%u %d", &uid, &pid);
    ::kill(supervisor, SIGTERM);
    int status = -1;
    CHECK(::waitpid(supervisor, &status, 0) == supervisor);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(!alive(pid));
    if (adopts_orphans) CHECK(!alive(last_module));
    if (!adopts_orphans) {
        // Nobody stopped them there: not left running after the test.
        ::kill(module, SIGKILL);
        ::kill(last_module, SIGKILL);
    }
    CHECK(read_all(log).find("supervise: stopped") != std::string::npos);

    // A second supervisor on the same directories is refused while one runs.
    const pid_t again = ::fork();
    if (again == 0) {
        fernsdr::SupervisorOptions same = options;
        std::string error;
        const int holder = ::open((update + "/supervise.lock").c_str(), O_RDWR);
        if (holder >= 0) ::flock(holder, LOCK_EX);  // stands in for a running supervisor
        const pid_t inner = ::fork();
        if (inner == 0) {
            const int result = fernsdr::run_supervisor(same, error);
            _exit(result == 1 && error.find("another fernsdr --supervise") != std::string::npos ? 0 : 1);
        }
        int inner_status = -1;
        ::waitpid(inner, &inner_status, 0);
        _exit(WIFEXITED(inner_status) ? WEXITSTATUS(inner_status) : 1);
    }
    CHECK(::waitpid(again, &status, 0) == again);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (test::failures() > failures_before) std::fprintf(stderr, "--- supervisor log\n%s---\n", read_all(logs + "/fernsdr.log").c_str());
    const int parent = fernsdr::open_directory("/tmp");
    std::string error;
    fernsdr::remove_tree_at(parent, root.substr(5), error);
    ::close(parent);
}
