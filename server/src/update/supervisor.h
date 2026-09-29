// `fernsdr --supervise`: what systemd's units do, for a machine without
// systemd. One process of root's, started by whatever init the machine has,
// that
//   - ends an interrupted update trial before anything else
//     (fernsdr-update-boot.service),
//   - runs the receiver as the owner of its state directory and starts it
//     again two seconds after it stops (fernsdr.service),
//   - starts the updater, from the trusted version, when the receiver leaves
//     an update request (fernsdr-update.path and fernsdr-update.service), and
//     answers the updater's two questions about the receiver over a socket:
//     restart it, and how often has it restarted on its own.
// The update itself stays in the updater: this process only reads whether
// the request file exists, never what is in it.
//
// It does not reproduce the units' sandbox (namespaces, the system call
// filter, device policy); DEPLOYMENT.md says so. What it does keep: the
// receiver runs without root and with no way back to it, and programs it
// starts as root get only the updater unit's capabilities.
#pragma once
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fernsdr {

// When the receiver is started again. It mirrors fernsdr.service: two
// seconds after it stops, and failed once a start would be the sixth within
// ten seconds, systemd's start limit, which with the delay of two seconds
// only a receiver that dies at once runs into. Unlike a unit that reached it, a failed
// receiver is still tried every thirty seconds, since nobody may be there to
// reset it; `failed` holds until a start has lasted ten seconds or the
// updater asks for a restart.
class RestartPolicy {
public:
    struct Settings {
        int64_t delay_ms = 2000;
        int64_t window_ms = 10000;
        int burst = 5;
        int64_t failed_retry_ms = 30000;
        int64_t stable_ms = 10000;
    };

    RestartPolicy() = default;
    explicit RestartPolicy(Settings settings) : settings_(settings) {}

    // The receiver was started; `automatic` when this policy chose the time.
    void started(int64_t now_ms, bool automatic);
    // It stopped without being asked to; when to start it again.
    int64_t stopped(int64_t now_ms);
    // Clears `failed` once the running receiver has lasted.
    void tick(int64_t now_ms);
    // An explicit restart, the updater's: history and `failed` start over,
    // and the stop it causes is not one of the receiver's.
    void reset();

    int restarts() const { return restarts_; }
    bool failed() const { return failed_; }

private:
    Settings settings_;
    std::vector<int64_t> starts_;
    int64_t running_since_ = -1;
    int restarts_ = 0;
    bool failed_ = false;
};

// The updater's side of the socket, one request and one answer per line:
//   restart\n  ->  ok\n  |  error <text>\n
//   service\n  ->  service <restarts> <0|1>\n
constexpr const char* kSupervisorFdVariable = "FERNSDR_SUPERVISOR_FD";

// The descriptor from the environment, made close-on-exec and taken out of
// the environment, so that nothing the updater starts inherits a line to a
// process of root's. -1 when this updater was not started by a supervisor.
int take_supervisor_fd();
bool supervisor_restart(int fd, std::string& error);
bool supervisor_service(int fd, int& restarts, bool& failed);

// Parses "service <n> <0|1>"; exposed for the tests.
bool parse_service_answer(const std::string& line, int& restarts, bool& failed);

struct SupervisorOptions {
    std::string install = "/opt/fernsdr";         // current/, trusted/
    std::string state = "/var/lib/fernsdr";       // the receiver's; update-request
    std::string update = "/var/lib/fernsdr-update";  // root's; the lock
    std::string log_directory = "/var/log/fernsdr";
    // Past this a log file becomes <name>.1, so at most twice it is kept.
    size_t log_limit = 1 << 20;
    RestartPolicy::Settings restart;
    int64_t poll_ms = 1000;             // how often the request file is looked for
    int64_t stop_ms = 20000;            // SIGTERM to SIGKILL
    int64_t orphan_stop_ms = 5000;      // the same for what the receiver left behind
    int64_t updater_limit_ms = 40 * 60 * 1000;  // the update unit's TimeoutStartSec
    int64_t boot_limit_ms = 5 * 60 * 1000;
    // After an updater left its request in place: wait this long, doubling
    // to 16 times, before starting another.
    int64_t request_backoff_ms = 60000;
    // Tests run as root with a state directory of root's.
    bool state_may_be_roots = false;
    // Passed on to the updater, from the supervisor's own environment:
    // FERNSDR_UPDATE_URL and FERNSDR_UPDATE_TRIAL_SECONDS. The URL goes to
    // the receiver too, whose Updates page looks for releases there.
    std::vector<std::string> updater_environment;
    // Called once the boot check has run and the receiver was started.
    std::function<void()> ready;
};

// Runs until SIGTERM or SIGINT; then stops the receiver, and the updater if
// one runs, and returns 0. Non-zero, with the reason in `error`, when it
// cannot start at all (another supervisor holds the lock, a directory is
// wrong).
int run_supervisor(const SupervisorOptions& options, std::string& error);

// `fernsdr --supervise [--daemon] [--pidfile PATH]`, as root.
int supervise_command(bool daemon, const std::string& pidfile);

}  // namespace fernsdr
