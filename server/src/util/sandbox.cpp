#include "sandbox.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern char** environ;

namespace fernsdr {

namespace {

// Landlock's interface, from the kernel's uapi (linux/landlock.h). Defined
// here rather than included because the build machines' headers can be older
// than the kernels the receiver runs on; the numbers are a stable ABI, and the
// system calls are the same on x86_64, aarch64 and 32-bit ARM.
constexpr long kCreateRuleset = 444;
constexpr long kAddRule = 445;
constexpr long kRestrictSelf = 446;
constexpr uint32_t kCreateRulesetVersion = 1u << 0;
constexpr int kRulePathBeneath = 1;
constexpr int kRuleNetPort = 2;

constexpr uint64_t kExecute = 1ull << 0;
constexpr uint64_t kReadFile = 1ull << 2;
constexpr uint64_t kReadDir = 1ull << 3;

struct RulesetAttr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
};

struct __attribute__((packed)) PathBeneathAttr {
    uint64_t allowed_access;
    int32_t parent_fd;
};

// Every file right the ABI knows: v1 has thirteen, v2 adds REFER, v3
// TRUNCATE, v5 IOCTL_DEV. Handling one the kernel does not know fails the
// whole ruleset, so the set grows with the ABI.
uint64_t file_rights(int abi) {
    uint64_t rights = (1ull << 13) - 1;
    if (abi >= 2) rights |= 1ull << 13;
    if (abi >= 3) rights |= 1ull << 14;
    if (abi >= 5) rights |= 1ull << 15;
    return rights;
}

bool allow(int ruleset, const char* path, uint64_t rights) {
    const int fd = ::open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return false;
    PathBeneathAttr rule{rights, fd};
    const bool ok = ::syscall(kAddRule, ruleset, kRulePathBeneath, &rule, 0) == 0;
    ::close(fd);
    return ok;
}

}  // namespace

SandboxReport apply_decoder_sandbox(const std::string& executable) {
    SandboxReport report;
    ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    // The lowest priority there is: time the audio needs is never spent here.
    ::setpriority(PRIO_PROCESS, 0, 19);
    sched_param idle{};
    ::sched_setscheduler(0, SCHED_IDLE, &idle);

    const long abi = ::syscall(kCreateRuleset, nullptr, 0, kCreateRulesetVersion);
    if (abi < 1) {
        report.problem = errno == ENOSYS || errno == EOPNOTSUPP
                             ? "this kernel has no Landlock, so the decoder can still read files the receiver can"
                             : std::string("Landlock is unavailable: ") + std::strerror(errno);
        return report;
    }
    report.landlock_abi = static_cast<int>(abi);
    RulesetAttr attr{file_rights(report.landlock_abi), 0};
    size_t size = sizeof(uint64_t);
    if (report.landlock_abi >= 4) {
        // TCP bind and connect, with no port allowed: no network at all over
        // TCP. UDP is not covered by Landlock; the decoder has no reason to
        // use it, and PR_SET_NO_NEW_PRIVS already keeps it from gaining the
        // rights raw sockets would need.
        attr.handled_access_net = (1ull << 0) | (1ull << 1);
        size = sizeof(RulesetAttr);
    }
    const int ruleset = static_cast<int>(::syscall(kCreateRuleset, &attr, size, 0));
    if (ruleset < 0) {
        report.problem = std::string("cannot create a Landlock ruleset: ") + std::strerror(errno);
        return report;
    }
    // The decoder itself, to be executed; and for one that is not linked
    // statically, the system's libraries and the loader's cache, read only.
    bool ok = allow(ruleset, executable.c_str(), kExecute | kReadFile);
    for (const char* library : {"/lib", "/lib64", "/usr/lib", "/usr/lib64", "/etc/ld.so.cache"}) {
        allow(ruleset, library, kExecute | kReadFile | kReadDir);
    }
    (void)kRuleNetPort;
    if (!ok) {
        report.problem = "cannot allow the decoder's own executable in the Landlock ruleset";
        ::close(ruleset);
        return report;
    }
    if (::syscall(kRestrictSelf, ruleset, 0) != 0) {
        report.problem = std::string("cannot apply the Landlock ruleset: ") + std::strerror(errno);
        ::close(ruleset);
        return report;
    }
    ::close(ruleset);
    report.files_closed = true;
    report.network_closed = report.landlock_abi >= 4;
    return report;
}

int sandbox_exec_command(int argc, char** argv, int first) {
    if (first >= argc || argv[first][0] != '/') {
        std::fprintf(stderr, "--sandbox-exec needs the absolute path of a program to run\n");
        return 2;
    }
    const SandboxReport report = apply_decoder_sandbox(argv[first]);
    // On fd 2, which is the decoder's log: FernSDR shows it to the operator.
    if (!report.problem.empty()) std::fprintf(stderr, "sandbox: %s\n", report.problem.c_str());
    std::vector<char*> arguments(argv + first, argv + argc);
    arguments.push_back(nullptr);
    ::execve(argv[first], arguments.data(), environ);
    std::fprintf(stderr, "sandbox: cannot run %s: %s\n", argv[first], std::strerror(errno));
    return 127;
}

}  // namespace fernsdr
