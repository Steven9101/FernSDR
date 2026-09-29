// A model of one listener's stream over a link, driven by the real
// StreamBudget. It shows how the budget copes with stalls and slow links
// without a browser or a network, over far more link shapes and stall phases
// than a browser run can cover. tools/link-lab.sh checks the same kinds of link with the real
// page over the kernel's TCP.
//
//   make -C server link-model
//
// Every 1024/48000 s the budget sees the backlog the server would report:
// the application queue, plus the kernel's unsent bytes as sampled every
// 100 ms. The stream then produces audio at 62% of the budget's ceiling,
// which is what NAC3 uses on speech at 48 kbit/s, 100 bytes at a time, and
// 150-byte waterfall lines at up to 12.5 a second, about 45 kbit/s in all.
// Frames wait in the application queue until the kernel's unsent bytes drop
// under 4 kB (TCP_NOTSENT_LOWAT) and expire there after 250 ms, as in the
// server. The link drains the kernel's bytes at its capacity. A stall stops it
// for its length plus 0.3 s, standing in for TCP's retransmission timer and
// slow start; nothing else of TCP is modelled.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../src/core/stream_budget.h"

namespace {

constexpr double kBlockSeconds = 1024.0 / 48000.0;
constexpr int kRequestedBps = 48000;
constexpr double kAudioUse = 0.62;
constexpr double kLineBytes = 150;
constexpr double kLinesPerSecond = 12.5;
constexpr double kSeconds = 600;

struct Frame {
    double queued_at;
    double bytes;
    bool audio;
};

struct Outcome {
    double waterfall = 0;       // share of the requested lines delivered to the queue
    double audio = 0;           // mean audio ceiling as a share of the request
    int queues = 0;             // times the budget saw congestion begin
    int dropped_seconds = 0;    // seconds in which audio expired unsent
};

using Capacity = std::function<double(double)>;

Outcome run(const Capacity& capacity_bps) {
    fernsdr::StreamBudget budget;
    std::deque<Frame> queue;
    double kernel = 0, sampled_kernel = 0, sampled_at = -1;
    double audio_credit = 0, line_credit = 0, measured = 0, window_bits = 0, window = 0;
    double lines = 0, ceiling_sum = 0, last_drop = -10;
    bool congested = false;
    Outcome out;
    for (double t = 0; t < kSeconds; t += kBlockSeconds) {
        if (t - sampled_at >= 0.1) {
            sampled_kernel = kernel;
            sampled_at = t;
        }
        bool expired = false;
        while (!queue.empty() && t - queue.front().queued_at >= 0.25) {
            if (queue.front().audio && t - last_drop >= 1) {
                out.dropped_seconds++;
                last_drop = t;
            }
            queue.pop_front();
            expired = true;
        }
        double queued = sampled_kernel;
        for (const Frame& frame : queue) queued += frame.bytes;
        budget.update(static_cast<size_t>(queued), kBlockSeconds, kRequestedBps, 30, expired,
                      static_cast<int>(measured));
        const double scale = budget.waterfall_scale(kRequestedBps);
        if (scale == 0 && !congested) out.queues++;
        congested = scale == 0;

        const double audio_bps = budget.audio_bitrate(kRequestedBps) * kAudioUse;
        audio_credit += audio_bps * kBlockSeconds / 8;
        window_bits += audio_bps * kBlockSeconds;
        window += kBlockSeconds;
        if (window >= 1) {
            measured = window_bits / window;
            window_bits = window = 0;
        }
        for (; audio_credit >= 100; audio_credit -= 100) queue.push_back({t, 100, true});
        line_credit += kBlockSeconds * kLinesPerSecond * scale;
        if (line_credit >= 1) {
            queue.push_back({t, kLineBytes, false});
            line_credit = std::min(line_credit - 1, 1.0);
            lines++;
        }
        ceiling_sum += budget.audio_bitrate(kRequestedBps) * kBlockSeconds;

        while (!queue.empty() && kernel < 4096) {
            kernel += queue.front().bytes;
            queue.pop_front();
        }
        kernel = std::max(0.0, kernel - capacity_bps(t) / 8 * kBlockSeconds);
    }
    out.waterfall = lines / (kLinesPerSecond * kSeconds);
    out.audio = ceiling_sum / (kRequestedBps * kSeconds);
    return out;
}

// Stalls of `length` seconds every `every` seconds, the first at `first`.
Capacity stalls(double rate, double first, double every, double length) {
    return [=](double t) {
        if (t < first) return rate;
        return std::fmod(t - first, every) < length + 0.3 ? 0.0 : rate;
    };
}

// Stalls at random, `gap` seconds apart on average, 0.3 to 2 s long.
Capacity random_stalls(double rate, double gap, unsigned seed) {
    std::mt19937 generator(seed);
    std::exponential_distribution<double> next(1.0 / gap);
    std::uniform_real_distribution<double> length(0.3, 2.0);
    std::vector<std::pair<double, double>> spans;
    for (double t = next(generator); t < kSeconds; t += next(generator)) {
        const double l = length(generator);
        spans.emplace_back(t, t + l + 0.3);
        t += l;
    }
    return [=](double t) {
        for (const auto& span : spans) {
            if (t >= span.first && t < span.second) return 0.0;
        }
        return rate;
    };
}

void row(const std::string& name, const std::vector<Outcome>& runs) {
    double worst = 1, waterfall = 0, audio = 0, queues = 0, dropped = 0;
    for (const Outcome& o : runs) {
        worst = std::min(worst, o.waterfall);
        waterfall += o.waterfall;
        audio += o.audio;
        queues += o.queues;
        dropped += o.dropped_seconds;
    }
    const double n = static_cast<double>(runs.size());
    std::printf("%-38s %6.2f %6.2f %6.2f %7.0f %7.0f\n", name.c_str(), worst, waterfall / n, audio / n,
                queues / n, dropped / n);
}

}  // namespace

int main() {
    const double fast = 2e6;
    std::printf("Ten minutes per link. Waterfall: share of lines, worst and mean over the runs.\n"
                "Audio: mean ceiling as a share of 48 kbit/s. Queues: congestion onsets.\n"
                "Dropped: seconds in which audio expired. Stall rows run the first stall at\n"
                "1, 3, 5, 8, 12 and 17 s; random rows run 20 seeds.\n\n");
    std::printf("%-38s %6s %6s %6s %7s %7s\n", "link", "worst", "mean", "audio", "queues", "dropped");
    struct Pattern {
        const char* name;
        double rate, every, length;
    };
    const Pattern patterns[] = {
        {"fast, 1 s stall every 10 s", fast, 10, 1.0},
        {"fast, 1 s stall every 20 s", fast, 20, 1.0},
        {"fast, 1 s stall every 30 s", fast, 30, 1.0},
        {"fast, 0.5 s stall every 60 s", fast, 60, 0.5},
        {"fast, 2 s stall every 90 s", fast, 90, 2.0},
        {"40 kbit/s, 1 s stall every 30 s", 40e3, 30, 1.0},
    };
    for (const Pattern& pattern : patterns) {
        std::vector<Outcome> runs;
        for (const double first : {1.0, 3.0, 5.0, 8.0, 12.0, 17.0}) {
            runs.push_back(run(stalls(pattern.rate, first, pattern.every, pattern.length)));
        }
        row(pattern.name, runs);
    }
    for (const double gap : {15.0, 30.0, 60.0, 120.0}) {
        std::vector<Outcome> runs;
        for (unsigned seed = 1; seed <= 20; seed++) runs.push_back(run(random_stalls(fast, gap, seed)));
        row("fast, random stalls every " + std::to_string(static_cast<int>(gap)) + " s", runs);
    }
    for (const double rate : {48e3, 45e3, 43e3, 40e3, 30e3, 24e3, 16e3}) {
        row(std::to_string(static_cast<int>(rate / 1000)) + " kbit/s", {run([=](double) { return rate; })});
    }
    row("fluctuating 30 to 60 kbit/s", {run([](double t) { return 45e3 + 15e3 * std::sin(t * 0.7) * std::cos(t * 0.13); })});
    row("fluctuating 35 to 75 kbit/s, slowly", {run([](double t) { return 55e3 + 20e3 * std::sin(t * 0.21); })});
    row("fast, 20 kbit/s from 60 to 240 s", {run([=](double t) { return t > 60 && t < 240 ? 20e3 : fast; })});
    row("fast, 40 kbit/s from 60 to 240 s", {run([=](double t) { return t > 60 && t < 240 ? 40e3 : fast; })});
    return 0;
}
