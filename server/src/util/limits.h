// Process limits the receiver raises for itself at startup.
#pragma once
#include <cstdint>

namespace fernsdr {

/**
 * Raises the soft limit on open files to the hard limit (at most 1 << 20) and
 * returns the soft limit in force afterwards.
 *
 * Every listener holds a socket. A service manager or container that leaves
 * the soft limit at the usual 1024 while allowing far more as the hard limit
 * stopped the receiver accepting at about a thousand connections with its
 * cores mostly idle: the benchmark lab served 800 listeners at 87 % of two
 * cores, and at 1600 every socket above 1017 failed to connect. Raising the
 * soft limit up to the hard one needs no privilege.
 */
std::uint64_t raise_open_file_limit();

}  // namespace fernsdr
