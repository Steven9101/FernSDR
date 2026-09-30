// Append latency on the band thread, with warm files and deterministic spectra.
#include "../src/core/archive.h"
#include "../src/util/log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

int64_t nanos(clockid_t clock) {
    timespec time{};
    clock_gettime(clock, &time);
    return time.tv_sec * 1000000000ll + time.tv_nsec;
}

bool measure(size_t source_bins, size_t archive_bins) {
    const std::string path = "/tmp/fernsdr-archive-bench-" + std::to_string(getpid()) + ".wfa";
    std::remove(path.c_str());
    fernsdr::WaterfallArchive archive;
    std::string error;
    if (!archive.open(path, archive_bins, 1.0, 1, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::vector<float> spectrum(source_bins);
    for (size_t i = 0; i < source_bins; ++i) spectrum[i] = -130.0f + static_cast<float>((i * 37) % 110);
    constexpr int64_t epoch = 1'700'000'000'000;
    constexpr size_t warmup = 5000;
    constexpr size_t repetitions = 20000;
    for (size_t i = 0; i < warmup; ++i) archive.append(spectrum.data(), spectrum.size(), epoch + i * 1000);
    std::vector<int64_t> latency(repetitions);
    const int64_t cpu_start = nanos(CLOCK_THREAD_CPUTIME_ID);
    const int64_t wall_start = nanos(CLOCK_MONOTONIC);
    for (size_t i = 0; i < repetitions; ++i) {
        const int64_t start = nanos(CLOCK_MONOTONIC);
        archive.append(spectrum.data(), spectrum.size(), epoch + (warmup + i) * 1000);
        latency[i] = nanos(CLOCK_MONOTONIC) - start;
    }
    const int64_t wall = nanos(CLOCK_MONOTONIC) - wall_start;
    const int64_t cpu = nanos(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    std::sort(latency.begin(), latency.end());
    std::printf("append,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f\n", source_bins, archive_bins, repetitions,
                cpu / (1000.0 * repetitions), wall / (1000.0 * repetitions),
                latency[repetitions / 2] / 1000.0, latency[repetitions * 95 / 100] / 1000.0,
                latency[repetitions * 99 / 100] / 1000.0);

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    const int64_t from = epoch + (warmup + repetitions - 1024) * 1000;
    const int64_t to = epoch + (warmup + repetitions - 1) * 1000;
    for (size_t i = 0; i < 10; ++i) {
        if (!archive.read(from, to, rows, times)) return false;
    }
    const size_t stride = (1024 - 1) / (1024 * 1024 / archive_bins) + 1;
    const size_t expected_rows = (1024 - 1) / stride + 1;
    if (archive.newest_ms() != to || times.size() != expected_rows || rows.size() != expected_rows * archive_bins) {
        std::fputs("archive did not record the benchmark window\n", stderr);
        return false;
    }
    for (size_t i = 0; i < times.size(); ++i) {
        if (times[i] != from + static_cast<int64_t>(i * stride * 1000)) return false;
    }
    constexpr size_t reads = 300;
    latency.resize(reads);
    const int64_t read_cpu_start = nanos(CLOCK_THREAD_CPUTIME_ID);
    const int64_t read_wall_start = nanos(CLOCK_MONOTONIC);
    for (size_t i = 0; i < reads; ++i) {
        const int64_t start = nanos(CLOCK_MONOTONIC);
        if (!archive.read(from, to, rows, times)) return false;
        latency[i] = nanos(CLOCK_MONOTONIC) - start;
    }
    const int64_t read_wall = nanos(CLOCK_MONOTONIC) - read_wall_start;
    const int64_t read_cpu = nanos(CLOCK_THREAD_CPUTIME_ID) - read_cpu_start;
    std::sort(latency.begin(), latency.end());
    std::printf("read,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f\n", source_bins, archive_bins, reads,
                read_cpu / (1000.0 * reads), read_wall / (1000.0 * reads),
                latency[reads / 2] / 1000.0, latency[reads * 95 / 100] / 1000.0,
                latency[reads * 99 / 100] / 1000.0);
    archive.close();
    std::remove(path.c_str());
    return true;
}

}  // namespace

int main() {
    fernsdr::set_log_level(fernsdr::LogLevel::None);
    std::puts("operation,source_bins,archive_bins,repetitions,cpu_us,wall_us,p50_us,p95_us,p99_us");
    return measure(1024, 1024) && measure(65536, 1024) && measure(65536, 8192) ? 0 : 1;
}
