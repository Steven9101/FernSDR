// Looking up a host's IPv4 addresses without the C library's resolver.
//
// The release builds are static, and a static glibc's getaddrinfo loads the
// NSS modules of whatever glibc is on the host at run time, or fails. For
// the few names the receiver itself contacts (report.pskreporter.info), a
// plain DNS query over UDP to the nameservers in /etc/resolv.conf is
// enough, and it can be read and tested here.
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fernsdr::dns {

// The nameservers /etc/resolv.conf (or `path`) lists, IPv4 and IPv6, in its
// order; 127.0.0.1 when it lists none, as the C library does.
std::vector<std::string> nameservers(const std::string& path = "/etc/resolv.conf");

// A query for `host`'s A records with transaction `id`; empty for a name
// that is not a plain host name.
std::vector<uint8_t> build_query(const std::string& host, uint16_t id);

// The addresses an answer to that query gives, following a CNAME chain
// within it. False, and why, for anything that is not such an answer: a
// different id or question, an error code, a malformed or looping message.
bool parse_answer(const uint8_t* data, size_t size, const std::string& host, uint16_t id,
                  std::vector<in_addr>& out, uint32_t& ttl, std::string& why);

// Asks each nameserver in turn, twice, waiting up to `timeout_ms` for each
// answer. `servers` empty means nameservers(). A literal IPv4 address is
// returned as it is. Blocking: for a thread of its own.
bool resolve_ipv4(const std::string& host, std::vector<in_addr>& out, std::string& error, int timeout_ms = 2000,
                  const std::vector<std::string>& servers = {}, uint16_t port = 53);

}  // namespace fernsdr::dns
