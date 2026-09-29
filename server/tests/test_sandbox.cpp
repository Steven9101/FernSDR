#include "../src/util/sandbox.h"
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

namespace {

// Runs `check` in a child with the decoder sandbox applied and returns its
// exit status: 0 when every expectation held, else which one failed.
template <typename Check>
int in_sandbox(Check check, fernsdr::SandboxReport* seen = nullptr) {
    int report_pipe[2];
    if (::pipe(report_pipe) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(report_pipe[0]);
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

TEST_CASE(a_decoder_sandbox_takes_away_privileges_files_and_tcp) {
    fernsdr::SandboxReport report;
    const int result = in_sandbox([](const fernsdr::SandboxReport& applied) {
        if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 1;
        if (applied.landlock_abi < 1) return 0;  // nothing more to check on this kernel
        // A file anyone may read, outside what the sandbox allows.
        const int fd = ::open("/etc/hostname", O_RDONLY);
        if (fd >= 0 || errno != EACCES) return 2;
        if (applied.landlock_abi >= 4) {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(9);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (s >= 0 && ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) return 3;
            if (errno != EACCES) return 4;
        }
        return 0;
    }, &report);
    CHECK_EQ(result, 0);
    // This machine has Landlock; say so if a test machine does not, rather
    // than passing quietly without having checked the part that matters.
    if (report.landlock_abi < 1) fprintf(stderr, "  note: no Landlock here, only no_new_privs was checked\n");
}

TEST_CASE(a_decoder_sandbox_still_lets_the_decoder_itself_run) {
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
