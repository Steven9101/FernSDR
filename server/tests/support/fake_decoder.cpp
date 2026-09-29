// A stand-in for a decoder module (API 2), built only for the tests. What it
// does comes from `settings.behaviour` in `open`:
//
//   good    reads frames, checks them, reports a decode every 8 frames of
//           channel 0 and its frame and gap counts as stats
//   bad     reports decodes the receiver must refuse
//   flood   reports 300 decodes for one slot
//   crash   exits right after `ready`
//   slow    stops reading samples for 6 seconds, then reads again and reports
//           how many frames said samples were lost
//   probe   reports, as an error event, whether it could open a file
//   varied  reports a slot's worth of plausible FT8 traffic from a fixed
//           list at each new 15 s slot, for looking at the pages by hand
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

void event(const std::string& line) {
    const std::string text = line + "\n";
    (void)!::write(3, text.data(), text.size());
}

bool read_exact(int fd, void* out, size_t size) {
    size_t got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, static_cast<char*>(out) + got, size - got);
        if (n > 0) got += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

// The value of "key":"..." or "key":number in a JSON line, crudely: enough
// for the one message this program reads.
std::string field(const std::string& line, const std::string& key) {
    const size_t at = line.find("\"" + key + "\":");
    if (at == std::string::npos) return "";
    size_t start = at + key.size() + 3;
    if (line[start] == '"') {
        const size_t end = line.find('"', start + 1);
        return line.substr(start + 1, end - start - 1);
    }
    size_t end = start;
    while (end < line.size() && line[end] != ',' && line[end] != '}') end++;
    return line.substr(start, end - start);
}

uint64_t le(const unsigned char* p, int bytes) {
    uint64_t value = 0;
    for (int i = bytes - 1; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "--fernsdr-module" || std::string(argv[2]) != "2") return 2;
    event("{\"type\":\"hello\",\"api\":2,\"id\":\"fake\",\"version\":\"1.2.3\",\"kind\":\"decoder\",\"modes\":[\"ft8\"]}");

    std::string open;
    char c;
    while (::read(0, &c, 1) == 1 && c != '\n') open.push_back(c);
    const std::string behaviour = field(open, "behaviour");
    const std::string channel = field(open, "id");
    const double offset = std::atof(field(open, "offset").c_str());
    event("{\"type\":\"ready\"}");

    if (behaviour == "crash") return 9;
    if (behaviour == "probe") {
        const int fd = ::open("/etc/hostname", O_RDONLY);
        event(std::string("{\"type\":\"error\",\"code\":\"internal\",\"message\":\"file open ") +
              (fd >= 0 ? "allowed" : "denied") + "\",\"fatal\":false}");
    }
    if (behaviour == "bad") {
        const std::string base = "{\"type\":\"decode\",\"channel\":\"" + channel + "\",\"time\":1790000000000,";
        event(base + "\"freq\":99999,\"snr\":-10,\"dt\":0.1,\"message\":\"CQ X\"}");               // outside the channel
        event(base + "\"freq\":2000,\"snr\":-10,\"dt\":0.1,\"message\":\"CQ X\",\"grid\":\"ZZ99\"}");  // no such grid
        event(base + "\"freq\":2000,\"snr\":-10,\"dt\":0.1,\"message\":\"\"}");                    // no message
        event("{\"type\":\"decode\",\"channel\":\"elsewhere\",\"time\":1790000000000,\"freq\":2000,\"snr\":-10,\"dt\":0.1,\"message\":\"CQ X\"}");
    }
    if (behaviour == "flood") {
        for (int i = 0; i < 300; i++) {
            event("{\"type\":\"decode\",\"channel\":\"" + channel + "\",\"time\":1790000000000,\"freq\":" +
                  std::to_string(static_cast<int>(offset)) + ",\"snr\":-10,\"dt\":0.1,\"message\":\"CQ N" + std::to_string(i) + "\"}");
        }
    }
    if (behaviour == "slow") std::this_thread::sleep_for(std::chrono::seconds(6));

    uint64_t frames = 0, gaps = 0, expected_index = 0;
    bool first = true;
    std::vector<unsigned char> samples;
    for (;;) {
        unsigned char header[32];
        if (!read_exact(4, header, sizeof(header))) return 0;
        if (std::memcmp(header, "FDR1", 4) != 0) {
            event("{\"type\":\"error\",\"code\":\"invalid\",\"message\":\"bad frame magic\",\"fatal\":true}");
            return 6;
        }
        const uint16_t ch = static_cast<uint16_t>(le(header + 4, 2));
        const uint16_t flags = static_cast<uint16_t>(le(header + 6, 2));
        const uint32_t count = static_cast<uint32_t>(le(header + 8, 4));
        const uint64_t index = le(header + 16, 8);
        const int64_t utc_us = static_cast<int64_t>(le(header + 24, 8));
        samples.resize(static_cast<size_t>(count) * 8);
        if (!read_exact(4, samples.data(), samples.size())) return 0;
        if (ch != 0) continue;
        if (first && index != 0) {
            event("{\"type\":\"error\",\"code\":\"invalid\",\"message\":\"the first frame is not index 0\",\"fatal\":true}");
            return 6;
        }
        if (flags & 1) gaps++;
        else if (!first && index != expected_index) {
            event("{\"type\":\"error\",\"code\":\"invalid\",\"message\":\"a gap without its flag\",\"fatal\":true}");
            return 6;
        }
        first = false;
        expected_index = index + count;
        frames++;
        if (behaviour == "varied") {
            static long long last_slot = -1;
            static const char* const traffic[][3] = {
                {"CQ DL1ABC JO31", "DL1ABC", "JO31"}, {"CQ K1ABC FN42", "K1ABC", "FN42"},
                {"CQ DX JA1XYZ PM95", "JA1XYZ", "PM95"}, {"K1ABC EA8ZZ IL18", "EA8ZZ", "IL18"},
                {"CQ VK2DEF QF56", "VK2DEF", "QF56"}, {"DL1ABC W9XYZ EN52", "W9XYZ", "EN52"},
                {"CQ PY2AB GG66", "PY2AB", "GG66"}, {"W9XYZ DL1ABC -14", "", ""},
                {"CQ ZS6AB KG33", "ZS6AB", "KG33"}, {"CQ G4ABC IO91", "G4ABC", "IO91"},
                {"EA8ZZ K1ABC R-09", "", ""}, {"CQ UA9ABC MO06", "UA9ABC", "MO06"},
                {"CQ VE3XYZ FN03", "VE3XYZ", "FN03"}, {"G4ABC OH2XX RR73", "", ""},
            };
            const long long slot_ms = (utc_us / 1000) / 15000 * 15000;
            if (slot_ms != last_slot) {
                last_slot = slot_ms;
                const size_t total = sizeof(traffic) / sizeof(traffic[0]);
                for (size_t i = 0; i < 6; i++) {
                    const auto& t = traffic[(static_cast<size_t>(slot_ms / 15000) * 5 + i) % total];
                    std::string extra;
                    if (t[1][0]) extra = std::string(",\"call\":\"") + t[1] + "\",\"grid\":\"" + t[2] + "\"";
                    event("{\"type\":\"decode\",\"channel\":\"" + channel + "\",\"time\":" + std::to_string(slot_ms) +
                          ",\"freq\":" + std::to_string(400 + static_cast<int>(i) * 410) + ",\"snr\":" +
                          std::to_string(-22 + static_cast<int>((slot_ms / 15000 + i * 7) % 30)) +
                          ",\"dt\":0.2,\"message\":\"" + t[0] + "\"" + extra + ",\"quality\":\"bp\"}");
                }
            }
        }
        if (behaviour == "good" && frames % 8 == 0) {
            const long long slot_ms = (utc_us / 1000) / 15000 * 15000;
            event("{\"type\":\"decode\",\"channel\":\"" + channel + "\",\"time\":" + std::to_string(slot_ms) +
                  ",\"freq\":" + std::to_string(static_cast<int>(offset)) +
                  ",\"snr\":-12,\"dt\":0.3,\"message\":\"CQ DL1ABC JO31\",\"call\":\"DL1ABC\",\"grid\":\"JO31\",\"quality\":\"bp\"}");
        }
        if (frames % 4 == 0 || behaviour == "slow") {
            event("{\"type\":\"stats\",\"channels\":[{\"id\":\"" + channel + "\",\"slots\":" + std::to_string(frames) +
                  ",\"decodes\":0,\"late\":" + std::to_string(gaps) + ",\"cpu_ms\":1}]}");
        }
    }
}
