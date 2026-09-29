#include <algorithm>
#include <vector>

#include "../src/core/stream_budget.h"
#include "../src/net/queue_delay.h"
#include "test_util.h"

namespace {

struct Sample {
    size_t unsent;        // bytes waiting in the socket and the application
    uint32_t rtt_us;      // the kernel's smoothed round trip
    uint32_t unacked;     // packets in flight
    uint32_t ack_age_ms;  // since the last acknowledgement
};

// Recorded every 100 ms over the kernel's TCP, sending a stream like a USB
// listener's (100-byte audio frames every 21 ms, 150-byte waterfall lines at
// 12.5 a second) while a plug qdisc held every packet on the path for a while
// and then let them go in order, as Wi-Fi retrying or a phone changing cells
// does. Each starts two samples before the hold.
//
// One second on a 20 ms path.
const std::vector<Sample> kHoldOnAShortPath = {
    {0, 20100, 2, 2}, {0, 20100, 2, 16}, {0, 20100, 1, 12}, {0, 20100, 7, 110}, {0, 20100, 11, 210},
    {800, 20100, 11, 310}, {1450, 20100, 11, 410}, {450, 20100, 12, 510}, {1100, 20100, 12, 610},
    {1900, 20100, 12, 710}, {2450, 20100, 12, 810}, {100, 20100, 13, 910}, {750, 20100, 13, 1012},
    {0, 328800, 2, 22}, {0, 178400, 1, 15}, {0, 101300, 1, 8}, {0, 61700, 2, 2}, {0, 44500, 2, 16},
    {0, 32600, 1, 10}, {0, 26500, 1, 3}, {0, 23800, 1, 18}, {0, 22000, 2, 11}, {0, 21100, 1, 4},
    {0, 20600, 1, 19}, {0, 20400, 1, 12}, {0, 20200, 2, 6}, {0, 20200, 1, 20}, {0, 20100, 1, 14},
};

// 0.6 s on a 120 ms path: too short to fill the socket past the congestion
// threshold, so the budget sees nothing of it but the echo, 334 ms.
const std::vector<Sample> kShortHoldOnALongPath = {
    {0, 120100, 8, 8}, {0, 120100, 7, 1}, {0, 120100, 8, 17}, {0, 120100, 14, 116}, {300, 120100, 16, 216},
    {550, 120100, 17, 316}, {1200, 120100, 17, 416}, {1750, 120100, 17, 516}, {0, 189700, 14, 0},
    {0, 454200, 8, 35}, {0, 270000, 7, 5}, {0, 187400, 7, 20}, {0, 154600, 7, 13}, {0, 137800, 7, 6},
    {0, 130400, 8, 21}, {0, 125400, 7, 14}, {0, 122800, 8, 8}, {0, 121500, 7, 1}, {0, 120900, 8, 16},
    {0, 120500, 7, 9}, {0, 120300, 6, 2}, {0, 120200, 8, 17}, {0, 120100, 8, 10}, {0, 120100, 6, 4},
    {0, 120100, 7, 18}, {0, 120100, 8, 12}, {0, 120100, 7, 5}, {0, 120100, 7, 20},
};

// Three seconds on a 120 ms path, the longest echo recorded: up to 1.5 s.
const std::vector<Sample> kLongHoldOnALongPath = {
    {0, 120100, 8, 8}, {0, 120100, 7, 2}, {0, 120100, 8, 17}, {0, 120100, 14, 116}, {300, 120100, 16, 216},
    {550, 120100, 17, 316}, {1200, 120100, 17, 416}, {1750, 120100, 17, 516}, {2400, 120100, 14, 616},
    {3200, 120100, 14, 716}, {3750, 120100, 14, 816}, {4400, 120100, 14, 916}, {5050, 120100, 14, 1016},
    {5750, 120100, 14, 1116}, {6400, 120100, 14, 1216}, {450, 120100, 15, 1316}, {1250, 120100, 15, 1416},
    {1800, 120100, 15, 1516}, {2450, 120100, 15, 1616}, {3100, 120100, 15, 1716}, {3650, 120100, 15, 1816},
    {4450, 120100, 15, 1916}, {5100, 120100, 15, 2016}, {5650, 120100, 15, 2116}, {6300, 120100, 15, 2216},
    {7100, 120100, 15, 2316}, {7650, 120100, 15, 2416}, {8300, 120100, 15, 2516}, {100, 120100, 16, 2616},
    {800, 120100, 16, 2716}, {1450, 120100, 16, 2816}, {2100, 120100, 16, 2916}, {0, 489300, 16, 0},
    {0, 1598700, 6, 38}, {0, 986800, 8, 16}, {0, 460400, 7, 10}, {0, 272800, 6, 3}, {0, 209600, 8, 18},
    {0, 166000, 8, 11}, {0, 143600, 6, 4}, {0, 133900, 7, 19}, {0, 127100, 8, 12}, {0, 123700, 7, 6},
    {0, 122200, 7, 20}, {0, 121200, 7, 14}, {0, 120600, 7, 7}, {0, 120400, 6, 0},
};

const std::vector<const std::vector<Sample>*> kHolds = {&kHoldOnAShortPath, &kShortHoldOnALongPath,
                                                        &kLongHoldOnALongPath};

// Samples of a settled connection: acknowledgements every few milliseconds.
void settle(fernsdr::QueueDelay& delay, int64_t& now, uint32_t rtt_us, int count) {
    for (int i = 0; i < count; i++, now += 100) delay.update(now, rtt_us, 2, 10);
}

}  // namespace

TEST_CASE(queue_delay_reads_the_round_trip_over_its_floor) {
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20000, 50);
    CHECK_EQ(delay.ms(), 0);
    // A queue building in a modem while the acknowledgements keep coming.
    for (uint32_t rtt = 20000; rtt <= 220000; rtt += 20000, now += 100) delay.update(now, rtt, 12, 30);
    CHECK_EQ(delay.ms(), 200);
    // No round trip measured yet says nothing.
    delay.update(now, 0, 12, 30);
    CHECK_EQ(delay.ms(), 200);
}

TEST_CASE(queue_delay_does_not_read_a_hold_as_a_queue) {
    for (const auto* hold : kHolds) {
        fernsdr::QueueDelay delay;
        int64_t now = 0;
        settle(delay, now, hold->front().rtt_us, 50);
        int most = 0;
        for (const Sample& s : *hold) {
            delay.update(now, s.rtt_us, s.unacked, s.ack_age_ms);
            most = std::max(most, delay.ms());
            now += 100;
        }
        CHECK(most < 50);
        // And afterwards it reads the round trip again.
        settle(delay, now, hold->front().rtt_us + 100000, 5);
        CHECK_EQ(delay.ms(), 100);
    }
}

TEST_CASE(queue_delay_reads_a_queue_that_follows_a_hold) {
    // The handover lands on a cell that is slower for the stream: once the
    // echo has passed, the queue it builds is read.
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20100, 50);
    for (const Sample& s : kHoldOnAShortPath) {
        delay.update(now, s.rtt_us, s.unacked, s.ack_age_ms);
        now += 100;
    }
    settle(delay, now, 270100, 1);
    CHECK_EQ(delay.ms(), 250);
}

TEST_CASE(queue_delay_takes_a_fifth_of_a_second_without_acknowledgements_for_a_hold) {
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20000, 50);
    delay.update(now, 20000, 6, 150);
    now += 100;
    delay.update(now, 20000, 8, 250);
    now += 100;
    delay.update(now, 220000, 2, 5);
    CHECK_EQ(delay.ms(), 0);
}

TEST_CASE(queue_delay_does_not_take_an_idle_connection_for_a_hold) {
    // Nothing in flight, nothing to acknowledge: a stream that pauses is not
    // held, and what the round trip says afterwards is read.
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20000, 50);
    delay.update(now, 20000, 0, 3000);
    now += 100;
    delay.update(now, 220000, 0, 3100);
    CHECK_EQ(delay.ms(), 200);
}

TEST_CASE(queue_delay_keeps_a_queue_through_a_gap_in_the_acknowledgements) {
    // A queue grown until the kernel retransmits: the acknowledgements pause
    // and the round trip jumps. Neither may make it read as no queue.
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20000, 50);
    for (uint32_t rtt = 20000; rtt <= 420000; rtt += 40000, now += 100) delay.update(now, rtt, 20, 40);
    CHECK_EQ(delay.ms(), 400);
    for (uint32_t age = 100; age <= 600; age += 100, now += 100) {
        delay.update(now, 420000, 25, age);
        CHECK_EQ(delay.ms(), 400);
    }
    delay.update(now, 900000, 25, 5);
    CHECK_EQ(delay.ms(), 400);
    for (int i = 0; i < 15; i++, now += 100) delay.update(now, 900000, 25, 5);
    CHECK_EQ(delay.ms(), 880);
}

TEST_CASE(queue_delay_follows_a_route_that_changes) {
    fernsdr::QueueDelay delay;
    int64_t now = 0;
    settle(delay, now, 20000, 200);
    settle(delay, now, 60000, 1);
    CHECK_EQ(delay.ms(), 40);
    // A route 40 ms longer is a queue for a while, and the floor within half
    // a minute.
    settle(delay, now, 60000, 300);
    CHECK_EQ(delay.ms(), 0);
}

// The fault these guard against: each hold's echo, read as a queue in the
// network, lowered the waterfall's level and took twenty seconds to climb back.
TEST_CASE(stream_budget_returns_at_once_after_a_recorded_hold) {
    for (const auto* hold : kHolds) {
        fernsdr::QueueDelay delay;
        fernsdr::StreamBudget budget;
        int64_t now = 0;
        const auto step = [&](const Sample& s) {
            delay.update(now, s.rtt_us, s.unacked, s.ack_age_ms);
            budget.update(s.unsent, 0.1, 48000, 0, false, 30000, delay.ms());
            now += 100;
        };
        const Sample settled = {0, hold->front().rtt_us, 2, 10};
        for (int i = 0; i < 150; i++) step(settled);
        for (const Sample& s : *hold) step(s);
        // A second and a bit after the last sample with anything in the
        // socket.
        size_t last = 0;
        for (size_t i = 0; i < hold->size(); i++) {
            if ((*hold)[i].unsent > 0) last = i;
        }
        for (size_t after = hold->size() - 1 - last; after < 12; after++) step(settled);
        CHECK_EQ(budget.audio_bitrate(48000), 48000);
        CHECK_NEAR(budget.waterfall_scale(48000), 1, 0);
    }
}
