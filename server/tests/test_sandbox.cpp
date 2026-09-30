#include "../src/util/sandbox.h"
#include "../src/util/sandbox_filter.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>

namespace {

bool sandbox_available() {
    if (!std::getenv("FERNSDR_TEST_UNDER_QEMU")) return true;
    static const bool available = [] {
        const pid_t pid = ::fork();
        if (pid == 0) {
            sock_filter filter[] = {BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
            sock_fprog program{1, filter};
            ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
            const int result = ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0);
            if (result != 0) std::fprintf(stderr, "  qemu guest PR_SET_SECCOMP: %s; kernel enforcement tests unavailable\n", std::strerror(errno));
            ::_exit(result == 0 ? 0 : 1);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }();
    return available;
}

bool deny_syscall(uint32_t nr, uint32_t error) {
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | error),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{4, filter};
    return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
        ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) == 0;
}

bool deny_prctl(uint32_t option) {
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prctl, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, option, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{6, filter};
    return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
        ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) == 0;
}

// Runs `check` in a child with the decoder sandbox applied and returns its
// exit status: 0 when every expectation held, else which one failed.
template <typename Check>
int in_sandbox(Check check, fernsdr::SandboxReport* seen = nullptr, bool without_landlock = false) {
    int report_pipe[2];
    if (::pipe(report_pipe) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(report_pipe[0]);
        if (without_landlock && !deny_syscall(444, ENOSYS)) ::_exit(90);
        const fernsdr::SandboxReport report = fernsdr::apply_decoder_sandbox("/bin/true");
        const int abi = report.landlock_abi;
        (void)!::write(report_pipe[1], &abi, sizeof(abi));
        ::_exit(check(report));
    }
    ::close(report_pipe[1]);
    int abi = 0;
    (void)!::read(report_pipe[0], &abi, sizeof(abi));
    ::close(report_pipe[0]);
    if (seen) seen->landlock_abi = abi;
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 100;
}

}  // namespace

TEST_CASE(a_decoder_sandbox_denies_internet_socket_creation_on_every_landlock_abi) {
    if (!sandbox_available()) return;
    CHECK_EQ(in_sandbox([](const fernsdr::SandboxReport& report) {
        if (!report.seccomp) return 3;
        for (const int family : {AF_INET, AF_INET6}) {
            for (const int type : {SOCK_DGRAM, SOCK_STREAM}) {
                const int fd = ::socket(family, type, 0);
                if (fd >= 0) { ::close(fd); return 1; }
                if (errno != EPERM) return 2;
            }
        }
        return 0;
    }), 0);
}

TEST_CASE(a_decoder_sandbox_takes_away_privileges_files_and_tcp) {
    if (!sandbox_available()) return;
    fernsdr::SandboxReport report;
    const int result = in_sandbox([](const fernsdr::SandboxReport& applied) {
        if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 1;
        if (applied.landlock_abi < 1) return 0;  // nothing more to check on this kernel
        // A file anyone may read, outside what the sandbox allows.
        const int fd = ::open("/etc/hostname", O_RDONLY);
        if (fd >= 0 || errno != EACCES) return 2;
        if (!applied.seccomp || !applied.network_closed) return 3;
        return 0;
    }, &report);
    CHECK_EQ(result, 0);
    // This machine has Landlock; say so if a test machine does not, rather
    // than passing quietly without having checked the part that matters.
    if (report.landlock_abi < 1) fprintf(stderr, "  note: no Landlock here, file restrictions unavailable\n");
}

TEST_CASE(a_decoder_sandbox_still_lets_the_decoder_itself_run) {
    if (!sandbox_available()) return;
    const pid_t pid = ::fork();
    if (pid == 0) {
        char program[] = "/bin/true";
        char* arguments[] = {const_cast<char*>("fernsdr"), const_cast<char*>("--sandbox-exec"), program, nullptr};
        ::_exit(fernsdr::sandbox_exec_command(3, arguments, 2));
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(a_decoder_sandbox_keeps_unix_ipc_and_its_own_signals) {
    if (!sandbox_available()) return;
    const int result = in_sandbox([](const fernsdr::SandboxReport& report) {
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return 2;
        ::close(fd);
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 3;
        ::close(pair[0]); ::close(pair[1]);
        if (::socketpair(AF_INET, SOCK_STREAM, 0, pair) != -1 || errno != EPERM) return 4;
        const pid_t pid = ::getpid();
        if (::syscall(SYS_kill, pid, 0) != 0) return 5;
        if (::syscall(SYS_tgkill, pid, ::syscall(SYS_gettid), 0) != 0) return 6;
        for (const pid_t target : {::getppid(), 0, -1, -pid}) {
            if (::syscall(SYS_kill, target, 0) != -1 || errno != EPERM) return 7;
        }
        if (::syscall(SYS_tgkill, ::getppid(), ::getppid(), 0) != -1 || errno != EPERM) return 8;
        return report.seccomp ? 0 : 1;
    }, nullptr, true);
    CHECK_EQ(result, 0);
}

TEST_CASE(a_decoder_sandbox_keeps_seccomp_without_landlock) {
    if (!sandbox_available()) return;
    CHECK_EQ(in_sandbox([](const fernsdr::SandboxReport& report) {
        if (!report.seccomp || !report.network_closed || report.files_closed || report.landlock_abi != 0) return 1;
        if (report.problem.find("no Landlock") == std::string::npos) return 2;
        const int fd = ::open("/etc/hostname", O_RDONLY);
        if (fd < 0) return 3;
        ::close(fd);
        if (::socket(AF_INET, SOCK_DGRAM, 0) != -1 || errno != EPERM) return 4;
        return 0;
    }, nullptr, true), 0);
}

TEST_CASE(a_decoder_sandbox_refuses_to_exec_when_seccomp_or_no_new_privs_fails) {
    if (!sandbox_available()) return;
    // Each child has its own outer filter. Refusing prctl here faults the
    // real launcher without a production switch that could weaken it.
    for (const uint32_t blocked : {PR_SET_NO_NEW_PRIVS, PR_SET_SECCOMP}) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            if (!deny_prctl(blocked)) ::_exit(90);
            char program[] = "/bin/true";
            char* arguments[] = {const_cast<char*>("fernsdr"), program, nullptr};
            ::_exit(fernsdr::sandbox_exec_command(2, arguments, 1));
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 126);
    }
}

// Interpret only the instructions the production filter uses. This checks
// the cross-compiled target's numbers even where qemu cannot install BPF.
uint32_t filter_result(const std::vector<sock_filter>& filter, const seccomp_data& data) {
    uint32_t accumulator = 0;
    for (size_t pc = 0; pc < filter.size(); pc++) {
        const sock_filter& instruction = filter[pc];
        switch (instruction.code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (instruction.k > sizeof(data) - sizeof(accumulator)) return SECCOMP_RET_KILL;
            std::memcpy(&accumulator, reinterpret_cast<const char*>(&data) + instruction.k, sizeof(accumulator));
            break;
        case BPF_JMP | BPF_JEQ | BPF_K:
            pc += accumulator == instruction.k ? instruction.jt : instruction.jf;
            break;
        case BPF_JMP | BPF_JGE | BPF_K:
            pc += accumulator >= instruction.k ? instruction.jt : instruction.jf;
            break;
        case BPF_RET | BPF_K: return instruction.k;
        default: return SECCOMP_RET_KILL;
        }
    }
    return SECCOMP_RET_KILL;
}

TEST_CASE(a_decoder_sandbox_filter_checks_the_target_architecture_and_syscalls) {
    using namespace fernsdr::sandbox_detail;
    const auto filter = decoder_filter(1234);
    seccomp_data data{};
    data.arch = kAuditArch;
    const auto result = [&](int nr, uint64_t argument) {
        data.nr = nr;
        data.args[0] = argument;
        return filter_result(filter, data);
    };
    constexpr uint32_t denied = SECCOMP_RET_ERRNO | EPERM;
#if defined(__x86_64__)
    constexpr int socket_nr = 41, pair_nr = 53, kill_nr = 62, tgkill_nr = 234;
    CHECK_EQ(result(0x40000000 | socket_nr, AF_INET), denied);
    CHECK_EQ(result(512, 1234), denied);
    CHECK_EQ(result(547, 1234), denied);
#elif defined(__aarch64__)
    constexpr int socket_nr = 198, pair_nr = 199, kill_nr = 129, tgkill_nr = 131;
#elif defined(__arm__)
    constexpr int socket_nr = 281, pair_nr = 288, kill_nr = 37, tgkill_nr = 268;
    for (const int nr : {102, 113, 0x900066, 0x900071, 0x900000 + socket_nr}) CHECK_EQ(result(nr, AF_INET), denied);
#endif
    for (const int nr : {socket_nr, pair_nr}) {
        for (const int family : {AF_INET, AF_INET6, AF_NETLINK, AF_PACKET}) CHECK_EQ(result(nr, family), denied);
        CHECK_EQ(result(nr, AF_UNIX), static_cast<uint32_t>(SECCOMP_RET_ALLOW));
        CHECK_EQ(result(nr, (1ull << 32) | AF_INET), denied);
    }
    for (const int nr : {kill_nr, tgkill_nr, SYS_rt_sigqueueinfo, SYS_rt_tgsigqueueinfo}) {
        CHECK_EQ(result(nr, 1234), static_cast<uint32_t>(SECCOMP_RET_ALLOW));
        for (const int target : {4321, 0, -1, -1234}) CHECK_EQ(result(nr, static_cast<uint64_t>(target)), denied);
    }
    for (const int nr : {424, 425, 438, SYS_tkill, SYS_ptrace}) CHECK_EQ(result(nr, 1234), denied);
    CHECK_EQ(result(SYS_read, 4), static_cast<uint32_t>(SECCOMP_RET_ALLOW));
    CHECK_EQ(result(SYS_write, 3), static_cast<uint32_t>(SECCOMP_RET_ALLOW));
    data.arch ^= 1;
    CHECK_EQ(result(socket_nr, AF_UNIX), static_cast<uint32_t>(SECCOMP_RET_KILL));
}
