// A stand-in for a direct-sampling input module such as Fern-RX888, for
// checking that the receiver keeps up with one: 16-bit real samples at tens
// of megasamples a second through the module pipe, a band starting at 0 Hz.
//
// It speaks the module protocol (docs/MODULES.md), announces s16 real at the
// band's rate with centre 0, and then writes, in real time, a carrier at
// module.tone Hz on a floor of noise. Like a real module it holds half a
// second of samples when the receiver falls behind and drops what does not
// fit, counting it in its stats and in its log, so a receiver that cannot
// keep up shows as dropped samples rather than as a module running late.
//
//   wideband-module --describe
//   wideband-module --fernsdr-module 1
#include <unistd.h>

#include <cerrno>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

bool write_all(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void event(const std::string& line) {
    const std::string text = line + "\n";
    write_all(3, text.data(), text.size());
}

void log_line(const std::string& line) {
    const std::string text = line + "\n";
    write_all(2, text.data(), text.size());
}

std::string read_line() {
    std::string line;
    char c;
    while (::read(0, &c, 1) == 1) {
        if (c == '\n') return line;
        line += c;
    }
    return line;
}

// The number after "key": in a line of JSON, or fallback.
double number_field(const std::string& line, const std::string& key, double fallback) {
    const size_t at = line.find("\"" + key + "\":");
    if (at == std::string::npos) return fallback;
    return std::strtod(line.c_str() + at + key.size() + 3, nullptr);
}

const char* describe_text =
    "{\"api\":1,\"id\":\"wideband\",\"name\":\"Wideband test\",\"version\":\"0.1.0\",\"kind\":\"input\","
    "\"settings\":[{\"key\":\"tone\",\"type\":\"number\",\"label\":\"Tone\",\"default\":7100000,\"unit\":\"Hz\","
    "\"live\":false}]}";

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--describe") {
        std::printf("%s\n", describe_text);
        return 0;
    }
    if (mode == "--list-devices") {
        std::printf("{\"devices\":[{\"index\":0,\"name\":\"Wideband test\",\"usable\":true}]}\n");
        return 0;
    }
    if (mode != "--fernsdr-module") return 2;

    event("{\"type\":\"hello\",\"api\":1,\"id\":\"wideband\",\"version\":\"0.1.0\",\"kind\":\"input\"}");
    const std::string open = read_line();
    const double rate = number_field(open, "sample_rate", 0);
    const double tone = number_field(open, "tone", 7100000);
    if (!(rate > 0) || open.find("\"signal\":\"real\"") == std::string::npos) {
        event("{\"type\":\"error\",\"code\":\"invalid\",\"message\":\"wants a real band\",\"fatal\":true}");
        return 6;
    }
    // The signal, repeated: a period in which the carrier completes a whole
    // number of cycles, so that the loop is seamless, of about four million
    // samples, so that the noise repeats less often than the receiver's
    // largest transform. Computed before ready: after ready the receiver
    // expects samples within 2 s.
    const uint64_t rate_hz = static_cast<uint64_t>(std::llround(rate));
    const uint64_t tone_hz = static_cast<uint64_t>(std::llround(tone));
    uint64_t a = rate_hz, b = tone_hz;
    while (b != 0) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    const uint64_t base = rate_hz / (a == 0 ? 1 : a);
    const size_t period = static_cast<size_t>(base * ((4000000 + base - 1) / base));
    std::vector<int16_t> signal(period);
    std::mt19937 random(1);
    std::normal_distribution<double> noise(0.0, 40.0);
    for (size_t i = 0; i < period; ++i) {
        const double phase = 2 * M_PI * static_cast<double>((i * tone_hz) % rate_hz) / rate;
        signal[i] = static_cast<int16_t>(std::lround(3000.0 * std::sin(phase) + noise(random)));
    }

    char ready[256];
    std::snprintf(ready, sizeof ready,
                  "{\"type\":\"ready\",\"format\":\"s16\",\"signal\":\"real\",\"sample_rate\":%.0f,\"center\":0,"
                  "\"device\":{\"name\":\"Wideband test\"},\"settings\":{\"tone\":%.0f}}",
                  rate, tone);
    event(ready);

    const size_t chunk = 512 * 1024;             // samples a write
    const double ring_seconds = 0.5;             // what a real module holds
    const auto start = std::chrono::steady_clock::now();
    uint64_t written = 0, dropped = 0;
    size_t position = 0;
    auto next_stats = start + std::chrono::seconds(1);
    for (;;) {
        const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const uint64_t due = static_cast<uint64_t>(now_s * rate);
        const uint64_t sent = written + dropped;
        if (sent + chunk > due) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        } else if (due - sent > ring_seconds * rate) {
            // Behind by more than the ring holds: those samples are gone.
            const uint64_t skip = due - sent - chunk;
            dropped += skip;
            position = (position + skip) % period;
        } else {
            size_t left = chunk;
            while (left > 0) {
                const size_t n = std::min(left, period - position);
                if (!write_all(1, signal.data() + position, n * 2)) return 0;
                position = (position + n) % period;
                left -= n;
            }
            written += chunk;
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            char stats[160];
            std::snprintf(stats, sizeof stats, "{\"type\":\"stats\",\"samples\":%llu,\"dropped\":%llu}",
                          static_cast<unsigned long long>(written), static_cast<unsigned long long>(dropped));
            event(stats);
            if (dropped > 0) log_line("dropped " + std::to_string(dropped) + " samples so far");
            next_stats += std::chrono::seconds(1);
        }
    }
}
