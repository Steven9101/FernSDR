// The sandbox a decoder module runs in. A decoder parses signals anyone with
// a transmitter can send, so a flaw in it must not reach further than the
// pipes it was given: it may not gain privileges, it may open no file and
// make no TCP connection, and it runs at the lowest CPU priority so a busy
// one never takes time from a listener's audio.
//
// Applied in a copy of this program started as `fernsdr --sandbox-exec
// <decoder> <arguments>`, which then executes the decoder: posix_spawn has
// no place to run code between fork and exec, and this needs none.
#pragma once

#include <string>

namespace fernsdr {

struct SandboxReport {
    int landlock_abi = 0;         // 0: the kernel has no Landlock
    bool files_closed = false;    // no file opens outside the executable and its libraries
    bool network_closed = false;  // no TCP bind or connect
    std::string problem;          // why something could not be closed, for the log
};

// Everything short of exec: no new privileges, idle priority, and Landlock
// allowing only `executable` to be run (and the system's libraries read, for
// a decoder that is not linked statically). Irreversible for this process.
SandboxReport apply_decoder_sandbox(const std::string& executable);

// `fernsdr --sandbox-exec <path> <arguments...>`: applies the sandbox and
// executes `path`. Returns only on failure, with the exit status to use.
int sandbox_exec_command(int argc, char** argv, int first);

}  // namespace fernsdr
