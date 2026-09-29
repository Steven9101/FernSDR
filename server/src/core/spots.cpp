#include "spots.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <random>

#include "../util/dns.h"
#include "../util/log.h"
#include "directory.h"

namespace fernsdr {

namespace {

// Field ids of PSK Reporter's enterprise (30351), and IANA's flowStartSeconds.
constexpr uint16_t kSenderCallsign = 0x8001;
constexpr uint16_t kReceiverCallsign = 0x8002;
constexpr uint16_t kSenderLocator = 0x8003;
constexpr uint16_t kReceiverLocator = 0x8004;
constexpr uint16_t kFrequency = 0x8005;
constexpr uint16_t kSnr = 0x8006;
constexpr uint16_t kDecoderSoftware = 0x8008;
constexpr uint16_t kAntennaInformation = 0x8009;
constexpr uint16_t kMode = 0x800a;
constexpr uint16_t kInformationSource = 0x800b;
constexpr uint16_t kFlowStartSeconds = 0x0096;
constexpr uint16_t kVariable = 0xffff;
constexpr uint32_t kEnterprise = 30351;
constexpr uint16_t kReceiverTemplate = 0x9992;
constexpr uint16_t kSenderTemplate = 0x9993;

constexpr int64_t kBatchSeconds = 300;
constexpr int64_t kRepeatSeconds = 3600;
constexpr int64_t kTemplateSeconds = 3600;
constexpr size_t kMaxPending = 5000;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v >> 16));
    put16(out, static_cast<uint16_t>(v));
}
void set16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
    out[at] = static_cast<uint8_t>(v >> 8);
    out[at + 1] = static_cast<uint8_t>(v);
}
void put_string(std::vector<uint8_t>& out, const std::string& text) {
    const size_t n = std::min<size_t>(text.size(), 254);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), text.begin(), text.begin() + static_cast<long>(n));
}
void put_field(std::vector<uint8_t>& out, uint16_t id, uint16_t length) {
    put16(out, id);
    put16(out, length);
    if (id & 0x8000) put32(out, kEnterprise);
}
void pad(std::vector<uint8_t>& out, size_t set_start) {
    while ((out.size() - set_start) % 4) out.push_back(0);
    set16(out, set_start + 2, static_cast<uint16_t>(out.size() - set_start));
}

std::vector<uint8_t> templates(bool with_antenna) {
    std::vector<uint8_t> out;
    // The receiver's record: an options template with one scope field.
    size_t start = out.size();
    put16(out, 3);
    put16(out, 0);
    put16(out, kReceiverTemplate);
    put16(out, with_antenna ? 4 : 3);
    put16(out, 1);
    put_field(out, kReceiverCallsign, kVariable);
    put_field(out, kReceiverLocator, kVariable);
    put_field(out, kDecoderSoftware, kVariable);
    if (with_antenna) put_field(out, kAntennaInformation, kVariable);
    pad(out, start);
    // A spot: the station, its frequency and SNR, the mode, how it was
    // found, its locator (empty when it sent none) and when.
    start = out.size();
    put16(out, 2);
    put16(out, 0);
    put16(out, kSenderTemplate);
    put16(out, 7);
    put_field(out, kSenderCallsign, kVariable);
    put_field(out, kFrequency, 4);
    put_field(out, kSnr, 1);
    put_field(out, kMode, kVariable);
    put_field(out, kInformationSource, 1);
    put_field(out, kSenderLocator, kVariable);
    put_field(out, kFlowStartSeconds, 4);
    pad(out, start);
    return out;
}

bool plain_call(const std::string& call) {
    return call.size() >= 3 && call.size() <= 12 && std::all_of(call.begin(), call.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
           });
}

int64_t wall_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string spot_receiver_problem(const SpotReceiver& receiver) {
    std::string call = receiver.callsign;
    for (char& c : call) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!plain_call(call)) return "the station's callsign (Station page, operator) is not a callsign";
    if (!valid_grid_locator(receiver.locator) || (receiver.locator.size() != 4 && receiver.locator.size() != 6)) {
        return "the station's locator (Station page, grid square) is not a 4 or 6 character locator";
    }
    return "";
}

std::vector<std::vector<uint8_t>> PskReporterPacker::pack(const SpotReceiver& receiver, const std::vector<Spot>& spots,
                                                         uint32_t now_s, bool with_templates, size_t max_bytes) {
    std::vector<std::vector<uint8_t>> datagrams;
    const bool with_antenna = !receiver.antenna.empty();
    size_t next = 0;
    bool first = true;
    do {
        std::vector<uint8_t> out;
        put16(out, 0x000a);
        put16(out, 0);
        put32(out, now_s);
        put32(out, sequence_);
        put32(out, stream_id_);
        if (with_templates || !first) {
            // Every datagram that is not the first of a timed batch carries
            // the formats too: UDP may lose the one that had them.
            const auto t = templates(with_antenna);
            out.insert(out.end(), t.begin(), t.end());
        }
        size_t start = out.size();
        put16(out, kReceiverTemplate);
        put16(out, 0);
        put_string(out, receiver.callsign);
        put_string(out, receiver.locator);
        put_string(out, receiver.software);
        if (with_antenna) put_string(out, receiver.antenna);
        pad(out, start);
        uint32_t records = 1;
        start = out.size();
        put16(out, kSenderTemplate);
        put16(out, 0);
        size_t in_set = 0;
        for (; next < spots.size(); next++) {
            const Spot& s = spots[next];
            std::vector<uint8_t> r;
            put_string(r, s.call);
            put32(r, s.frequency_hz);
            r.push_back(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(s.snr, -128, 127))));
            put_string(r, s.mode);
            r.push_back(1);  // informationSource: found automatically
            put_string(r, s.grid);
            put32(r, s.time_s);
            // Room for this record and the set's padding.
            if (in_set > 0 && out.size() + r.size() + 3 > max_bytes) break;
            out.insert(out.end(), r.begin(), r.end());
            in_set++;
            records++;
        }
        if (in_set > 0) pad(out, start);
        else out.resize(start);
        set16(out, 2, static_cast<uint16_t>(out.size()));
        sequence_ += records;
        datagrams.push_back(std::move(out));
        first = false;
    } while (next < spots.size());
    return datagrams;
}

SpotReporter::SpotReporter() : packer_(static_cast<uint32_t>(std::random_device{}())) {
    now_s_ = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    send_ = [this](const std::vector<uint8_t>& datagram, std::string& error) { return default_send(datagram, error); };
    thread_ = std::thread([this] { run(); });
}

SpotReporter::~SpotReporter() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void SpotReporter::configure(const SpotReceiver& receiver, const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = receiver.callsign != receiver_.callsign || receiver.locator != receiver_.locator ||
                         host != host_ || port != port_;
    receiver_ = receiver;
    for (char& c : receiver_.callsign) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    host_ = host;
    port_ = port;
    problem_ = host.empty() ? "" : spot_receiver_problem(receiver_);
    if (changed) {
        // A new identity or server starts with the formats again.
        packets_ = 0;
        address_until_ = 0;
    }
    if (next_send_ == 0) {
        // The first batch a random while after starting, so that receivers
        // started together do not report together.
        std::random_device random;
        next_send_ = now_s_() + 60 + static_cast<int64_t>(random() % 120);
    }
}

void SpotReporter::set_sender(Send send) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_ = std::move(send);
}

void SpotReporter::set_software(std::function<std::string()> software) {
    std::lock_guard<std::mutex> lock(mutex_);
    software_ = std::move(software);
}

void SpotReporter::set_clock(std::function<int64_t()> now_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_s_ = std::move(now_s);
}

void SpotReporter::add(const Decode& decode) {
    if (decode.quality == "low" || !plain_call(decode.call)) return;
    Spot spot;
    spot.call = decode.call;
    spot.grid = decode.grid;
    spot.mode = decode.mode;
    for (char& c : spot.mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const double hz = std::round(decode.dial_hz + decode.audio_hz);
    if (!(hz > 0.0 && hz < 4.2e9)) return;
    spot.frequency_hz = static_cast<uint32_t>(hz);
    spot.snr = static_cast<int>(std::lround(decode.snr));
    spot.time_s = static_cast<uint32_t>(decode.time_ms / 1000);
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_.empty() || !problem_.empty()) return;
    // Once an hour per station and band, sooner only when its locator
    // changed, and never within five minutes.
    const std::string key = spot.call + "|" + std::to_string(spot.frequency_hz / 1000000);
    const int64_t now = now_s_();
    const auto found = reported_.find(key);
    if (found != reported_.end()) {
        const int64_t since = now - found->second.at;
        const bool moved = !spot.grid.empty() && spot.grid != found->second.grid;
        if (since < kBatchSeconds || (since < kRepeatSeconds && !moved)) return;
    }
    reported_[key] = {now, spot.grid.empty() && found != reported_.end() ? found->second.grid : spot.grid};
    if (pending_.size() < kMaxPending) pending_.push_back(std::move(spot));
}

void SpotReporter::flush() {
    send_batch();
}

void SpotReporter::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, std::chrono::seconds(5));
        if (stopping_) break;
        const int64_t now = now_s_();
        if (next_send_ == 0 || now < next_send_) continue;
        // Five minutes and a random part, so that the sends of many
        // receivers do not fall on the same second.
        std::random_device random;
        next_send_ = now + kBatchSeconds + static_cast<int64_t>(random() % 30);
        lock.unlock();
        send_batch();
        lock.lock();
    }
}

void SpotReporter::send_batch() {
    std::function<std::string()> software;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        software = software_;
    }
    // Asked outside the lock: it looks at the running decoders.
    const std::string decoding = software ? software() : "";
    std::vector<Spot> batch;
    SpotReceiver receiver;
    Send send;
    bool with_templates = false;
    std::vector<std::vector<uint8_t>> datagrams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty() || host_.empty() || !problem_.empty()) return;
        batch.swap(pending_);
        receiver = receiver_;
        if (!decoding.empty()) receiver.software = decoding;
        send = send_;
        const int64_t now = now_s_();
        // The formats with the first three datagrams and then once an hour.
        with_templates = packets_ < 3 || now >= templates_due_;
        if (with_templates) templates_due_ = now + kTemplateSeconds;
        datagrams = packer_.pack(receiver, batch, static_cast<uint32_t>(wall_seconds()), with_templates);
        packets_ += static_cast<int>(datagrams.size());
        // Stale entries go, so the map stays as small as an hour's stations.
        for (auto it = reported_.begin(); it != reported_.end();) {
            if (now - it->second.at > 2 * kRepeatSeconds) it = reported_.erase(it);
            else ++it;
        }
    }
    std::string error;
    size_t sent = 0;
    for (const auto& datagram : datagrams) {
        if (!send(datagram, error)) break;
        sent++;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (sent == datagrams.size()) {
        spots_sent_ += batch.size();
        last_sent_wall_ = wall_seconds();
        last_error_.clear();
    } else {
        last_error_ = error;
        LOG_WARN("spots", "PSK Reporter: %s", error.c_str());
    }
}

bool SpotReporter::default_send(const std::vector<uint8_t>& datagram, std::string& error) {
    std::string host;
    uint16_t port = 0;
    uint32_t address = 0;
    int64_t until = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host = host_;
        port = port_;
        address = address_;
        until = address_until_;
    }
    const int64_t now = now_s_();
    if (until <= now) {
        std::vector<in_addr> found;
        if (!dns::resolve_ipv4(host, found, error)) {
            error = "cannot look up " + host + ": " + error;
            if (address == 0) return false;  // keep using the last one that worked
        } else {
            address = found.front().s_addr;
            std::lock_guard<std::mutex> lock(mutex_);
            address_ = address;
            address_until_ = now + 3600;
        }
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = "cannot open a UDP socket";
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = address;
    const ssize_t n = ::sendto(fd, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    ::close(fd);
    if (n != static_cast<ssize_t>(datagram.size())) {
        error = "the datagram could not be sent";
        return false;
    }
    return true;
}

Json SpotReporter::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::make_object();
    out.set("service", "pskreporter");
    out.set("enabled", !host_.empty() && problem_.empty());
    if (!problem_.empty()) out.set("problem", problem_);
    out.set("waiting", static_cast<double>(pending_.size()));
    out.set("sent", static_cast<double>(spots_sent_));
    if (last_sent_wall_ > 0) out.set("last_sent_ms", static_cast<double>(last_sent_wall_) * 1000.0);
    if (!last_error_.empty()) out.set("error", last_error_);
    return out;
}

}  // namespace fernsdr
