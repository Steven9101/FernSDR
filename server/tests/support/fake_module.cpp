// A stand-in for a real input module, built only for the tests. Its
// behaviour comes from the `behaviour` setting in `open`, so one program can
// play every kind of module the receiver has to cope with: a good one, one
// that crashes, one that hangs, one that lies about its sample rate, one that
// refuses to stop.
//
// Also run directly: `--fds` prints the descriptors it holds, `--ignore-term`
// sleeps through SIGTERM, `--list-devices` and `--describe` answer as a module
// would.
#include <dirent.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool write_all(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t put = ::write(fd, data + written, size - written);
        if (put > 0) written += static_cast<size_t>(put);
        else if (put < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

void event(const std::string& line) {
    const std::string text = line + "\n";
    write_all(3, text.data(), text.size());
}

void log(const std::string& line) {
    const std::string text = line + "\n";
    write_all(2, text.data(), text.size());
}

std::string open_descriptors() {
    std::string out;
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return "unknown";
    const int own = dirfd(directory);
    std::vector<int> fds;
    while (const dirent* entry = readdir(directory)) {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end || fd == own) continue;
        fds.push_back(static_cast<int>(fd));
    }
    closedir(directory);
    std::sort(fds.begin(), fds.end());
    for (int fd : fds) out += (out.empty() ? "" : " ") + std::to_string(fd);
    return out;
}

// The value of "key" in a flat JSON object, as its text: enough for the
// few fields a test module reads.
std::string field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    size_t start = at + needle.size();
    if (start < json.size() && json[start] == '"') {
        const size_t end = json.find('"', start + 1);
        return json.substr(start + 1, end - start - 1);
    }
    size_t end = start;
    while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
    return json.substr(start, end - start);
}

// Reads one command line from fd 0; false at end of file.
bool read_line(std::string& buffer, std::string& line) {
    while (true) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        char chunk[4096];
        const ssize_t got = ::read(0, chunk, sizeof(chunk));
        if (got > 0) buffer.append(chunk, static_cast<size_t>(got));
        else if (got < 0 && errno == EINTR) continue;
        else return false;
    }
}

bool stdin_ready() {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(0, &set);
    timeval zero{0, 0};
    return select(1, &set, nullptr, nullptr, &zero) > 0;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void wait_for_eof() {
    char chunk[256];
    while (true) {
        const ssize_t got = ::read(0, chunk, sizeof(chunk));
        if (got == 0 || (got < 0 && errno != EINTR)) return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--fds") {
        std::printf("%s\n", open_descriptors().c_str());
        return 0;
    }
    if (mode == "--ignore-term") {
        signal(SIGTERM, SIG_IGN);
        std::printf("ready\n");
        std::fflush(stdout);
        while (true) sleep_ms(1000);
    }
    if (mode == "--list-devices") {
        std::printf("{\"devices\":[{\"index\":0,\"name\":\"Fake radio\",\"serial\":\"F00D\",\"usable\":true,"
                    "\"secret\":{\"nested\":1},\"tuning\":{\"ranges\":[[9000,31000000]],\"rates\":[768000],"
                    "\"signal\":\"iq\"}},{\"index\":1,\"name\":\"Busy radio\",\"usable\":false,"
                    "\"error\":\"in use\",\"tuning\":{\"ranges\":[[5,1]],\"rates\":[768000],\"signal\":\"iq\"}}]}\n");
        return 0;
    }
    if (mode == "--describe") {
        std::printf("{\"api\":1,\"id\":\"fake\",\"name\":\"Fake\",\"version\":\"1.0.0\",\"kind\":\"input\",\"settings\":[]}\n");
        return 0;
    }
    if (mode != "--fernsdr-module") return 2;

    event(R"({"type":"hello","api":1,"id":"fake","version":"1.0.0","kind":"input"})");
    std::string buffer, line;
    // The behaviour is only known from `open`; until then this is any module.
    if (!read_line(buffer, line)) return 0;
    const std::string behaviour = field(line, "behaviour");
    const double rate = std::atof(field(line, "sample_rate").c_str());
    const double center = std::atof(field(line, "center").c_str());
    const std::string signal_kind = field(line, "signal");
    const std::string label = field(line, "label");
    log("open: " + behaviour + " at " + std::to_string(static_cast<long long>(rate)));

    if (behaviour == "no-ready") {
        wait_for_eof();
        return 0;
    }
    if (behaviour == "invalid") {
        event(R"({"type":"error","code":"invalid","message":"that gain is not possible here","fatal":true})");
        return 6;
    }
    if (behaviour == "no-device") {
        event(R"({"type":"error","code":"no-device","message":"no fake radio is plugged in","fatal":true})");
        return 3;
    }
    if (behaviour == "early") {
        const char bytes[64] = {};
        write_all(1, bytes, sizeof(bytes));
        wait_for_eof();
        return 0;
    }
    if (behaviour == "huge") {
        event("{\"type\":\"note\",\"text\":\"" + std::string(70000, 'x') + "\"}");
        wait_for_eof();
        return 0;
    }
    if (behaviour == "ignore-stop") {
        // Deaf to everything short of SIGKILL: the pipes closing, SIGTERM.
        signal(SIGTERM, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
    }

    double ready_rate = rate;
    double ready_center = center;
    if (behaviour == "bad-rate") ready_rate = rate * 1.01;
    if (behaviour == "bad-center") ready_center = center + 50;
    // Where an RTL-SDR's mixer may leave it, a few hertz beside.
    if (behaviour == "near-center") ready_center = center + 3.25;
    char ready[512];
    std::snprintf(ready, sizeof(ready),
                  R"({"type":"ready","format":"u8","signal":"%s","sample_rate":%.3f,"center":%.3f,)"
                  R"("device":{"name":"Fake radio","serial":"F00D"},"settings":{"label":"%s","gain":10}})",
                  signal_kind.c_str(), ready_rate, ready_center, label.c_str());
    event(ready);
    if (behaviour == "fds") log("fds: " + open_descriptors());
    if (behaviour == "applied-flood") {
        // Every event names settings nobody declared.
        for (int i = 0; i < 400; i++) {
            event("{\"type\":\"applied\",\"id\":0,\"settings\":{\"invented_" + std::to_string(i) + "\":1}}");
        }
    }
    if (behaviour == "helper" || behaviour == "helper-exit") {
        // A child of its own, as a module with a USB reader process might
        // have. It must not outlive the module.
        const pid_t helper = fork();
        if (helper == 0) {
            for (;;) sleep_ms(1000);
        }
        log("helper: " + std::to_string(static_cast<long long>(helper)));
    }
    if (behaviour == "flood") {
        for (int i = 0; i < 5000; i++) log("flood line " + std::to_string(i));
    }

    // A tone a quarter of the way up the band, as unsigned 8-bit IQ, paced
    // at the sample rate in 10 ms pieces.
    const bool iq = signal_kind != "real";
    const size_t per_chunk = static_cast<size_t>(std::max(1.0, rate / 100));
    std::vector<char> chunk(per_chunk * (iq ? 2 : 1));
    uint64_t sent = 0;
    const auto started = std::chrono::steady_clock::now();
    auto last_stats = started;
    bool stopping = false;
    while (!stopping) {
        for (size_t i = 0; i < per_chunk; i++) {
            const double phase = 2 * M_PI * 0.25 * static_cast<double>(sent + i);
            if (iq) {
                chunk[2 * i] = static_cast<char>(static_cast<int>(127.5 + 100 * std::cos(phase)));
                chunk[2 * i + 1] = static_cast<char>(static_cast<int>(127.5 + 100 * std::sin(phase)));
            } else {
                chunk[i] = static_cast<char>(static_cast<int>(127.5 + 100 * std::cos(phase)));
            }
        }
        if (behaviour == "stall" && sent >= 20000) {
            wait_for_eof();
            return 0;
        }
        if (!write_all(1, chunk.data(), chunk.size())) {
            if (behaviour == "ignore-stop") {
                while (true) sleep_ms(1000);
            }
            return 0;
        }
        sent += per_chunk;
        if (behaviour == "crash" && sent >= 20000) std::abort();
        if ((behaviour == "exit0" || behaviour == "helper-exit") && sent >= 20000) return 0;
        while (stdin_ready()) {
            if (!read_line(buffer, line)) {
                if (behaviour == "ignore-stop") break;
                return 0;
            }
            const std::string type = field(line, "type");
            if (type == "stop" && behaviour != "ignore-stop") stopping = true;
            if (type == "set") {
                const std::string id = field(line, "id");
                const std::string gain = field(line, "gain");
                if (gain.empty()) {
                    event("{\"type\":\"error\",\"id\":" + id +
                          ",\"code\":\"invalid\",\"message\":\"only gain changes live\",\"fatal\":false}");
                } else {
                    event("{\"type\":\"applied\",\"id\":" + id + ",\"settings\":{\"gain\":" + gain + "}}");
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stats >= std::chrono::seconds(1)) {
            // As the RTL-SDR module reports them: a tenth of the samples
            // clipped, at a gain it chose itself.
            const std::string clipping = behaviour == "clipping" ? ",\"clipping\":0.1,\"gain\":22.9" : "";
            event("{\"type\":\"stats\",\"samples\":" + std::to_string(sent) + ",\"dropped\":0" + clipping + "}");
            last_stats = now;
        }
        const auto due = started + std::chrono::microseconds(static_cast<long long>(sent * 1e6 / rate));
        std::this_thread::sleep_until(due);
    }
    return 0;
}
