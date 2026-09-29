#include "dns.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "password.h"

namespace fernsdr::dns {

namespace {

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypeCname = 5;
constexpr uint16_t kClassIn = 1;

bool plain_host(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    size_t label = 0;
    for (const char c : host) {
        if (c == '.') {
            if (label == 0) return false;
            label = 0;
            continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-') || ++label > 63) return false;
    }
    return label > 0;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A name at `at`, following compression pointers; `next` is where the
// record goes on after the name as it is written. Pointers must point
// backwards and are limited in number, so a crafted message cannot loop.
bool read_name(const uint8_t* data, size_t size, size_t at, std::string& name, size_t& next) {
    name.clear();
    bool jumped = false;
    int jumps = 0;
    size_t pos = at;
    while (true) {
        if (pos >= size) return false;
        const uint8_t length = data[pos];
        if ((length & 0xc0) == 0xc0) {
            if (pos + 1 >= size || ++jumps > 16) return false;
            const size_t target = static_cast<size_t>((length & 0x3f) << 8 | data[pos + 1]);
            if (target >= pos) return false;
            if (!jumped) next = pos + 2;
            jumped = true;
            pos = target;
            continue;
        }
        if (length & 0xc0) return false;
        if (length == 0) {
            if (!jumped) next = pos + 1;
            return true;
        }
        if (pos + 1 + length > size || name.size() + length + 1 > 255) return false;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(data + pos + 1), length);
        pos += 1 + length;
    }
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint16_t random_id() {
    const std::string hex = random_hex(2);
    return static_cast<uint16_t>(std::stoul(hex, nullptr, 16));
}

}  // namespace

std::vector<std::string> nameservers(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string key, address;
        if (!(words >> key >> address) || key != "nameserver") continue;
        in_addr v4{};
        in6_addr v6{};
        // A scope (fe80::1%eth0) is dropped; such a server is rarely the only one.
        const std::string bare = address.substr(0, address.find('%'));
        if (inet_pton(AF_INET, bare.c_str(), &v4) == 1 || inet_pton(AF_INET6, bare.c_str(), &v6) == 1) out.push_back(bare);
    }
    if (out.empty()) out.push_back("127.0.0.1");
    return out;
}

std::vector<uint8_t> build_query(const std::string& host, uint16_t id) {
    if (!plain_host(host)) return {};
    std::vector<uint8_t> q = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id), 0x01, 0x00,  // RD
                              0, 1, 0, 0, 0, 0, 0, 0};
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) dot = host.size();
        q.push_back(static_cast<uint8_t>(dot - start));
        q.insert(q.end(), host.begin() + static_cast<long>(start), host.begin() + static_cast<long>(dot));
        start = dot + 1;
    }
    q.push_back(0);
    q.insert(q.end(), {0, kTypeA, 0, kClassIn});
    return q;
}

bool parse_answer(const uint8_t* data, size_t size, const std::string& host, uint16_t id,
                  std::vector<in_addr>& out, uint32_t& ttl, std::string& why) {
    out.clear();
    ttl = 0;
    if (size < 12) {
        why = "the answer is shorter than a DNS header";
        return false;
    }
    if (get16(data) != id || !(data[2] & 0x80)) {
        why = "the answer is not to this query";
        return false;
    }
    if (data[2] & 0x02) {
        why = "the answer was truncated";
        return false;
    }
    const int rcode = data[3] & 0x0f;
    if (rcode != 0) {
        why = rcode == 3 ? "no such name" : "the nameserver answered with error " + std::to_string(rcode);
        return false;
    }
    const uint16_t questions = get16(data + 4);
    const uint16_t answers = get16(data + 6);
    if (questions != 1) {
        why = "the answer does not repeat the question";
        return false;
    }
    std::string name;
    size_t pos = 12;
    if (!read_name(data, size, pos, name, pos) || pos + 4 > size || lower(name) != lower(host) ||
        get16(data + pos) != kTypeA || get16(data + pos + 2) != kClassIn) {
        why = "the answer is to another question";
        return false;
    }
    pos += 4;
    // The name the addresses belong to: the host, or where its CNAMEs lead.
    std::string wanted = lower(host);
    uint32_t shortest = UINT32_MAX;
    for (uint16_t i = 0; i < answers; i++) {
        std::string owner;
        if (!read_name(data, size, pos, owner, pos) || pos + 10 > size) {
            why = "an answer record is malformed";
            return false;
        }
        const uint16_t type = get16(data + pos);
        const uint16_t klass = get16(data + pos + 2);
        const uint32_t record_ttl = get32(data + pos + 4);
        const uint16_t length = get16(data + pos + 8);
        pos += 10;
        if (pos + length > size) {
            why = "an answer record runs past the message";
            return false;
        }
        if (klass == kClassIn && lower(owner) == wanted) {
            if (type == kTypeCname) {
                std::string target;
                size_t after = 0;
                if (!read_name(data, size, pos, target, after)) {
                    why = "a CNAME record is malformed";
                    return false;
                }
                wanted = lower(target);
            } else if (type == kTypeA && length == 4) {
                in_addr address{};
                std::memcpy(&address, data + pos, 4);
                out.push_back(address);
                shortest = std::min(shortest, record_ttl);
            }
        }
        pos += length;
    }
    if (out.empty()) {
        why = "the name has no IPv4 address";
        return false;
    }
    ttl = shortest;
    return true;
}

bool resolve_ipv4(const std::string& host, std::vector<in_addr>& out, std::string& error, int timeout_ms,
                  const std::vector<std::string>& servers, uint16_t port) {
    out.clear();
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        out.push_back(literal);
        return true;
    }
    if (!plain_host(host)) {
        error = "'" + host + "' is not a host name";
        return false;
    }
    const std::vector<std::string> list = servers.empty() ? nameservers() : servers;
    error = "no nameserver answered";
    for (int attempt = 0; attempt < 2; attempt++) {
        for (const std::string& server : list) {
            sockaddr_storage address{};
            socklen_t address_size = 0;
            int family = AF_INET;
            auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
            auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
            if (inet_pton(AF_INET, server.c_str(), &v4->sin_addr) == 1) {
                v4->sin_family = AF_INET;
                v4->sin_port = htons(port);
                address_size = sizeof(sockaddr_in);
            } else if (inet_pton(AF_INET6, server.c_str(), &v6->sin6_addr) == 1) {
                family = AF_INET6;
                v6->sin6_family = AF_INET6;
                v6->sin6_port = htons(port);
                address_size = sizeof(sockaddr_in6);
            } else {
                continue;
            }
            const int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0) continue;
            // Connected: the kernel drops datagrams from anywhere but the
            // nameserver, which is the first defence against a forged answer;
            // the random id and the repeated question are the rest.
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), address_size) != 0) {
                ::close(fd);
                continue;
            }
            const uint16_t id = random_id();
            const std::vector<uint8_t> query = build_query(host, id);
            if (::send(fd, query.data(), query.size(), 0) != static_cast<ssize_t>(query.size())) {
                ::close(fd);
                continue;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (true) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
                uint8_t answer[1500];
                const ssize_t n = ::recv(fd, answer, sizeof answer, 0);
                if (n <= 0) break;
                uint32_t ttl = 0;
                std::string why;
                if (parse_answer(answer, static_cast<size_t>(n), host, id, out, ttl, why)) {
                    ::close(fd);
                    error.clear();
                    return true;
                }
                error = server + ": " + why;
                // A wrong id may be a stray; anything else from this server is its answer.
                if (why != "the answer is not to this query") break;
            }
            ::close(fd);
        }
    }
    return false;
}

}  // namespace fernsdr::dns
