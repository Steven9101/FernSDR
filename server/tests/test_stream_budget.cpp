#include "../src/core/stream_budget.h"
#include "test_util.h"

TEST_CASE(stream_budget_observes_expiry_even_when_trimming_keeps_the_queue_short) {
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 20; i++) budget.update(400, 0.05, 48000, 0, i % 4 == 0);
    CHECK(budget.audio_bitrate(48000) < 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 0, 0);
    for (int i = 0; i < 10; i++) budget.update(0, 0.05, 48000);
    CHECK(budget.waterfall_scale(48000) > 0);
    for (int i = 0; i < 1800; i++) budget.update(0, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 1, 0);
}

TEST_CASE(stream_budget_observes_expiry_in_a_block_longer_than_the_hold) {
    fernsdr::StreamBudget budget;
    budget.update(0, 0.4, 48000, 0, true);
    CHECK(budget.audio_bitrate(48000) < 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 0, 0);
    budget.update(0, 0.4, 48000);
    CHECK(budget.waterfall_scale(48000) > 0);
}

TEST_CASE(stream_budget_reserves_control_traffic_and_releases_old_peaks) {
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 40; i++) budget.update(0, 0.25, 48000, 500);
    CHECK_NEAR(budget.control_bitrate(), 16000, 0);
    budget.update(0, 0.25, 48000, 70);
    CHECK_NEAR(budget.control_bitrate(), 16000, 0);
    for (int i = 0; i < 3; i++) budget.update(0, 0.25, 48000, 70);
    CHECK_NEAR(budget.control_bitrate(), 2240, 0);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
}

TEST_CASE(stream_budget_does_not_reserve_a_one_second_burst_of_replies) {
    fernsdr::StreamBudget budget;
    // Meters: 70 bytes four times a second.
    for (int i = 0; i < 16; i++) budget.update(0, 0.25, 48000, 70);
    CHECK_NEAR(budget.control_bitrate(), 2240, 0);
    // A second of state replies while a filter is dragged: 10 kB.
    for (int i = 0; i < 4; i++) budget.update(0, 0.25, 48000, 2500);
    CHECK_NEAR(budget.control_bitrate(), 2240, 0);
    for (int i = 0; i < 8; i++) budget.update(0, 0.25, 48000, 70);
    CHECK_NEAR(budget.control_bitrate(), 2240, 0);
    // Traffic that stays up is reserved once it has lasted.
    for (int i = 0; i < 12; i++) budget.update(0, 0.25, 48000, 2500);
    CHECK_NEAR(budget.control_bitrate(), 80000, 0);
}

TEST_CASE(stream_budget_protects_audio_before_reducing_quality) {
    fernsdr::StreamBudget budget;
    budget.update(4096, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 0, 0);
    for (int i = 0; i < 10; i++) budget.update(4096, 0.1, 48000);
    CHECK(budget.audio_bitrate(48000) < 48000);
    for (int i = 0; i < 100; i++) budget.update(65536, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), 8000);
}

TEST_CASE(stream_budget_recovers_after_the_link_clears) {
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 10; i++) budget.update(4096, 0.1, 48000);
    const int reduced = budget.audio_bitrate(48000);
    budget.update(0, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), reduced);
    CHECK(budget.waterfall_scale(48000) > 0);
    for (int i = 0; i < 1800; i++) budget.update(0, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 1, 0);
}

TEST_CASE(stream_budget_does_not_restore_full_waterfall_with_the_audio_bitrate) {
    fernsdr::StreamBudget budget;
    // A link that cannot carry the start: two returns in a row meet a queue,
    // so there is nothing to return to and recovery is by probes.
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000);
        for (int i = 0; i < 11; i++) budget.update(0, 0.1, 48000);
    }
    for (int i = 0; i < 900 && budget.audio_bitrate(48000) < 48000; i++) budget.update(0, 0.1, 48000);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK(budget.waterfall_scale(48000) <= 0.25);
    for (int i = 0; i < 65; i++) budget.update(0, 0.1, 48000);
    CHECK(budget.waterfall_scale(48000) > 0.25);
    CHECK(budget.waterfall_scale(48000) < 1);
}

namespace {

// One second without delivery: the queue fills at once and frames expire.
void stall(fernsdr::StreamBudget& budget) {
    for (int i = 0; i < 10; i++) budget.update(6000, 0.1, 48000, 0, i >= 3, 30000);
}

void clear(fernsdr::StreamBudget& budget, int ticks) {
    for (int i = 0; i < ticks; i++) budget.update(0, 0.1, 48000, 0, false, 30000);
}

// A link a little too slow for what it is asked to carry: the queue takes
// two seconds to pass the congestion threshold, and a little longer to drain.
void creep(fernsdr::StreamBudget& budget) {
    for (size_t queued = 600; queued <= 2600; queued += 100) budget.update(queued, 0.1, 48000, 0, false, 30000);
}

// Tenths of a second until the budget is back at the whole stream.
int ticks_to_full(fernsdr::StreamBudget& budget, int limit = 600) {
    int ticks = 0;
    while ((budget.audio_bitrate(48000) < 48000 || budget.waterfall_scale(48000) < 1) && ticks < limit) {
        clear(budget, 1);
        ticks++;
    }
    return ticks;
}

// Tenths of a second until the waterfall is above `level`.
int ticks_above(fernsdr::StreamBudget& budget, double level, int limit = 600) {
    int ticks = 0;
    while (budget.waterfall_scale(48000) <= level && ticks < limit) {
        clear(budget, 1);
        ticks++;
    }
    return ticks;
}

}  // namespace

TEST_CASE(stream_budget_backs_off_after_a_failed_recovery_probe) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    // The queue creeps up at the whole stream: the link is a little too slow
    // for the waterfall, which comes back at the half it can carry.
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
    // Its first probe comes after the shortest interval.
    const int first = ticks_above(budget, 0.5);
    CHECK(first >= 29 && first <= 31);
    // The link refuses it, so the next probe waits twice as long.
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    const int second = ticks_above(budget, 0.5);
    CHECK(second >= 2 * first - 2 && second <= 2 * first + 2);
}

// A stall on a link that has carried the whole stream: Wi-Fi retrying, a
// phone changing cells. It says nothing about the link's speed, so neither
// the waterfall nor the audio may stay reduced, however often it happens.
TEST_CASE(stream_budget_returns_at_once_after_each_stall_on_a_fast_link) {
    fernsdr::StreamBudget budget;
    for (int round = 0; round < 8; round++) {
        clear(budget, 190);
        CHECK_EQ(budget.audio_bitrate(48000), 48000);
        CHECK_NEAR(budget.waterfall_scale(48000), 1, 0);
        stall(budget);
        CHECK(budget.audio_bitrate(48000) < 48000);
        CHECK_NEAR(budget.waterfall_scale(48000), 0, 0);
        // The expiry holds the pressure for a moment, then one clear second.
        CHECK(ticks_to_full(budget) <= 16);
    }
}

// The same stalls starting in the first second of the session, before
// anything has been proven, and every ten seconds, so that no stretch is ever
// quiet for long enough to prove anything.
TEST_CASE(stream_budget_returns_at_once_from_stalls_before_anything_is_proven) {
    fernsdr::StreamBudget budget;
    clear(budget, 10);
    for (int round = 0; round < 12; round++) {
        stall(budget);
        const int back = ticks_to_full(budget);
        CHECK(back <= 16);
        clear(budget, 90 - back);
    }
}

TEST_CASE(stream_budget_returns_only_to_a_level_that_held_ten_quiet_seconds) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    // Two probes lift the waterfall to 0.75 within seven quiet seconds,
    // shorter than a good spell on a link whose speed swings every few
    // seconds, and the stall that follows returns it to the level before.
    CHECK(ticks_above(budget, 0.625) <= 61);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.75, 1e-9);
    stall(budget);
    clear(budget, 16);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
}

TEST_CASE(stream_budget_keeps_what_a_climb_proved_when_a_stall_interrupts_it) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    // Three probes and half of a fourth's window: ten quiet seconds have
    // passed. The stall meets the open fourth probe and so counts against
    // it, which sends the budget back to the level before that probe.
    clear(budget, 105);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.875, 1e-9);
    stall(budget);
    clear(budget, 16);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.75, 1e-9);
}

TEST_CASE(stream_budget_does_not_return_after_a_queue_that_filled_slowly) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    // Nothing changed on this side and the queue took two seconds: the link
    // itself has become slower, and the whole waterfall would fill it again.
    creep(budget);
    clear(budget, 25);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
}

TEST_CASE(stream_budget_stops_returning_to_a_level_the_link_no_longer_carries) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    CHECK(ticks_to_full(budget) <= 16);
    // A sudden queue right after the return: most likely the same bad moment
    // stalling once more, so the budget tries the level again.
    for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000, 0, false, 30000);
    CHECK(ticks_to_full(budget) <= 12);
    // A second one in a row: the link has become slower.
    for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000, 0, false, 30000);
    clear(budget, 30);
    CHECK(budget.audio_bitrate(48000) < 48000);
    CHECK(budget.waterfall_scale(48000) <= 0.25);
}

TEST_CASE(stream_budget_does_not_forgive_a_return_that_fills_the_queue_slowly) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    CHECK(ticks_to_full(budget) <= 16);
    // A queue that creeps up after the return is not the same stall again:
    // the link cannot carry the level any more.
    creep(budget);
    clear(budget, 30);
    CHECK(budget.audio_bitrate(48000) < 48000);
    CHECK(budget.waterfall_scale(48000) <= 0.25);
}

TEST_CASE(stream_budget_brings_the_waterfall_back_only_with_the_whole_audio) {
    fernsdr::StreamBudget budget;
    // Two returns refused in a row leave a level with a reduced audio
    // ceiling to return to.
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000);
        clear(budget, 11);
    }
    const double waterfall = budget.waterfall_scale(48000);
    CHECK(budget.audio_bitrate(48000) < 48000);
    // A stall halves the waterfall. Returning afterwards restores the audio
    // ceiling it can, but not the waterfall while the audio is short: on a
    // link too slow for the whole audio, what the waterfall does not take
    // goes to the audio.
    stall(budget);
    clear(budget, 16);
    CHECK(budget.audio_bitrate(48000) < 48000);
    CHECK(budget.waterfall_scale(48000) <= waterfall / 2 + 1e-9);
}

TEST_CASE(stream_budget_remembers_the_link_across_a_change_of_mode) {
    fernsdr::StreamBudget budget;
    // CW asks for less audio than USB.
    for (int i = 0; i < 150; i++) budget.update(0, 0.1, 27000, 0, false, 17000);
    for (int i = 0; i < 10; i++) budget.update(6000, 0.1, 27000, 0, i >= 3, 17000);
    for (int i = 0; i < 80; i++) budget.update(0, 0.1, 27000, 0, false, 17000);
    CHECK_EQ(budget.audio_bitrate(27000), 27000);
    // Tuning to USB returns the audio to what the link carried before the
    // stall, and the quiet seconds that follow must not make CW's bitrate
    // the level to remember.
    for (int i = 0; i < 25; i++) budget.update(0, 0.1, 48000, 0, false, 30000);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    stall(budget);
    CHECK(ticks_to_full(budget) <= 16);
}

TEST_CASE(stream_budget_forgives_a_return_again_once_a_step_has_held) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    CHECK(ticks_to_full(budget) <= 16);
    // The return meets a sudden queue and is let off. Then a queue creeps up
    // and clears at once, too briefly to cut the audio, so the level the
    // budget returns to drops and it climbs back by probes alone, with no
    // return in between that would clear the strike by itself.
    for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000, 0, false, 30000);
    clear(budget, 5);
    for (size_t queued = 600; queued <= 2100; queued += 100) budget.update(queued, 0.1, 48000, 0, false, 30000);
    CHECK(ticks_to_full(budget) < 400);
    clear(budget, 120);
    // Much later, a stall and one more sudden queue after the return: the
    // steps that held in between mean this is not the second in a row.
    stall(budget);
    CHECK(ticks_to_full(budget) <= 16);
    for (int i = 0; i < 5; i++) budget.update(4096, 0.1, 48000, 0, false, 30000);
    CHECK(ticks_to_full(budget) <= 12);
}

TEST_CASE(stream_budget_blames_a_step_only_for_a_queue_in_its_first_seconds) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    CHECK(ticks_above(budget, 0.5) <= 31);
    creep(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    // Probes now wait six seconds. One lifts the waterfall to 0.625, holds
    // its three seconds, and a stall comes a second and a half later: that
    // is not the link refusing the step, so the budget returns to it and the
    // wait before the next probe does not double again.
    const int wait = ticks_above(budget, 0.5);
    CHECK(wait >= 59 && wait <= 62);
    clear(budget, 45);
    stall(budget);
    CHECK(ticks_above(budget, 0.5) < 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.625, 1e-9);
    const int next = ticks_above(budget, 0.625);
    CHECK(next >= wait - 2 && next <= wait + 2);
}

// A link a little too slow for the stream: the kernel hands the bytes on and
// the queue builds in the network, where only the round trip shows it.
TEST_CASE(stream_budget_gives_the_waterfall_up_to_a_queue_in_the_network) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int delay = 0; delay <= 300; delay += 50) budget.update(0, 0.1, 48000, 0, false, 30000, delay);
    CHECK_NEAR(budget.waterfall_scale(48000), 0, 0);
    // However long it takes to drain, it costs the audio nothing.
    for (int i = 0; i < 30; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 300);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    // Drained, the waterfall comes back, but not to the level that built it.
    clear(budget, 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
}

TEST_CASE(stream_budget_does_not_probe_into_a_queue_in_the_network) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int i = 0; i < 5; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 200);
    clear(budget, 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
    // A round trip 80 ms up is no queue yet, but no room to probe into
    // either: the waterfall holds where it is.
    for (int i = 0; i < 100; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
    CHECK(ticks_above(budget, 0.5) <= 31);
}

// A round trip 50 to 150 ms over its floor is no queue, but no room for the
// waterfall to grow into either. The audio does not wait for it: a stall's cut
// is undone once this socket has been clear for a second, and the waterfall
// follows once the round trip has settled.
TEST_CASE(stream_budget_returns_the_audio_while_the_round_trip_is_a_little_long) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    CHECK(budget.audio_bitrate(48000) < 48000);
    for (int i = 0; i < 15; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    for (int i = 0; i < 100; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
    CHECK(budget.waterfall_scale(48000) < 1);
    CHECK(ticks_to_full(budget) <= 11);
}

// A stall fills this socket at the whole stream however long the round trip
// has been: time spent with the round trip a little long is not time the
// socket spent filling.
TEST_CASE(stream_budget_takes_a_stall_for_a_stall_while_the_round_trip_is_a_little_long) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int i = 0; i < 30; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
    stall(budget);
    CHECK(ticks_to_full(budget) <= 14);
}

// A queue in the network is the waterfall's to answer. Coming just after a
// stall that cut the audio, it lowers the waterfall's level, and must not keep
// the audio's cut.
TEST_CASE(stream_budget_does_not_keep_the_audio_cut_for_a_queue_in_the_network) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    clear(budget, 5);
    // Nothing returns into the queue while it lasts, the audio included...
    for (int i = 0; i < 20; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 200);
    CHECK(budget.audio_bitrate(48000) < 48000);
    // ...and once it has drained, the audio comes back whole.
    clear(budget, 11);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.125, 1e-9);
}

// A return that fills a queue in the network has shown the link carrying the
// level before it and no more, the audio's included. On a link too slow for the
// whole audio, the network fills before this socket does; a budget that did not
// learn from that returned to the whole audio after every cut, and the listener
// heard each return as a dropout.
TEST_CASE(stream_budget_learns_from_a_return_that_fills_the_network) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    stall(budget);
    clear(budget, 5);
    const int cut = budget.audio_bitrate(48000);
    clear(budget, 10);
    CHECK_EQ(budget.audio_bitrate(48000), 48000);
    for (int i = 0; i < 5; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 200);
    clear(budget, 50);
    stall(budget);
    clear(budget, 15);
    CHECK_EQ(budget.audio_bitrate(48000), cut);
}

// A return has held once this socket has stayed clear for a few seconds,
// whatever the round trip does, so stalls a minute apart on a cell whose round
// trip stays a little long are separate stalls, not one return failing again.
TEST_CASE(stream_budget_keeps_returning_after_stalls_while_the_round_trip_is_a_little_long) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int round = 0; round < 3; round++) {
        stall(budget);
        for (int i = 0; i < 100; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
        CHECK_EQ(budget.audio_bitrate(48000), 48000);
    }
}

// A probe builds its queue in the network slowly: a round trip that climbs
// after it and passes the threshold seconds later is still the link refusing
// it, so the waterfall goes back to the level before it.
TEST_CASE(stream_budget_blames_a_probe_for_a_queue_it_builds_slowly_in_the_network) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int i = 0; i < 5; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 200);
    clear(budget, 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
    CHECK(ticks_above(budget, 0.5) < 15);
    for (int delay = 0; delay <= 180; delay += 3) budget.update(0, 0.1, 48000, 0, false, 30000, delay);
    clear(budget, 20);
    CHECK_NEAR(budget.waterfall_scale(48000), 0.5, 1e-9);
}

// A probe the socket has carried for seconds held as far as the socket goes,
// even while the round trip stayed a little long; a stall a minute later is a
// stall, and the next probe comes after the shortest interval.
TEST_CASE(stream_budget_does_not_blame_an_old_probe_for_a_stall) {
    fernsdr::StreamBudget budget;
    clear(budget, 150);
    for (int i = 0; i < 5; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 200);
    clear(budget, 20);
    CHECK(ticks_above(budget, 0.5) < 15);
    for (int i = 0; i < 600; i++) budget.update(0, 0.1, 48000, 0, false, 30000, 80);
    stall(budget);
    CHECK(ticks_above(budget, 0.4) < 20);
    CHECK(ticks_above(budget, 0.5) <= 31);
}

TEST_CASE(stream_budget_cuts_audio_from_what_it_actually_uses) {
    // A variable-rate codec spending 20 kbit/s under a 48 kbit/s ceiling:
    // the first cut has to bite, not trim ceiling nobody is using.
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 4; i++) budget.update(4096, 0.1, 48000, 0, false, 20000);
    CHECK(budget.audio_bitrate(48000) < 20000);
    // Without a measurement it falls back to the requested rate, as before.
    fernsdr::StreamBudget unmeasured;
    for (int i = 0; i < 4; i++) unmeasured.update(4096, 0.1, 48000);
    CHECK_EQ(unmeasured.audio_bitrate(48000), 38400);
}

TEST_CASE(stream_budget_recovers_quickly_from_a_cut_taken_while_the_squelch_was_closed) {
    // A closed squelch measures a few kbit/s. A short burst of congestion
    // then must not pin the ceiling at the floor for minutes after the
    // squelch opens on a signal.
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 4; i++) budget.update(4096, 0.1, 48000, 0, false, 4500);
    CHECK(budget.audio_bitrate(48000) >= 16000);
    CHECK(budget.audio_bitrate(48000) < 48000);
    int tenths = 0;
    while (budget.audio_bitrate(48000) < 48000 && tenths < 3000) {
        budget.update(0, 0.1, 48000, 0, false, 40000);
        tenths++;
    }
    CHECK(tenths <= 350);
}

TEST_CASE(stream_budget_still_reaches_the_floor_under_sustained_pressure) {
    fernsdr::StreamBudget budget;
    for (int i = 0; i < 40; i++) budget.update(65536, 0.1, 48000, 0, false, 4500);
    CHECK_EQ(budget.audio_bitrate(48000), 8000);
}
