#include "source_udp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

#include "../net/server.h"
#include "../util/config.h"
#include "../util/log.h"

namespace fernsdr {

namespace {

constexpr size_t kMaxDatagram = 65536;
// RTP fixed header length; ka9q-radio uses a plain 12-byte header.
constexpr size_t kRtpHeaderBytes = 12;

class UdpSource : public Source {
public:
    UdpSource(std::string bind_address, int port, std::string multicast_group, SourceFormat format,
              double sample_rate, double center_hz, bool rtp, std::vector<std::string> senders)
        : bind_address_(std::move(bind_address)),
          port_(port),
          multicast_group_(std::move(multicast_group)),
          format_(format),
          sample_rate_(sample_rate),
          center_hz_(center_hz),
          rtp_(rtp),
          senders_(std::move(senders)) {}

    ~UdpSource() override { stop(); }

    bool start(std::string& error) override {
        stop();
        interrupted_.store(false);
        total_samples_.store(0);
        dropped_.store(0);
        pending_iq_.clear();
        pending_real_.clear();
        have_sequence_ = false;
        last_sequence_ = 0;
        // Whatever arrives is played to every listener, so a port open to the
        // network with no list of senders is worth a line in the log.
        if (senders_.empty() && !address_matches_cidr(bind_address_, "loopback")) {
            LOG_WARN("source", "UDP port %d takes samples from anyone who can reach it; "
                     "list the senders with 'senders' to limit that", port_);
        }
        ignored_logged_ = false;
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            return false;
        }

        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // A generous receive buffer: a stall in the DSP thread should cost
        // latency, not a burst of lost samples.
        int rcvbuf = 4 * 1024 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port_));
        address.sin_addr.s_addr = multicast_group_.empty() ? inet_addr(bind_address_.c_str()) : INADDR_ANY;

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = "cannot bind UDP " + bind_address_ + ":" + std::to_string(port_) + ": " +
                    std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        if (!multicast_group_.empty()) {
            ip_mreq request{};
            request.imr_multiaddr.s_addr = inet_addr(multicast_group_.c_str());
            request.imr_interface.s_addr = inet_addr(bind_address_.c_str());
            if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) != 0) {
                error = "cannot join multicast group " + multicast_group_ + ": " + std::strerror(errno);
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            LOG_INFO("source", "joined multicast %s on %s", multicast_group_.c_str(), bind_address_.c_str());
        }

        datagram_.resize(kMaxDatagram);
        connected_ = true;
        return true;
    }

    void stop() override {
        interrupt();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        connected_ = false;
    }

    void interrupt() override { interrupted_.store(true); }

    SignalKind kind() const override { return format_.kind; }

    bool read(cfloat* out, size_t count) override {
        if (format_.kind != SignalKind::Iq) return false;
        if (!fill(count)) return false;
        std::memcpy(out, pending_iq_.data(), count * sizeof(cfloat));
        pending_iq_.erase(pending_iq_.begin(), pending_iq_.begin() + static_cast<long>(count));
        total_samples_ += count;
        return true;
    }

    bool read_real(float* out, size_t count) override {
        if (format_.kind != SignalKind::Real) return false;
        if (!fill(count)) return false;
        std::memcpy(out, pending_real_.data(), count * sizeof(float));
        pending_real_.erase(pending_real_.begin(), pending_real_.begin() + static_cast<long>(count));
        total_samples_ += count;
        return true;
    }

private:
    size_t pending() const {
        return format_.kind == SignalKind::Iq ? pending_iq_.size() : pending_real_.size();
    }

    // Accumulates datagrams until `count` samples are available.
    bool fill(size_t count) {
        while (pending() < count) {
            if (interrupted_.load()) return false;
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);
            const ssize_t got = ::recvfrom(fd_, datagram_.data(), datagram_.size(), 0,
                                           reinterpret_cast<sockaddr*>(&from), &from_length);
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    pollfd descriptor{fd_, POLLIN, 0};
                    const int ready = ::poll(&descriptor, 1, 100);
                    if (ready >= 0 || errno == EINTR) continue;
                }
                LOG_ERROR("source", "UDP receive failed: %s", std::strerror(errno));
                connected_ = false;
                return false;
            }

            if (!senders_.empty()) {
                char text[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
                bool known = false;
                for (const std::string& cidr : senders_) known = known || address_matches_cidr(text, cidr);
                if (!known) {
                    if (!ignored_logged_) {
                        LOG_WARN("source", "UDP port %d: ignoring samples from %s, which is not in 'senders'", port_,
                                 text);
                        ignored_logged_ = true;
                    }
                    continue;
                }
            }

            size_t offset = 0;
            if (rtp_) {
                if (static_cast<size_t>(got) < kRtpHeaderBytes) continue;  // runt
                offset = kRtpHeaderBytes;
                check_rtp_sequence(datagram_.data());
            }

            const size_t stride = bytes_per_sample(format_.type, format_.kind);
            const size_t payload = (static_cast<size_t>(got) - offset) / stride;
            if (payload == 0) continue;

            if (format_.kind == SignalKind::Iq) {
                const size_t base = pending_iq_.size();
                pending_iq_.resize(base + payload);
                convert_iq(format_.type, datagram_.data() + offset, pending_iq_.data() + base, payload);
            } else {
                const size_t base = pending_real_.size();
                pending_real_.resize(base + payload);
                convert_real(format_.type, datagram_.data() + offset, pending_real_.data() + base,
                             payload);
            }
        }
        return !interrupted_.load();
    }

public:
    double sample_rate() const override { return sample_rate_; }
    double center_hz() const override { return center_hz_; }
    SourceStats stats() const override { return {total_samples_.load(), dropped_.load(), connected_.load()}; }
    const char* kind_name() const override { return "udp"; }

private:
    // Datagrams can be lost or reordered.  We do not try to reconstruct: we
    // count the gap so the operator can see it, because silently absorbing
    // loss makes an intermittent network look like an SDR fault.
    void check_rtp_sequence(const uint8_t* header) {
        const uint16_t sequence = static_cast<uint16_t>((header[2] << 8) | header[3]);
        if (have_sequence_) {
            const uint16_t expected = static_cast<uint16_t>(last_sequence_ + 1);
            if (sequence != expected) {
                const uint16_t gap = static_cast<uint16_t>(sequence - expected);
                if (gap < 1000) dropped_ += gap;
            }
        }
        last_sequence_ = sequence;
        have_sequence_ = true;
    }

    std::string bind_address_;
    int port_;
    std::string multicast_group_;
    SourceFormat format_;
    double sample_rate_;
    double center_hz_;
    bool rtp_;
    // CIDRs whose datagrams are taken; empty takes everyone's.
    std::vector<std::string> senders_;
    bool ignored_logged_ = false;

    int fd_ = -1;
    std::atomic<bool> connected_{false};
    std::atomic<bool> interrupted_{false};
    std::atomic<uint64_t> total_samples_{0};
    std::atomic<uint64_t> dropped_{0};
    uint16_t last_sequence_ = 0;
    bool have_sequence_ = false;

    std::vector<uint8_t> datagram_;
    std::vector<cfloat> pending_iq_;
    std::vector<float> pending_real_;
};

}  // namespace

std::unique_ptr<Source> make_udp_source(const ConfigSection& section, std::string& error) {
    const int port = static_cast<int>(section.get_int("port", 0));
    if (port <= 0 || port > 65535) {
        error = "[" + section.name() + "] source=udp needs a valid 'port'";
        return nullptr;
    }

    SourceFormat format;
    if (!parse_source_format(section, format, error)) return nullptr;

    const double sample_rate = section.get_double("sample_rate", 0.0);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        error = "[" + section.name() + "] sample_rate is required for a UDP source";
        return nullptr;
    }

    std::vector<std::string> senders;
    for (const std::string& entry : split_list(section.get("senders", ""))) senders.push_back(entry);
    return std::make_unique<UdpSource>(section.get("bind", "0.0.0.0"), port,
                                       section.get("multicast", ""), format, sample_rate,
                                       section.get_double("center", 0.0),
                                       section.get_bool("rtp", false), std::move(senders));
}

}  // namespace fernsdr
