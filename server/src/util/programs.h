// Finding and running the helper programs the receiver may use, such as curl
// for module downloads and the directory listing.
#pragma once
#include <string>
#include <vector>

namespace fernsdr {

/**
 * Absolute path of `name` in /usr/bin, /usr/local/bin or /bin, or empty.
 * Looked up in fixed places rather than on PATH, which the service manager
 * sets and an attacker who can write a directory on it could exploit.
 */
std::string find_program(const char* name);

/**
 * A fixed environment for a network helper, plus whatever proxy and
 * certificate settings the receiver itself was given: a site behind a proxy
 * has to be able to reach the internet at all.
 */
std::vector<std::string> network_helper_environment();

}  // namespace fernsdr
