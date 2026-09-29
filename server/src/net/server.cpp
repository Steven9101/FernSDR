#include "server.h"
#include "../util/utf8.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "../util/log.h"

namespace fernsdr {

namespace {

constexpr size_t kReadChunk = 16 * 1024;
constexpr size_t kMaxHttpRequestBytes = 64 * 1024;

/*
 * One path takes a file, and a file does not fit in 64 kB.
 *
 * Raising the limit for everything would let any unauthenticated caller park
 * megabytes in this process, four hundred times over. Raising it only for the
 * upload path still would, so the number of connections allowed to do it at
 * once is capped as well: four times nine megabytes is a bounded amount of
 * memory, and a fifth simultaneous upload is told to wait rather than being
 * quietly dropped.
 *
 * The path is read from the bytes already buffered, before the request is
 * parsed, because the decision has to be made while the body is still
 * arriving.
 */
constexpr size_t kMaxUploadRequestBytes = 9 * 1024 * 1024;
constexpr int kMaxConcurrentUploads = 4;
/*
 * How long a refused request is drained for. The answer is written straight
 * away, but the client is still sending and cannot read until it is done, so
 * closing on the flush would replace the message with a reset connection.
 * Long enough for the rest of a large body over a slow uplink, short enough
 * that it is not a way to hold a socket open.
 */
constexpr int64_t kDrainMs = 10000;

/** True when the buffered head of a request is the upload endpoint. */
bool looks_like_an_upload(const std::vector<uint8_t>& buffer) {
    static const char kHead[] = "POST /api/admin/upload";
    const size_t length = sizeof(kHead) - 1;
    if (buffer.size() <= length) return false;
    return std::memcmp(buffer.data(), kHead, length) == 0 &&
           (buffer[length] == ' ' || buffer[length] == '?');
}
// Slots only this machine may use once the receiver is full, so an operator
// coming in over ssh -L can still reach the admin panel while every other slot
// is taken.
constexpr int kReservedLocalConnections = 4;

/** Answers a connection that will not be served, and closes it. */
void refuse_connection(int fd, const char* reason) {
    const std::string response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: " +
                                 std::to_string(std::strlen(reason)) + "\r\nConnection: close\r\n\r\n" + reason;
    ssize_t ignored = ::write(fd, response.data(), response.size());
    (void)ignored;
    ::close(fd);
}

constexpr int kEpollTimeoutMs = 20;
constexpr int64_t kTickIntervalMs = 100;

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

int64_t monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Connection::send_text(const std::string& text) {
    if (state_ != State::WebSocket) return;
    out_.frame(ws::Opcode::Text, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    want_write_ = true;
}

void Connection::send_binary(const std::vector<uint8_t>& payload, bool bulk) {
    if (state_ != State::WebSocket) return;
    out_.frame(ws::Opcode::Binary, payload.data(), payload.size(), monotonic_ms(), bulk);
    want_write_ = true;
}

void Connection::close(uint16_t code, const std::string& reason) {
    if (state_ == State::WebSocket && !close_queued_) {
        out_.close(code, reason);
        close_queued_ = true;
    }
    close_after_flush_ = true;
    want_write_ = true;
}

int Connection::tcp_option(int name) const {
    int value = 0;
    socklen_t length = sizeof(value);
    return getsockopt(fd_, IPPROTO_TCP, name, &value, &length) == 0 ? value : -1;
}

size_t Connection::transport_backlog() {
    const int64_t now = monotonic_ms();
    if (now - backlog_updated_ms_ >= 100) {
        int unsent = 0;
        // Count bytes not yet transmitted, excluding packets awaiting an ACK.
        // A long RTT on mobile data is not itself a bandwidth shortage.
        if (::ioctl(fd_, SIOCOUTQNSD, &unsent) == 0) kernel_backlog_ = static_cast<size_t>(std::max(0, unsent));
        tcp_info info{};
        socklen_t length = sizeof(info);
        if (::getsockopt(fd_, IPPROTO_TCP, TCP_INFO, &info, &length) == 0) {
            queue_delay_.update(now, info.tcpi_rtt, info.tcpi_unacked, info.tcpi_last_ack_recv);
        }
        backlog_updated_ms_ = now;
    }
    return pending_bytes() + kernel_backlog_;
}

Server::Server(ServerConfig config, ServerHandler& handler)
    : config_(std::move(config)),
      handler_(handler),
      static_files_(config_.document_root),
      uploads_(config_.uploads_root) {
    static_files_.set_frame_ancestors(config_.frame_ancestors);
}

bool is_an_upload_name(const std::string& name) {
    const size_t dot = name.find('.');
    if (dot != 16 || name.size() > 24) return false;
    for (size_t i = 0; i < dot; i++) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    const std::string extension = name.substr(dot + 1);
    return extension == "png" || extension == "jpg" || extension == "gif" || extension == "webp";
}

bool Server::serve_upload(const HttpRequest& request, std::string& response) const {
    static const std::string kPrefix = "/uploads/";
    if (!uploads_.enabled() || request.path.rfind(kPrefix, 0) != 0) return false;
    if (!is_an_upload_name(request.path.substr(kPrefix.size()))) {
        response = build_http_response(404, "text/plain", "not found", {}, request.keep_alive());
        return true;
    }
    // The prefix is the route, not part of the filename, so it is taken off
    // before the path is resolved inside the uploads directory.
    HttpRequest rewritten = request;
    rewritten.path = request.path.substr(kPrefix.size() - 1);
    return uploads_.serve(rewritten, response);
}

Server::~Server() {
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool Server::start(std::string& error) {
    // A client that disconnects mid-write must not take the process with it.
    signal(SIGPIPE, SIG_IGN);

    // A dual-stack socket where the address allows it, so a receiver reachable
    // over IPv6 does not need a second process or a proxy in front. Binding
    // AF_INET6 with V6ONLY off accepts both families on one socket, and an
    // IPv4 peer then arrives as ::ffff:a.b.c.d, which resolve_client_address
    // and the CIDR matcher both understand.
    const bool wildcard = config_.bind_address.empty() || config_.bind_address == "0.0.0.0" ||
                          config_.bind_address == "::" || config_.bind_address == "*";
    const bool ipv6_literal = config_.bind_address.find(':') != std::string::npos;
    const bool use_ipv6 = wildcard || ipv6_literal;

    // Every descriptor this process opens is close-on-exec: a module started
    // later must not inherit the listening socket, or it would keep the port
    // bound after the receiver stops.
    listen_fd_ = ::socket(use_ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0 && use_ipv6) {
        // A kernel built without IPv6, or a container that hides it. Fall back
        // rather than refusing to start.
        LOG_INFO("server", "this kernel has no IPv6; falling back to IPv4");
        dual_stack_ = false;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ >= 0) {
            int reuse_v4 = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_v4, sizeof(reuse_v4));
            sockaddr_in fallback{};
            fallback.sin_family = AF_INET;
            fallback.sin_port = htons(static_cast<uint16_t>(config_.port));
            fallback.sin_addr.s_addr = INADDR_ANY;
            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&fallback), sizeof(fallback)) != 0) {
                error = "cannot bind :" + std::to_string(config_.port) + ": " + std::strerror(errno);
                return false;
            }
            socklen_t length = sizeof(fallback);
            if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&fallback), &length) == 0) {
                bound_port_ = ntohs(fallback.sin_port);
            }
            goto listening;
        }
    }
    if (listen_fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    {
        int reuse = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // An explicit IPv4 bind address is an IPv4 socket, and saying otherwise
        // in the startup line is exactly the kind of small lie that costs
        // somebody an hour. Caught by the admin panel's own log view.
        dual_stack_ = use_ipv6;

        if (use_ipv6) {
            int only_v6 = 0;
            setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof(only_v6));

            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(static_cast<uint16_t>(config_.port));
            if (wildcard) {
                address.sin6_addr = in6addr_any;
            } else if (inet_pton(AF_INET6, config_.bind_address.c_str(), &address.sin6_addr) != 1) {
                error = "invalid bind address '" + config_.bind_address + "'";
                return false;
            }
            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                error = "cannot bind " + config_.bind_address + ":" + std::to_string(config_.port) +
                        ": " + std::strerror(errno);
                return false;
            }
            socklen_t length = sizeof(address);
            if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
                bound_port_ = ntohs(address.sin6_port);
            }
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<uint16_t>(config_.port));
            address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());
            if (address.sin_addr.s_addr == INADDR_NONE) {
                error = "invalid bind address '" + config_.bind_address + "'";
                return false;
            }
            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                error = "cannot bind " + config_.bind_address + ":" + std::to_string(config_.port) +
                        ": " + std::strerror(errno);
                return false;
            }
            socklen_t length = sizeof(address);
            if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
                bound_port_ = ntohs(address.sin_port);
            }
        }
    }

listening:
    if (::listen(listen_fd_, 128) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        return false;
    }
    if (!set_nonblocking(listen_fd_)) {
        error = "cannot set the listening socket non-blocking";
        return false;
    }

    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        error = std::string("epoll_create1: ") + std::strerror(errno);
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event);

    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        error = std::string("eventfd: ") + std::strerror(errno);
        return false;
    }
    event.data.fd = wake_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event);

    running_ = true;
    last_tick_ms_ = monotonic_ms();
    // Report what it is actually listening on, not what was asked for: after a
    // fallback those are different, and a log that says ":::8096" on an
    // IPv4-only socket is a log that will mislead somebody at 2am.
    LOG_INFO("server", "listening on %s:%d (%s)%s", config_.bind_address.c_str(), bound_port_,
             dual_stack_ ? "IPv4 and IPv6" : "IPv4",
             static_files_.enabled() ? "" : " (no document root; API only)");
    return true;
}

void Server::stop() { running_ = false; wake(); }

void Server::wake() {
    if (wake_fd_ < 0) return;
    const uint64_t one = 1;
    ssize_t ignored = ::write(wake_fd_, &one, sizeof(one));
    (void)ignored;
}

void Server::run() {
    std::vector<epoll_event> events(64);

    while (running_) {
        const int count = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()),
                                     kEpollTimeoutMs);
        if (count < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("server", "epoll_wait: %s", std::strerror(errno));
            break;
        }

        bool woken = false;
        for (int i = 0; i < count; i++) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                accept_pending();
                continue;
            }
            if (fd == wake_fd_) {
                uint64_t drained;
                while (::read(wake_fd_, &drained, sizeof(drained)) > 0) { /* drain */ }
                woken = true;
                continue;
            }

            auto it = connections_.find(fd);
            if (it == connections_.end()) continue;
            Connection& connection = *it->second;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                drop(connection, "peer closed");
                continue;
            }
            if (events[i].events & EPOLLIN) handle_readable(connection);
            if (connections_.count(fd) && (events[i].events & EPOLLOUT)) handle_writable(connection);
        }

        // Push whatever the DSP threads produced.  Doing this on every wake,
        // rather than on a timer, is what keeps end-to-end latency close to
        // the block period.
        if (woken || count == 0) handler_.on_flush();

        const int64_t now = monotonic_ms();
        if (now - last_tick_ms_ >= kTickIntervalMs) {
            last_tick_ms_ = now;
            handler_.on_tick();
            reap_idle();
        }

        // Flush anything the handler queued, and retire closed connections.
        for (auto& entry : connections_) {
            if (entry.second->want_write_) handle_writable(*entry.second);
        }
        for (int fd : to_close_) {
            auto it = connections_.find(fd);
            if (it == connections_.end()) continue;
            Connection& closing = *it->second;
            if (closing.handshake_completed_ && !closing.disconnect_delivered_) {
                closing.disconnect_delivered_ = true;
                handler_.on_disconnect(closing);
            }
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
            connections_.erase(it);
        }
        to_close_.clear();
    }

    LOG_INFO("server", "shutting down with %d connection(s)", connection_count());
    for (auto& entry : connections_) {
        Connection& connection = *entry.second;
        if (connection.handshake_completed_ && !connection.disconnect_delivered_) {
            connection.disconnect_delivered_ = true;
            handler_.on_disconnect(connection);
        }
        ::close(entry.first);
    }
    connections_.clear();
}

namespace {

// Parses a dotted quad into host-order bits.  Returns false on anything that
// is not exactly four decimal octets, which keeps a malformed header from
// matching a prefix by accident.
bool parse_ipv4(const std::string& text, uint32_t& out) {
    uint32_t value = 0;
    int octets = 0;
    size_t index = 0;
    while (octets < 4) {
        if (index >= text.size() || !isdigit(static_cast<unsigned char>(text[index]))) return false;
        int part = 0;
        while (index < text.size() && isdigit(static_cast<unsigned char>(text[index]))) {
            part = part * 10 + (text[index] - '0');
            if (part > 255) return false;
            index++;
        }
        value = (value << 8) | static_cast<uint32_t>(part);
        octets++;
        if (octets < 4) {
            if (index >= text.size() || text[index] != '.') return false;
            index++;
        }
    }
    return index == text.size() ? (out = value, true) : false;
}

// Parses an IPv6 literal into 16 bytes. inet_pton does the work; this exists
// to keep the caller free of address-family branching.
bool parse_ipv6(const std::string& text, uint8_t out[16]) {
    return inet_pton(AF_INET6, text.c_str(), out) == 1;
}

// True when the first `bits` bits of two byte strings agree.
bool prefix_equal(const uint8_t* a, const uint8_t* b, int bits) {
    const int whole = bits / 8;
    if (whole > 0 && std::memcmp(a, b, static_cast<size_t>(whole)) != 0) return false;
    const int remainder = bits % 8;
    if (remainder == 0) return true;
    const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remainder));
    return (a[whole] & mask) == (b[whole] & mask);
}

bool parse_prefix_bits(const std::string& text, int maximum, int& bits) {
    if (text.empty()) return false;
    bits = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        bits = bits * 10 + c - '0';
        if (bits > maximum) return false;
    }
    return true;
}

std::string trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

bool address_matches_cidr(const std::string& address, const std::string& cidr) {
    if (cidr == "any") return true;
    if (cidr == "none" || cidr.empty()) return false;
    // Loopback means both families. A receiver reached over IPv6 through a
    // local proxy is the common case for the HTTPS setup DEPLOYMENT.md
    // describes, and before this it fell through and the proxy's own address
    // was attributed to every listener.
    if (cidr == "loopback") {
        return address_matches_cidr(address, "127.0.0.0/8") ||
               address_matches_cidr(address, "::1/128");
    }

    // IPv6 on either side is handled in 128-bit space. An IPv4-mapped literal
    // (::ffff:a.b.c.d) matches an IPv4 CIDR too, because it IS that address.
    if (address.find(':') != std::string::npos || cidr.find(':') != std::string::npos) {
        uint8_t peer6[16];
        if (!parse_ipv6(address, peer6)) {
            // An IPv4 address against an IPv6 CIDR: map it and carry on.
            uint32_t plain = 0;
            if (!parse_ipv4(address, plain)) return false;
            std::memset(peer6, 0, 10);
            peer6[10] = peer6[11] = 0xFF;
            for (int i = 0; i < 4; i++) peer6[12 + i] = static_cast<uint8_t>(plain >> (24 - 8 * i));
        }

        const size_t slash = cidr.find('/');
        const std::string network_text = slash == std::string::npos ? cidr : cidr.substr(0, slash);
        uint8_t network6[16];
        if (!parse_ipv6(network_text, network6)) {
            // An IPv6 peer against an IPv4 CIDR only matches when the peer is
            // the mapped form of an address inside it.
            static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
            if (std::memcmp(peer6, kMappedPrefix, sizeof(kMappedPrefix)) != 0) return false;
            char unwrapped[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop(AF_INET, peer6 + 12, unwrapped, sizeof(unwrapped))) return false;
            return address_matches_cidr(unwrapped, cidr);
        }

        int bits = 128;
        if (slash != std::string::npos) {
            const std::string bits_text = cidr.substr(slash + 1);
            if (!parse_prefix_bits(bits_text, 128, bits)) return false;
        }
        return prefix_equal(peer6, network6, bits);
    }

    uint32_t peer = 0;
    if (!parse_ipv4(address, peer)) return false;

    const size_t slash = cidr.find('/');
    const std::string network_text = slash == std::string::npos ? cidr : cidr.substr(0, slash);
    uint32_t network = 0;
    if (!parse_ipv4(network_text, network)) return false;

    int bits = 32;
    if (slash != std::string::npos) {
        const std::string bits_text = cidr.substr(slash + 1);
        if (!parse_prefix_bits(bits_text, 32, bits)) return false;
    }
    if (bits == 0) return true;
    const uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1u);
    return (peer & mask) == (network & mask);
}

std::string network_key(const std::string& address) {
    // Any process on this machine can pick its source address anywhere in
    // 127/8, so an address each would hand every one of them a fresh
    // allowance.
    if (address_matches_cidr(address, "loopback")) return "loopback";
    char text[INET6_ADDRSTRLEN] = {0};
    uint32_t four = 0;
    if (parse_ipv4(address, four)) {
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", four >> 24, (four >> 16) & 0xFF, (four >> 8) & 0xFF,
                      four & 0xFF);
        return text;
    }
    uint8_t six[16];
    if (!parse_ipv6(address, six)) return address;
    static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(six, kMappedPrefix, sizeof(kMappedPrefix)) == 0) {
        return inet_ntop(AF_INET, six + 12, text, sizeof(text)) ? std::string(text) : address;
    }
    std::memset(six + 8, 0, 8);
    return inet_ntop(AF_INET6, six, text, sizeof(text)) ? std::string(text) + "/64" : address;
}

std::string wide_network_key(const std::string& address) {
    uint32_t four = 0;
    if (parse_ipv4(address, four)) return "";
    uint8_t six[16];
    if (!parse_ipv6(address, six)) return "";
    static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(six, kMappedPrefix, sizeof(kMappedPrefix)) == 0) return "";
    std::memset(six + 6, 0, 10);
    char text[INET6_ADDRSTRLEN] = {0};
    return inet_ntop(AF_INET6, six, text, sizeof(text)) ? std::string(text) + "/48" : "";
}

std::string resolve_client_address(const std::string& peer, const HttpRequest& request,
                                   const std::vector<std::string>& trusted_proxies) {
    bool trusted = false;
    for (const std::string& cidr : trusted_proxies) {
        if (address_matches_cidr(peer, cidr)) {
            trusted = true;
            break;
        }
    }
    if (!trusted) return peer;

    // Proxies often append to an existing header. Walk from the socket peer
    // toward the client and stop at the first untrusted hop; entries before
    // it may have been supplied by that client.
    const auto looks_like_an_address = [](const std::string& text) {
        uint32_t four = 0;
        uint8_t six[16];
        return parse_ipv4(text, four) || parse_ipv6(text, six);
    };

    const std::string forwarded = request.header("x-forwarded-for");
    if (!forwarded.empty()) {
        std::string current = peer;
        size_t end = forwarded.size();
        while (true) {
            const bool hop_trusted = std::any_of(trusted_proxies.begin(), trusted_proxies.end(),
                [&current](const std::string& cidr) { return address_matches_cidr(current, cidr); });
            if (!hop_trusted) return current;
            const size_t comma = end == 0 ? std::string::npos : forwarded.rfind(',', end - 1);
            const size_t begin = comma == std::string::npos ? 0 : comma + 1;
            const std::string next = trim(forwarded.substr(begin, end - begin));
            if (!looks_like_an_address(next)) return peer;
            current = next;
            if (comma == std::string::npos) return current;
            end = comma;
        }
    }

    const std::string real = trim(request.header("x-real-ip"));
    if (!real.empty() && looks_like_an_address(real)) return real;

    return peer;
}

bool is_trusted_proxy(const std::string& peer, const std::vector<std::string>& trusted_proxies) {
    return std::any_of(trusted_proxies.begin(), trusted_proxies.end(),
                       [&peer](const std::string& cidr) { return address_matches_cidr(peer, cidr); });
}

bool request_is_secure(const std::string& peer, const HttpRequest& request,
                       const std::vector<std::string>& trusted_proxies) {
    return request.header("x-forwarded-proto") == "https" && is_trusted_proxy(peer, trusted_proxies);
}

void Server::accept_pending() {
    while (true) {
        sockaddr_storage peer{};
        socklen_t length = sizeof(peer);
        const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_WARN("server", "accept: %s", std::strerror(errno));
            break;
        }

        // An IPv4 peer on a dual-stack socket arrives as ::ffff:a.b.c.d.
        // Unwrapping it here means logs, per-address limits and the admin view
        // all show the address a person would recognise.
        char address[INET6_ADDRSTRLEN] = {0};
        if (peer.ss_family == AF_INET6) {
            const sockaddr_in6* six = reinterpret_cast<const sockaddr_in6*>(&peer);
            if (IN6_IS_ADDR_V4MAPPED(&six->sin6_addr)) {
                inet_ntop(AF_INET, &six->sin6_addr.s6_addr[12], address, sizeof(address));
            } else {
                inet_ntop(AF_INET6, &six->sin6_addr, address, sizeof(address));
            }
        } else {
            const sockaddr_in* four = reinterpret_cast<const sockaddr_in*>(&peer);
            inet_ntop(AF_INET, &four->sin_addr, address, sizeof(address));
        }

        // Say so rather than dropping the connection silently, so the user
        // sees "full" instead of a broken page.
        const bool local = address_matches_cidr(address, "loopback");
        int open = 0;
        for (const auto& entry : connections_) {
            if (entry.second->state_ != Connection::State::Closing) open++;
        }
        if (open >= config_.max_connections + (local ? kReservedLocalConnections : 0)) {
            refuse_connection(fd, "receiver is at capacity");
            continue;
        }
        const std::string network = network_key(address);
        const std::string wide = wide_network_key(address);
        const bool proxy = std::any_of(config_.trusted_proxies.begin(), config_.trusted_proxies.end(),
                                       [&](const std::string& cidr) { return address_matches_cidr(address, cidr); });
        if (config_.max_connections_per_address > 0 && !proxy) {
            // A /48 may hold a few times what one /64 may.
            int from_there = 0, from_around = 0;
            for (const auto& entry : connections_) {
                const Connection& other = *entry.second;
                if (other.state_ == Connection::State::Closing) continue;
                if (other.network_ == network) from_there++;
                if (!wide.empty() && other.wide_network_ == wide) from_around++;
            }
            if (from_there >= config_.max_connections_per_address ||
                from_around >= 4 * config_.max_connections_per_address) {
                if (local && !refused_loopback_logged_) {
                    // The likely reason is a raw TCP tunnel, through which
                    // everyone arrives from loopback; said once, like the
                    // listener limit says it.
                    LOG_WARN("server", "refusing connections from loopback: max_connections_per_address (%d) "
                             "reached; behind a raw TCP tunnel set it to 0 and limit clients at the far end",
                             config_.max_connections_per_address);
                    refused_loopback_logged_ = true;
                }
                refuse_connection(fd, "too many connections from this address");
                continue;
            }
        }

        set_nonblocking(fd);
        // Latency matters more than packet efficiency for an audio stream.
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        auto connection = std::unique_ptr<Connection>(new Connection(next_connection_id_++, fd, address));
        connection->network_ = network;
        connection->wide_network_ = wide;
        connection->last_activity_ms_ = monotonic_ms();
        connection->request_started_ms_ = connection->last_activity_ms_;

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
        connections_.emplace(fd, std::move(connection));
    }
}

void Server::handle_readable(Connection& connection) {
    uint8_t buffer[kReadChunk];
    size_t received = 0;
    // epoll is level-triggered. Yield after a bounded amount of work so one
    // uploader cannot starve the audio sockets, and parse before reading more
    // so request limits apply while bytes arrive.
    while (received < 64 * 1024 && !connection.close_after_flush_ &&
           connection.state_ != Connection::State::Closing) {
        const ssize_t got = ::read(connection.fd_, buffer, sizeof(buffer));
        if (got > 0) {
            received += static_cast<size_t>(got);
            connection.last_activity_ms_ = monotonic_ms();
            if (connection.state_ == Connection::State::Http && connection.request_started_ms_ == 0) {
                connection.request_started_ms_ = connection.last_activity_ms_;
            }
            // Already answered: read it and let it go, so the client can finish
            // sending and then read what it was told.
            if (!connection.draining_) {
                connection.in_.insert(connection.in_.end(), buffer, buffer + got);
                if (connection.state_ == Connection::State::Http) process_http(connection);
                else if (connection.state_ == Connection::State::WebSocket) process_websocket(connection);
            }
            continue;
        }
        if (got == 0) {
            drop(connection, "peer closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        drop(connection, std::strerror(errno));
        return;
    }

    if (connection.draining_ || connection.close_after_flush_ || connection.state_ == Connection::State::Closing) {
        // Nothing left to do but let the outbox flush and close.
        return;
    }
    if (connection.state_ == Connection::State::Http) {
        process_http(connection);
    } else if (connection.state_ == Connection::State::WebSocket) {
        process_websocket(connection);
    }
}

void Server::refuse_and_drain(Connection& connection, int status, const std::string& message) {
    const std::string response = build_http_response(status, "application/json",
                                                     "{\"error\":\"" + message + "\"}", {}, false);
    queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
    // Deliberately not close_after_flush_: the answer is written immediately
    // and the client is still sending, so closing on the flush would hand it a
    // reset instead of the message. The response says Connection: close, so a
    // client that finishes and reads it closes on its own; the deadline covers
    // one that does not.
    connection.draining_ = true;
    connection.drain_deadline_ms_ = monotonic_ms() + kDrainMs;
    connection.in_.clear();
    connection.in_.shrink_to_fit();
    if (connection.oversized_) {
        connection.oversized_ = false;
        if (uploads_in_flight_ > 0) uploads_in_flight_--;
    }
    update_interest(connection);
}

void Server::process_http(Connection& connection) {
    while (true) {
        // Decided from the request line rather than from how much has arrived: the
        // body limit has to be known before the first byte of it is parsed, and a
        // large upload whose first read is small would otherwise be measured
        // against the ordinary limit and refused.
        const bool upload = looks_like_an_upload(connection.in_);

        if (connection.in_.size() > kMaxHttpRequestBytes && !connection.oversized_) {
            if (!upload) {
                refuse_and_drain(connection, 413, "that request is too large");
                return;
            }
            // The head is complete by now, since it may not exceed the ordinary
            // limit, and it decides whether the rest may come at all.
            HttpRequest head;
            size_t unused = 0;
            const HttpParse peek = parse_http_request(reinterpret_cast<const char*>(connection.in_.data()),
                                                      connection.in_.size(), head, unused, kMaxUploadRequestBytes);
            if (peek == HttpParse::Error || head.version.empty()) {
                refuse_and_drain(connection, 400, "bad request");
                return;
            }
            if (peek == HttpParse::TooLarge) {
                refuse_and_drain(connection, 413, "that image is too large, the limit is 8 MB");
                return;
            }
            head.secure = request_is_secure(connection.peer_address_, head, config_.trusted_proxies);
            head.via_trusted_proxy = is_trusted_proxy(connection.peer_address_, config_.trusted_proxies);
            if (!handler_.may_upload(connection, head)) {
                refuse_and_drain(connection, 401, "sign in to the admin panel to upload");
                return;
            }
            if (uploads_in_flight_ >= kMaxConcurrentUploads) {
                refuse_and_drain(connection, 503, "too many uploads at once, try again in a moment");
                return;
            }
            connection.oversized_ = true;
            uploads_in_flight_++;
        }
        if (connection.oversized_ && connection.in_.size() > kMaxUploadRequestBytes) {
            refuse_and_drain(connection, 413, "that image is too large, the limit is 8 MB");
            return;
        }

        HttpRequest request;
        size_t consumed = 0;
        const HttpParse result = parse_http_request(reinterpret_cast<const char*>(connection.in_.data()),
                                                    connection.in_.size(), request, consumed,
                                                    upload ? kMaxUploadRequestBytes : kMaxHttpRequestBytes);
        if (result == HttpParse::NeedMore) {
            connection.request_headers_complete_ = !request.version.empty();
            return;
        }
        if (result == HttpParse::TooLarge) {
            refuse_and_drain(connection, 413,
                             upload ? "that image is too large, the limit is 8 MB"
                                    : "that request is too large");
            return;
        }
        if (result == HttpParse::Error) {
            refuse_and_drain(connection, 400, "bad request");
            return;
        }

        if (connection.oversized_) {
            // Returning the upload slot must also return its allocation.
            // Otherwise completed requests on idle keep-alive connections
            // retain megabytes each, outside the concurrent-upload limit.
            std::vector<uint8_t>(connection.in_.begin() + static_cast<long>(consumed),
                                 connection.in_.end()).swap(connection.in_);
        } else {
            connection.in_.erase(connection.in_.begin(), connection.in_.begin() + static_cast<long>(consumed));
        }
        const int64_t now = monotonic_ms();
        connection.request_started_ms_ = connection.in_.empty() ? 0 : now;
        connection.request_headers_complete_ = false;
        if (connection.http_credit_updated_ms_ == 0) connection.http_credit_updated_ms_ = now;
        connection.http_credit_ = std::min(60.0, connection.http_credit_ +
            (now - connection.http_credit_updated_ms_) * 0.03);
        connection.http_credit_updated_ms_ = now;
        if (connection.http_credit_ < 1) {
            refuse_and_drain(connection, 429, "too many requests on this connection");
            return;
        }
        connection.http_credit_--;

        // The oversized request is complete, so the slot goes back. Held past
        // this, four uploads would close the endpoint for the rest of the
        // receiver's life.
        if (connection.oversized_) {
            connection.oversized_ = false;
            if (uploads_in_flight_ > 0) uploads_in_flight_--;
        }

        request.secure = request_is_secure(connection.peer_address_, request, config_.trusted_proxies);
        request.via_trusted_proxy = is_trusted_proxy(connection.peer_address_, config_.trusted_proxies);
        connection.remote_address_ = resolve_client_address(connection.peer_address_, request, config_.trusted_proxies);

        const bool is_upgrade = request.header_contains("connection", "upgrade") &&
                                request.header_contains("upgrade", "websocket");
        if (is_upgrade) {
            if (!complete_handshake(connection, request)) return;
            // Anything already buffered belongs to the WebSocket stream.
            process_websocket(connection);
            return;
        }

        std::string response;
        if (handler_.on_http(connection, request, response)) {
            queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        } else if (serve_upload(request, response)) {
            queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        } else if (static_files_.serve(request, response)) {
            queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        } else {
            response = build_http_response(404, "text/plain", "not found", {}, request.keep_alive());
            queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        }

        if (!request.keep_alive()) connection.close_after_flush_ = true;
        if (connection.pending_bytes() > config_.max_output_bytes) handle_writable(connection);
        if (connection.state_ == Connection::State::Closing) return;
        update_interest(connection);

        // A pipelined follow-up request may already be buffered.
        if (connection.close_after_flush_ || connection.in_.empty()) return;
    }
}

bool Server::complete_handshake(Connection& connection, const HttpRequest& request) {
    const std::string key = request.header("sec-websocket-key");
    const std::string version = request.header("sec-websocket-version");
    if (request.method != "GET" || request.version != "HTTP/1.1" ||
        !request.body.empty() || !ws::valid_client_key(key)) {
        const std::string response = build_http_response(400, "text/plain", "invalid WebSocket handshake", {}, false);
        queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        connection.close_after_flush_ = true;
        update_interest(connection);
        return false;
    }
    if (version != "13") {
        const std::string response = build_http_response(
            426, "text/plain", "this endpoint speaks WebSocket version 13",
            {{"Sec-WebSocket-Version", "13"}}, false);
        queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        connection.close_after_flush_ = true;
        update_interest(connection);
        return false;
    }
    if (!config_.websocket_path.empty() && request.path != config_.websocket_path) {
        const std::string response = build_http_response(404, "text/plain", "no WebSocket here", {}, false);
        queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());
        connection.close_after_flush_ = true;
        update_interest(connection);
        return false;
    }

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + ws::accept_key(key) + "\r\n\r\n";
    queue(connection, reinterpret_cast<const uint8_t*>(response.data()), response.size());

    connection.state_ = Connection::State::WebSocket;
    const int low_water = 4096;
    setsockopt(connection.fd_, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &low_water, sizeof(low_water));
#ifdef TCP_THIN_LINEAR_TIMEOUTS
    // A listener's stream is thin: a few small packets in flight. When a link
    // drops for a second, TCP's retransmission timer doubles on every try, so
    // the retry that gets through can come a second or more after the link is
    // back, and the audio stays silent that long. For a thin stream this makes
    // the first six retries keep the same interval instead.
    const int linear = 1;
    setsockopt(connection.fd_, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &linear, sizeof(linear));
#endif
    connection.handshake_completed_ = true;
    connection.last_pong_ms_ = monotonic_ms();

    if (!handler_.on_connect(connection)) {
        connection.close(1013, "receiver is at capacity");
        update_interest(connection);
        return false;
    }
    update_interest(connection);
    return true;
}

void Server::process_websocket(Connection& connection) {
    while (!connection.in_.empty() && !connection.close_after_flush_) {
        ws::Frame frame;
        size_t consumed = 0;
        const ws::ParseResult result =
            ws::parse_frame(connection.in_.data(), connection.in_.size(), frame, consumed, kMaxClientMessageBytes);
        if (result == ws::ParseResult::NeedMore) break;
        if (result == ws::ParseResult::Error) {
            connection.close(1002, "protocol error");
            update_interest(connection);
            return;
        }
        connection.in_.erase(connection.in_.begin(), connection.in_.begin() + static_cast<long>(consumed));

        const int64_t now = monotonic_ms();
        if (connection.credit_updated_ms_ == 0) connection.credit_updated_ms_ = now;
        connection.message_credit_ = std::min(480.0, connection.message_credit_ +
            (now - connection.credit_updated_ms_) * 0.24);
        connection.credit_updated_ms_ = now;
        if (connection.message_credit_ < 1) {
            connection.close(1008, "too many commands");
            return;
        }
        connection.message_credit_ -= 1;

        std::vector<uint8_t> message;
        ws::Opcode opcode;
        const auto status = connection.assembler_.feed(frame, message, opcode);
        if (status == ws::MessageAssembler::Status::Error) {
            connection.close(1009, "message too large");
            update_interest(connection);
            return;
        }
        if (status != ws::MessageAssembler::Status::Complete) continue;

        switch (opcode) {
            case ws::Opcode::Text:
                if (!valid_utf8(std::string_view(reinterpret_cast<const char*>(message.data()), message.size()))) {
                    connection.close(1007, "invalid UTF-8 text");
                    update_interest(connection);
                    return;
                }
                handler_.on_text(connection, std::string(message.begin(), message.end()));
                break;
            case ws::Opcode::Binary:
                // The client has nothing to say in binary; ignore rather than
                // close, so a future extension stays backward compatible.
                break;
            case ws::Opcode::Ping: {
                connection.out_.frame(ws::Opcode::Pong, message.data(), message.size());
                connection.want_write_ = true;
                break;
            }
            case ws::Opcode::Pong:
                connection.last_pong_ms_ = monotonic_ms();
                break;
            case ws::Opcode::Close:
                connection.close(1000, "");
                break;
            default:
                break;
        }
        update_interest(connection);
    }
}

void Server::queue(Connection& connection, const uint8_t* data, size_t size) {
    connection.out_.append(data, size);
    connection.want_write_ = true;
}

void Server::handle_writable(Connection& connection) {
    while (connection.out_.size()) {
        handler_.on_before_write(connection);
        if (!connection.out_.size()) break;
        const ssize_t sent = ::write(connection.fd_, connection.out_.data(), connection.out_.size());
        if (sent > 0) {
            connection.out_.consume(static_cast<size_t>(sent));
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (sent < 0 && errno == EINTR) continue;
        drop(connection, "write failed");
        return;
    }

    if (!connection.out_.size()) {
        connection.want_write_ = false;
        if (connection.close_after_flush_) {
            drop(connection, "closed");
            return;
        }
    }

    if (connection.pending_bytes() > config_.max_output_bytes) {
        drop(connection, "client too slow");
        return;
    }
    update_interest(connection);
}

void Server::update_interest(Connection& connection) {
    epoll_event event{};
    event.events = EPOLLIN | (connection.want_write_ ? EPOLLOUT : 0u);
    event.data.fd = connection.fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection.fd_, &event);
}

void Server::drop(Connection& connection, const char* reason) {
    if (connection.state_ == Connection::State::Closing) return;
    // Give the upload slot back. Without this a dropped upload holds one
    // forever and four of them close the endpoint for good.
    if (connection.oversized_) {
        connection.oversized_ = false;
        if (uploads_in_flight_ > 0) uploads_in_flight_--;
    }
    connection.state_ = Connection::State::Closing;
    LOG_DEBUG("server", "connection %llu from %s closed: %s",
              static_cast<unsigned long long>(connection.id()), connection.remote_address().c_str(), reason);
    to_close_.push_back(connection.fd_);
}

void Server::reap_idle() {
    const int64_t now = monotonic_ms();
    const int64_t limit = static_cast<int64_t>(config_.idle_timeout_seconds) * 1000;
    for (auto& entry : connections_) {
        Connection& connection = *entry.second;
        if (connection.state_ == Connection::State::Closing) continue;
        if (connection.draining_ && now > connection.drain_deadline_ms_) {
            drop(connection, "drained long enough");
            continue;
        }
        if (connection.state_ == Connection::State::Http && connection.request_started_ms_ != 0) {
            const int deadline = connection.request_headers_complete_ ? config_.request_timeout_ms : config_.header_timeout_ms;
            if (now - connection.request_started_ms_ > deadline) {
                drop(connection, "request deadline exceeded");
                continue;
            }
        }
        if (connection.state_ == Connection::State::Http && connection.request_started_ms_ == 0 &&
            connection.out_.size() == 0 && config_.keepalive_idle_ms > 0 &&
            now - connection.last_activity_ms_ > config_.keepalive_idle_ms) {
            drop(connection, "kept alive with nothing to do");
            continue;
        }
        if (now - connection.last_activity_ms_ > limit) drop(connection, "idle timeout");
    }
}

void Server::for_each_connection(const std::function<void(Connection&)>& fn) {
    for (auto& entry : connections_) {
        if (entry.second->state_ == Connection::State::WebSocket) fn(*entry.second);
    }
}

}  // namespace fernsdr
