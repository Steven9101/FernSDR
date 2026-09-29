#include "../src/core/spots.h"
#include "test_util.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using fernsdr::Decode;
using fernsdr::PskReporterPacker;
using fernsdr::Spot;
using fernsdr::SpotReceiver;
using fernsdr::SpotReporter;

namespace {

uint16_t u16(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint16_t>(d[at] << 8 | d[at + 1]); }
uint32_t u32(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint32_t>(u16(d, at)) << 16 | u16(d, at + 2); }

// Reads a datagram as IPFIX: every set's length adds up to the message's,
// templates are remembered, and data records are decoded by them.
struct Read {
    bool ok = false;
    uint32_t sequence = 0;
    bool templates = false;
    std::vector<std::map<uint16_t, std::string>> receivers, spots;
};

Read read_ipfix(const std::vector<uint8_t>& d, std::map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>>& known) {
    Read r;
    if (d.size() < 16 || u16(d, 0) != 0x000a || u16(d, 2) != d.size()) return r;
    r.sequence = u32(d, 8);
    size_t at = 16;
    while (at < d.size()) {
        if (at + 4 > d.size()) return r;
        const uint16_t set = u16(d, at);
        const uint16_t length = u16(d, at + 2);
        if (length < 4 || at + length > d.size() || length % 4) return r;
        const size_t end = at + length;
        size_t p = at + 4;
        if (set == 2 || set == 3) {
            r.templates = true;
            const uint16_t id = u16(d, p);
            const uint16_t count = u16(d, p + 2);
            p += set == 3 ? 6 : 4;
            std::vector<std::pair<uint16_t, uint16_t>> fields;
            for (uint16_t i = 0; i < count; i++) {
                const uint16_t field = u16(d, p);
                const uint16_t size = u16(d, p + 2);
                p += 4;
                if (field & 0x8000) {
                    if (u32(d, p) != 30351) return r;
                    p += 4;
                }
                fields.emplace_back(field, size);
            }
            known[id] = fields;
        } else {
            const auto& fields = known.at(set);
            while (p < end) {
                // What is left in a set shorter than a record is padding.
                if (end - p < 4 && std::all_of(d.begin() + static_cast<long>(p), d.begin() + static_cast<long>(end),
                                               [](uint8_t b) { return b == 0; })) break;
                std::map<uint16_t, std::string> record;
                for (const auto& [field, size] : fields) {
                    if (size == 0xffff) {
                        const uint8_t n = d[p++];
                        record[field] = std::string(d.begin() + static_cast<long>(p), d.begin() + static_cast<long>(p + n));
                        p += n;
                    } else {
                        uint32_t v = 0;
                        for (uint16_t k = 0; k < size; k++) v = v << 8 | d[p++];
                        record[field] = std::to_string(size == 1 ? static_cast<int>(static_cast<int8_t>(v)) : static_cast<long long>(v));
                    }
                }
                (set == 0x9992 ? r.receivers : r.spots).push_back(record);
            }
        }
        at = end;
    }
    r.ok = true;
    return r;
}

SpotReceiver receiver() {
    SpotReceiver r;
    r.callsign = "DL1ABC";
    r.locator = "JO62qm";
    r.software = "FernSDR 0.1.0";
    return r;
}

Decode decode(const std::string& call, const std::string& grid = "", const std::string& quality = "bp", double snr = -12) {
    Decode d;
    d.call = call;
    d.grid = grid;
    d.quality = quality;
    d.mode = "ft8";
    d.dial_hz = 14074000;
    d.audio_hz = 1234.4;
    d.snr = snr;
    d.time_ms = 1790604000000;
    d.message = "CQ " + call + " " + grid;
    return d;
}

}  // namespace

TEST_CASE(psk_reporter_datagrams_are_ipfix_as_the_service_describes) {
    PskReporterPacker packer(0x01020304);
    std::vector<Spot> spots;
    spots.push_back({"K1ABC", "FN42", "FT8", 14075234, -12, 1790604000});
    spots.push_back({"JA1XYZ", "", "FT8", 7075500, 5, 1790604015});
    const auto datagrams = packer.pack(receiver(), spots, 1790604100, true);
    CHECK_EQ(datagrams.size(), 1u);
    const auto& d = datagrams[0];
    CHECK_EQ(u32(d, 4), 1790604100u);
    CHECK_EQ(u32(d, 12), 0x01020304u);
    // The receiver's options template, as the service's example writes it.
    const uint8_t expected[] = {0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03, 0x00, 0x01, 0x80, 0x02, 0xff, 0xff,
                                0x00, 0x00, 0x76, 0x8f, 0x80, 0x04, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f, 0x80, 0x08,
                                0xff, 0xff, 0x00, 0x00, 0x76, 0x8f, 0x00, 0x00};
    CHECK(std::memcmp(d.data() + 16, expected, sizeof expected) == 0);
    std::map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>> known;
    const Read r = read_ipfix(d, known);
    CHECK(r.ok);
    CHECK(r.templates);
    CHECK_EQ(r.sequence, 0u);
    CHECK_EQ(r.receivers.size(), 1u);
    CHECK_EQ_STR(r.receivers[0].at(0x8002), "DL1ABC");
    CHECK_EQ_STR(r.receivers[0].at(0x8004), "JO62qm");
    CHECK_EQ(r.spots.size(), 2u);
    CHECK_EQ_STR(r.spots[0].at(0x8001), "K1ABC");
    CHECK_EQ_STR(r.spots[0].at(0x8005), "14075234");
    CHECK_EQ_STR(r.spots[0].at(0x8006), "-12");
    CHECK_EQ_STR(r.spots[0].at(0x800a), "FT8");
    CHECK_EQ_STR(r.spots[0].at(0x800b), "1");
    CHECK_EQ_STR(r.spots[0].at(0x8003), "FN42");
    CHECK_EQ_STR(r.spots[0].at(0x0096), "1790604000");
    CHECK_EQ_STR(r.spots[1].at(0x8003), "");

    // A batch too big for one datagram is split, each within the limit and
    // each carrying the receiver and the formats; the sequence counts the
    // data records sent before it.
    std::vector<Spot> many;
    for (int i = 0; i < 200; i++) many.push_back({"W" + std::to_string(1000 + i), "EN52", "FT8", 14075000, -10, 1790604000});
    const auto split = packer.pack(receiver(), many, 1790604400, false);
    CHECK(split.size() > 1);
    size_t total = 0;
    uint32_t expected_sequence = 3;  // the first datagram's receiver and two spots
    for (size_t i = 0; i < split.size(); i++) {
        CHECK(split[i].size() <= 1400);
        std::map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>> seen = known;
        const Read part = read_ipfix(split[i], seen);
        CHECK(part.ok);
        CHECK_EQ(part.templates, i > 0);
        CHECK_EQ(part.sequence, expected_sequence);
        CHECK_EQ(part.receivers.size(), 1u);
        expected_sequence += static_cast<uint32_t>(1 + part.spots.size());
        total += part.spots.size();
    }
    CHECK_EQ(total, 200u);
}

TEST_CASE(spot_reporter_reports_what_the_rules_allow_and_when) {
    SpotReporter reporter;
    int64_t now = 1000;
    reporter.set_clock([&] { return now; });
    std::vector<std::vector<uint8_t>> sent;
    reporter.set_sender([&](const std::vector<uint8_t>& d, std::string&) {
        sent.push_back(d);
        return true;
    });
    // No callsign: nothing is reported, and the reporter says why.
    SpotReceiver nobody = receiver();
    nobody.callsign = "";
    reporter.configure(nobody, "report.pskreporter.info", 4739);
    reporter.add(decode("K1ABC", "FN42"));
    reporter.flush();
    CHECK(sent.empty());
    CHECK(reporter.status()["problem"].string().find("callsign") != std::string::npos);

    reporter.configure(receiver(), "report.pskreporter.info", 4739);
    reporter.add(decode("K1ABC", "FN42"));
    reporter.add(decode("K1ABC", "FN42"));             // again, at once
    reporter.add(decode("W9XYZ", "", "low"));           // doubtful
    reporter.add(decode(""));                            // known only by a hash
    reporter.add(decode("VK2DEF", "QF56", "osd", 5));
    reporter.flush();
    CHECK_EQ(sent.size(), 1u);
    std::map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>> known;
    Read r = read_ipfix(sent[0], known);
    CHECK(r.ok);
    CHECK_EQ(r.spots.size(), 2u);
    CHECK_EQ(reporter.status()["sent"].number(), 2.0);

    // Within the hour a station is reported again only when its locator
    // changed, and never within five minutes.
    now += 200;
    reporter.add(decode("K1ABC", "FN43"));
    now += 200;
    reporter.add(decode("K1ABC", "FN43"));
    reporter.add(decode("VK2DEF", "QF56"));
    reporter.flush();
    CHECK_EQ(sent.size(), 2u);
    r = read_ipfix(sent[1], known);
    CHECK_EQ(r.spots.size(), 1u);
    CHECK_EQ_STR(r.spots[0].at(0x8003), "FN43");
    // The formats went with the first three datagrams, then not for an hour.
    now += 3600;
    reporter.add(decode("VK2DEF", "QF56"));
    reporter.flush();
    reporter.add(decode("JA1XYZ", "PM95"));
    reporter.flush();
    CHECK_EQ(sent.size(), 4u);
    CHECK(read_ipfix(sent[2], known).templates);
    CHECK(!read_ipfix(sent[3], known).templates);
    // Nothing waiting, nothing sent.
    reporter.flush();
    CHECK_EQ(sent.size(), 4u);
}
