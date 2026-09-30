// The sandbox a decoder module runs in. A decoder parses signals anyone with
// a transmitter can send, so a flaw in it must not reach further than the
// pipes it was given: it may not gain privileges or create non-UNIX sockets.
// Landlock restricts file access where available, and it runs at the lowest
// CPU priority so a busy one yields to a listener's audio.
//
// Applied in a copy of this program started as `fernsdr --sandbox-exec
// <decoder> <arguments>`, which then executes the decoder: posix_spawn has
// no place to run code between fork and exec, and this needs none.
#pragma once

#include <string>

namespace fernsdr {

struct SandboxReport {
    bool seccomp = false;         // mandatory socket and signal restrictions
    int landlock_abi = 0;         // 0: the kernel has no Landlock
    bool files_closed = false;    // no file opens outside the executable and its libraries
    bool network_closed = false;  // no non-UNIX socket creation
    bool scoped = false;          // Landlock scopes signals and abstract UNIX sockets
    std::string problem;          // why something could not be closed, for the log
};

// Everything short of exec: no new privileges, seccomp, idle priority, and Landlock
// allowing only `executable` to be run (and the system's libraries read, for
// a decoder that is not linked statically). Irreversible for this process.
SandboxReport apply_decoder_sandbox(const std::string& executable);

// `fernsdr --sandbox-exec <path> <arguments...>`: applies the sandbox and
// executes `path`. With a report descriptor, writes the launcher's status
// there and closes it before exec. Returns only on failure.
int sandbox_exec_command(int argc, char** argv, int first, int report_fd = -1);

}  // namespace fernsdr
