// Reporting what the decoders hear to PSK Reporter (pskreporter.info), where
// operators everywhere look up who heard them and how well.
//
// The receiver reports, not the decoders: they run without a network. What
// is sent follows the service's rules (pskreporter.info/pskdev.html): IPFIX
// over UDP, at most one datagram every five minutes and not in step with
// the clock, each station at most once an hour unless it changed, and only
// decodes that were made automatically and with confidence: no decode the
// decoder itself calls doubtful, and no station known only by a hash.
#pragma once

#include "../util/json.h"
#include "decodes.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fernsdr {

// Who is listening: the station's callsign and locator, what decodes, and
// the antenna, which the map shows.
struct SpotReceiver {
    std::string callsign;
    std::string locator;
    std::string software;
    std::string antenna;
};

struct Spot {
    std::string call;
    std::string grid;  // empty when the message gave none
    std::string mode;  // as ADIF names it: FT8
    uint32_t frequency_hz = 0;
    int snr = 0;
    uint32_t time_s = 0;  // when it was heard
};

// Why `receiver` cannot report, or "" when it can: a callsign of 3 to 12 of
// A-Z 0-9 /, and a locator of 4 or 6 characters.
std::string spot_receiver_problem(const SpotReceiver& receiver);

// The datagrams for a batch: the receiver's record and as many spots as
// fit in `max_bytes` each, with the record formats (templates) when asked.
class PskReporterPacker {
public:
    explicit PskReporterPacker(uint32_t stream_id) : stream_id_(stream_id) {}
    std::vector<std::vector<uint8_t>> pack(const SpotReceiver& receiver, const std::vector<Spot>& spots,
                                           uint32_t now_s, bool with_templates, size_t max_bytes = 1400);

private:
    uint32_t stream_id_;
    uint32_t sequence_ = 0;  // data records sent so far, as IPFIX counts them
};

class SpotReporter {
public:
    // Sends one datagram; the default resolves the server and sends over UDP.
    using Send = std::function<bool(const std::vector<uint8_t>& datagram, std::string& error)>;

    SpotReporter();
    ~SpotReporter();

    // Who reports, and where to; `host` empty stops reporting. Spots
    // waiting are kept across a change of receiver.
    void configure(const SpotReceiver& receiver, const std::string& host, uint16_t port);
    // Tests put their own sender in, and a clock: seconds, monotonic.
    void set_sender(Send send);
    void set_clock(std::function<int64_t()> now_s);
    // What decodes, asked for at each send: the receiver and the decoder
    // modules that run then, with their versions. The configured software
    // name is used when it is not set or gives nothing.
    void set_software(std::function<std::string()> software);

    // A decode to report, if it is one to report (see the file comment).
    void add(const Decode& decode);
    // Sends what is waiting now, from the calling thread (tests, shutdown).
    void flush();

    Json status() const;

private:
    void run();
    void send_batch();
    bool default_send(const std::vector<uint8_t>& datagram, std::string& error);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    SpotReceiver receiver_;
    std::string host_;
    uint16_t port_ = 4739;
    std::string problem_;
    Send send_;
    std::function<int64_t()> now_s_;
    std::function<std::string()> software_;
    PskReporterPacker packer_;
    std::vector<Spot> pending_;
    // "CALL|MHz" to when it was last reported and with which locator.
    struct Reported {
        int64_t at = 0;
        std::string grid;
    };
    std::map<std::string, Reported> reported_;
    int64_t next_send_ = 0;
    int64_t templates_due_ = 0;
    int packets_ = 0;
    uint64_t spots_sent_ = 0;
    int64_t last_sent_wall_ = 0;
    std::string last_error_;
    // The server's address and until when it holds.
    uint32_t address_ = 0;
    int64_t address_until_ = 0;
    std::thread thread_;
};

}  // namespace fernsdr
