// Kept separate so the target's BPF can also be checked under qemu-user,
// which does not implement PR_SET_SECCOMP for the guest.
#pragma once

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fernsdr {
namespace sandbox_detail {

#if defined(__x86_64__) && !defined(__ILP32__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__arm__) && defined(__ARM_EABI__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_ARM;
#else
#error Unsupported decoder sandbox architecture
#endif

inline std::vector<sock_filter> decoder_filter(pid_t pid) {
    std::vector<sock_filter> filter = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        // Sample pipes dominate a decoder's syscalls. Keep their permission
        // unconditional and short, including on kernels without filter caching.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    constexpr uint32_t denied = SECCOMP_RET_ERRNO | EPERM;
#if defined(__x86_64__)
    // x32 shares the audit architecture; older kernels also accepted its
    // raw 512..547 aliases without the x32 bit. Neither ABI is supported.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 512, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, denied));
#elif defined(__arm__)
    // OABI compatibility can expose socketcall and syscall indirection even
    // though EABI headers do not list them. BPF cannot inspect their pointers.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x900000, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, denied));
    for (const uint32_t nr : {102u, 113u}) {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, denied));
    }
#endif
    // io_uring can create sockets without socket(2). The other calls have
    // thread, descriptor or tracer targets whose process BPF cannot verify.
    // These two newer syscall numbers are shared by all supported targets,
    // including builds whose system headers predate their introduction.
    for (const uint32_t nr : {425u, 424u, 438u, static_cast<uint32_t>(SYS_tkill),
                              static_cast<uint32_t>(SYS_ptrace)}) {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, denied));
    }
    const auto argument_is = [&](uint32_t nr, uint32_t value) {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 4));
        // The kernel consumes these arguments as 32-bit int/pid_t values on
        // all three targets; their upper bits do not change the operation.
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, denied));
    };
    argument_is(SYS_socket, AF_UNIX);
    argument_is(SYS_socketpair, AF_UNIX);
    for (const uint32_t nr : {SYS_kill, SYS_tgkill, SYS_rt_sigqueueinfo, SYS_rt_tgsigqueueinfo}) {
        argument_is(nr, static_cast<uint32_t>(pid));
    }
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    return filter;
}

}  // namespace sandbox_detail
}  // namespace fernsdr
