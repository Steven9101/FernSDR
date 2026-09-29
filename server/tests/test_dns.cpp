#include "../src/util/dns.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace dns = fernsdr::dns;

namespace {

// An answer to `query`: its header and question, then the given records.
std::vector<uint8_t> answer_to(const std::vector<uint8_t>& query, const std::vector<std::vector<uint8_t>>& records,
                               uint8_t rcode = 0) {
    std::vector<uint8_t> a(query);
    a[2] = 0x81;
    a[3] = static_cast<uint8_t>(0x80 | rcode);
    a[6] = 0;
    a[7] = static_cast<uint8_t>(records.size());
    for (const auto& r : records) a.insert(a.end(), r.begin(), r.end());
    return a;
}

// A record owned by the name at `owner` (a compression pointer), of `type`,
// with `data`.
std::vector<uint8_t> record(uint16_t owner, uint16_t type, const std::vector<uint8_t>& data, uint32_t ttl = 300) {
    std::vector<uint8_t> r = {static_cast<uint8_t>(0xc0 | owner >> 8), static_cast<uint8_t>(owner), 0,
                              static_cast<uint8_t>(type), 0, 1, static_cast<uint8_t>(ttl >> 24),
                              static_cast<uint8_t>(ttl >> 16), static_cast<uint8_t>(ttl >> 8), static_cast<uint8_t>(ttl),
                              static_cast<uint8_t>(data.size() >> 8), static_cast<uint8_t>(data.size())};
    r.insert(r.end(), data.begin(), data.end());
    return r;
}

std::string text(const in_addr& address) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address, buf, sizeof buf);
    return buf;
}

}  // namespace

TEST_CASE(dns_query_and_answer_are_read_as_the_protocol_says) {
    const auto q = dns::build_query("report.pskreporter.info", 0x1234);
    CHECK(!q.empty());
    CHECK(dns::build_query("bad name", 1).empty());
    CHECK(dns::build_query("a..b", 1).empty());
    CHECK(dns::build_query(std::string(64, 'a') + ".info", 1).empty());

    std::vector<in_addr> out;
    uint32_t ttl = 0;
    std::string why;
    // The question starts at 12: point there.
    auto a = answer_to(q, {record(12, 1, {192, 0, 2, 7}, 600), record(12, 1, {192, 0, 2, 8}, 60)});
    CHECK(dns::parse_answer(a.data(), a.size(), "REPORT.pskreporter.info", 0x1234, out, ttl, why));
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ_STR(text(out[0]), "192.0.2.7");
    CHECK_EQ(ttl, 60u);

    // Through a CNAME: its target is written out after the first record.
    std::vector<uint8_t> target = {3, 'w', 'e', 'b', 0xc0, 19};  // "web" + ".pskreporter.info" (at 19)
    auto cname = record(12, 5, target);
    const uint16_t target_at = static_cast<uint16_t>(q.size() + 12);  // where the target name starts
    auto chained = answer_to(q, {cname, record(target_at, 1, {198, 51, 100, 4})});
    CHECK(dns::parse_answer(chained.data(), chained.size(), "report.pskreporter.info", 0x1234, out, ttl, why));
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ_STR(text(out[0]), "198.51.100.4");
    // An address for some other name is not taken.
    auto elsewhere = answer_to(q, {record(target_at, 1, {203, 0, 113, 9})});
    CHECK(!dns::parse_answer(elsewhere.data(), elsewhere.size(), "report.pskreporter.info", 0x1234, out, ttl, why));

    // Refused: another id, an error, a truncated message, a pointer loop.
    CHECK(!dns::parse_answer(a.data(), a.size(), "report.pskreporter.info", 0x1235, out, ttl, why));
    auto error = answer_to(q, {}, 3);
    CHECK(!dns::parse_answer(error.data(), error.size(), "report.pskreporter.info", 0x1234, out, ttl, why));
    CHECK(why == "no such name");
    CHECK(!dns::parse_answer(a.data(), a.size() - 3, "report.pskreporter.info", 0x1234, out, ttl, why));
    auto loop = answer_to(q, {{0xc0, static_cast<uint8_t>(q.size()), 0, 1, 0, 1, 0, 0, 0, 1, 0, 4, 1, 2, 3, 4}});
    CHECK(!dns::parse_answer(loop.data(), loop.size(), "report.pskreporter.info", 0x1234, out, ttl, why));
    auto other = dns::build_query("example.org", 0x1234);
    auto wrong = answer_to(other, {record(12, 1, {192, 0, 2, 1})});
    CHECK(!dns::parse_answer(wrong.data(), wrong.size(), "report.pskreporter.info", 0x1234, out, ttl, why));
}

TEST_CASE(dns_resolves_through_a_nameserver_and_reads_resolv_conf) {
    // A nameserver on loopback that answers every A query with 192.0.2.44.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    socklen_t size = sizeof address;
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    const uint16_t port = ntohs(address.sin_port);
    std::thread server([fd] {
        uint8_t buf[512];
        sockaddr_in from{};
        socklen_t from_size = sizeof from;
        const ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (n <= 0) return;
        std::vector<uint8_t> q(buf, buf + n);
        auto a = answer_to(q, {record(12, 1, {192, 0, 2, 44})});
        sendto(fd, a.data(), a.size(), 0, reinterpret_cast<sockaddr*>(&from), from_size);
    });
    std::vector<in_addr> out;
    std::string error;
    CHECK(dns::resolve_ipv4("report.pskreporter.info", out, error, 2000, {"127.0.0.1"}, port));
    server.join();
    close(fd);
    CHECK_EQ(out.size(), 1u);
    if (!out.empty()) CHECK_EQ_STR(text(out[0]), "192.0.2.44");
    CHECK(dns::resolve_ipv4("192.0.2.9", out, error));
    CHECK(!dns::resolve_ipv4("not a name", out, error));

    char path[] = "/tmp/fernsdr-resolv-XXXXXX";
    const int file = mkstemp(path);
    CHECK(file >= 0);
    close(file);
    std::ofstream(path) << "# comment\nsearch lan\nnameserver 192.0.2.53\nnameserver fe80::1%eth0\nnameserver bogus\n";
    const auto servers = dns::nameservers(path);
    CHECK_EQ(servers.size(), 2u);
    CHECK_EQ_STR(servers[0], "192.0.2.53");
    CHECK_EQ_STR(servers[1], "fe80::1");
    std::ofstream(path) << "";
    CHECK_EQ_STR(dns::nameservers(path)[0], "127.0.0.1");
    unlink(path);
}
