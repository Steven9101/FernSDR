#include "../src/dsp/rds.h"
#include "rds_signal.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <utility>
#include <vector>

using fernsdr::RdsDecoder;
using namespace rds_signal;

namespace {

// A multiplex at `rate` carrying `bits` as RDS (differential, biphase,
// 57 kHz BPSK at `level`), with a 1 kHz tone, the pilot and some noise.
std::vector<float> multiplex(const std::vector<int>& bits, double rate, double level, double noise, uint32_t seed,
                             double ppm = 20.0, double stereo = 0.0, double stereo_hz = 0.0) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> phase(0.0, 6.2831853);
    std::normal_distribution<double> gauss(0.0, noise);
    const double carrier_phase = phase(rng);
    const double chip_rate = 2375.0 * (1.0 + ppm * 1e-6);
    const double carrier = 57000.0 * (1.0 + ppm * 1e-6);
    const std::vector<int> chips = rds_chips(bits);
    const size_t samples = static_cast<size_t>(static_cast<double>(chips.size()) / chip_rate * rate);
    std::vector<float> out(samples);
    for (size_t n = 0; n < samples; n++) {
        const double t = static_cast<double>(n) / rate;
        const size_t chip = std::min(chips.size() - 1, static_cast<size_t>(t * chip_rate));
        out[n] = static_cast<float>(level * chips[chip] * std::cos(6.2831853 * carrier * t + carrier_phase) +
                                    0.4 * std::sin(6.2831853 * 1000.0 * t) + 0.08 * std::cos(6.2831853 * 19000.0 * t) +
                                    stereo * std::sin(6.2831853 * stereo_hz * t) * std::cos(6.2831853 * 38000.0 * t) +
                                    gauss(rng));
    }
    return out;
}

void feed(RdsDecoder& decoder, const std::vector<float>& mpx) {
    for (size_t at = 0; at < mpx.size(); at += 2048) decoder.process(mpx.data() + at, std::min<size_t>(2048, mpx.size() - at));
}

}  // namespace

TEST_CASE(rds_blocks_and_groups_give_the_station_name_and_text) {
    RdsDecoder decoder(256000.0);
    std::vector<int> bits;
    for (int r = 0; r < 3; r++) {
        auto g = groups(0xd3c3, 10, "FERN SDR", "Hello from FernSDR, gr\x99n");
        bits.insert(bits.end(), g.begin(), g.end());
    }
    // Some noise ahead, so it has to find the blocks.
    for (int i = 0; i < 37; i++) decoder.take_bit(i % 3 == 0);
    for (const int b : bits) decoder.take_bit(b);
    const auto& s = decoder.state();
    CHECK(s.synced);
    CHECK_EQ(s.pi, 0xd3c3);
    CHECK_EQ(s.pty, 10);
    CHECK(s.tp);
    CHECK_EQ_STR(s.ps, "FERN SDR");
    CHECK_EQ_STR(s.rt, "Hello from FernSDR, gr\xc3\xbcn");
    CHECK_EQ(decoder.bad_blocks(), 0u);

    // 2B radiotext, PI in C', and a new text after the A/B flag flips.
    bits.clear();
    for (int r = 0; r < 2; r++) {
        auto g = groups(0xd3c3, 10, "FERN SDR", "Short one", true, 1);
        bits.insert(bits.end(), g.begin(), g.end());
    }
    const uint64_t before = s.sequence;
    for (const int b : bits) decoder.take_bit(b);
    CHECK_EQ_STR(decoder.state().rt, "Short one");
    CHECK(decoder.state().sequence > before);
}

namespace {

void ps_group(std::vector<int>& bits, uint16_t pi, int segment, const char* two) {
    append_block(bits, pi, kOffsetA);
    append_block(bits, static_cast<uint16_t>(1 << 10 | 10 << 5 | segment), kOffsetB);
    append_block(bits, 0xe0cd, kOffsetC);
    append_block(bits, static_cast<uint16_t>(static_cast<uint8_t>(two[0]) << 8 | static_cast<uint8_t>(two[1])), kOffsetD);
}

void rt_group(std::vector<int>& bits, uint16_t pi, int segment, const std::string& text) {
    std::string padded = text + "\r";
    while (padded.size() % 4) padded += ' ';
    const auto c = [&](int k) { return static_cast<uint8_t>(padded[static_cast<size_t>(4 * segment + k)]); };
    append_block(bits, pi, kOffsetA);
    append_block(bits, static_cast<uint16_t>(2 << 12 | 10 << 5 | segment), kOffsetB);
    append_block(bits, static_cast<uint16_t>(c(0) << 8 | c(1)), kOffsetC);
    append_block(bits, static_cast<uint16_t>(c(2) << 8 | c(3)), kOffsetD);
}

// Feeds one group's bits, or garbles them all as a lost group would be.
void feed_group(RdsDecoder& decoder, const std::vector<int>& bits, bool lost = false) {
    for (size_t i = 0; i < bits.size(); i++) decoder.take_bit(lost && i % 3 == 0 ? bits[i] ^ 1 : bits[i]);
}

}  // namespace

TEST_CASE(rds_never_joins_two_texts_across_lost_groups) {
    // Two titles that share their start and middle, changed without the
    // A/B flag, the first with two segments lost: the second's fill the
    // holes, and a segment both share must not make that one text.
    const std::string one = "Artist One - Title One", two = "Artist Two - Title Two";
    RdsDecoder decoder(256000.0);
    std::vector<std::string> shown;
    const auto group = [&](const std::string& text, int segment, bool lost) {
        std::vector<int> bits;
        rt_group(bits, 0xd3c3, segment, text);
        feed_group(decoder, bits, lost);
        if (!decoder.state().rt.empty() && (shown.empty() || shown.back() != decoder.state().rt)) {
            shown.push_back(decoder.state().rt);
        }
    };
    const std::string before = "Zero hour news on the hour";
    for (int round = 0; round < 2; round++) {
        for (int segment = 0; segment < 7; segment++) group(before, segment, false);
    }
    // The first title's only pass, two of its segments lost.
    for (int segment = 0; segment < 6; segment++) group(one, segment, segment == 1 || segment == 2);
    for (int round = 0; round < 2; round++) {
        for (int segment = 0; segment < 6; segment++) group(two, segment, false);
    }
    CHECK(!shown.empty());
    for (const std::string& text : shown) CHECK(text == before || text == one || text == two);
    CHECK_EQ_STR(decoder.state().rt, two);

    // A scrolled name whose run is broken by lost groups across a step:
    // segments 0 and 1 of one step and 2 and 3 of the next are no name.
    RdsDecoder scrolled(256000.0);
    std::vector<std::string> names;
    const auto name = [&](const char* step, int segment, bool lost) {
        std::vector<int> bits;
        ps_group(bits, 0xd3c3, segment, step + 2 * segment);
        feed_group(scrolled, bits, lost);
        if (!scrolled.state().ps.empty()) names.push_back(scrolled.state().ps);
    };
    // Stations put other groups between the name's; here radiotext, so
    // the lost ones are scattered as they are on the air, not a gap long
    // enough to lose sync.
    const auto other = [&] {
        std::vector<int> bits;
        rt_group(bits, 0xd3c3, 0, "Hi");
        feed_group(scrolled, bits, false);
    };
    for (int segment = 0; segment < 4; segment++) name("AAAAAAAA", segment, false);
    name("AAAAAAAA", 0, false);
    name("AAAAAAAA", 1, false);
    const std::pair<const char*, int> lost[] = {{"AAAAAAAA", 2}, {"AAAAAAAA", 3}, {"BBBBBBBB", 0}, {"BBBBBBBB", 1}};
    for (const auto& [step, segment] : lost) {
        other();
        name(step, segment, true);
    }
    other();
    name("BBBBBBBB", 2, false);
    name("BBBBBBBB", 3, false);
    for (int segment = 0; segment < 4; segment++) name("BBBBBBBB", segment, false);
    for (const std::string& n : names) CHECK(n == "AAAAAAAA" || n == "BBBBBBBB");
    CHECK_EQ_STR(scrolled.state().ps, "BBBBBBBB");
}

TEST_CASE(rds_follows_a_new_text_without_the_flag_and_ignores_padding) {
    RdsDecoder decoder(256000.0);
    const auto feed_groups = [&](const std::string& rt) {
        for (int r = 0; r < 2; r++) {
            for (const int b : groups(0xd3c3, 10, "FERN SDR", rt)) decoder.take_bit(b);
        }
    };
    feed_groups("First text, the longer of the two");
    CHECK_EQ_STR(decoder.state().rt, "First text, the longer of the two");
    // The same A/B flag, as many stations send: still the new text alone.
    feed_groups("Second text");
    CHECK_EQ_STR(decoder.state().rt, "Second text");
    feed_groups("Second text, grown longer");
    CHECK_EQ_STR(decoder.state().rt, "Second text, grown longer");
    // The change hidden in a gap long enough to lose sync: the new text's
    // start and the old text's end never mix.
    for (const int b : groups(0xd3c3, 10, "FERN SDR", "Old text of some length, sent once")) decoder.take_bit(b);
    {
        std::vector<int> bits = groups(0xd3c3, 10, "FERN SDR", "Brand new words here, also long");
        // The first half of the new text's groups garbled, then the rest.
        for (size_t i = 0; i < bits.size() / 2; i++) bits[i] ^= (i % 3 == 0);
        for (const int b : bits) decoder.take_bit(b);
        for (const int b : groups(0xd3c3, 10, "FERN SDR", "Old text of some length, sent once")) decoder.take_bit(b);
    }
    CHECK_EQ_STR(decoder.state().rt, "Old text of some length, sent once");
    // Sent to all 64 characters, spaces after the end: the text stays.
    feed_groups("Padded\r" + std::string(56, ' '));
    CHECK_EQ_STR(decoder.state().rt, "Padded");
    const uint64_t settled = decoder.state().sequence;
    feed_groups("Padded\r" + std::string(56, ' '));
    CHECK_EQ(decoder.state().sequence, settled);
}

TEST_CASE(rds_shows_a_scrolling_name_step_by_step_and_a_steady_one_through_losses) {
    // A slogan scrolled a character at a time, each step in segment order,
    // with a group lost now and then: every name shown is one of the steps.
    const std::string slogan = "FERN SDR  THE RECEIVER  ";
    RdsDecoder scrolling(256000.0);
    std::vector<std::string> steps, shown;
    int lost = 0;
    for (size_t at = 0; at + 8 <= slogan.size(); at++) {
        const std::string step = slogan.substr(at, 8);
        steps.push_back(step);
        for (int repeat = 0; repeat < 2; repeat++) {
            for (int segment = 0; segment < 4; segment++) {
                if (++lost % 7 == 0) continue;
                std::vector<int> bits;
                ps_group(bits, 0xd3c3, segment, step.c_str() + 2 * segment);
                const uint64_t before = scrolling.state().sequence;
                for (const int b : bits) scrolling.take_bit(b);
                if (scrolling.state().sequence != before && !scrolling.state().ps.empty() &&
                    (shown.empty() || shown.back() != scrolling.state().ps)) {
                    shown.push_back(scrolling.state().ps);
                }
            }
        }
    }
    CHECK(shown.size() > steps.size() / 2);
    for (const std::string& name : shown) CHECK(std::find(steps.begin(), steps.end(), name) != steps.end());

    // A name that stays, its segments out of order and some lost: shown.
    RdsDecoder steady(256000.0);
    for (const int segment : {2, 0, 3, 1, 2, 0, 3, 1}) {
        std::vector<int> bits;
        ps_group(bits, 0xd3c3, segment, &"RADIO  1"[2 * segment]);
        for (const int b : bits) steady.take_bit(b);
    }
    CHECK_EQ_STR(steady.state().ps, "RADIO  1");
}

TEST_CASE(rds_is_decoded_from_a_multiplex_through_noise) {
    std::vector<int> bits;
    for (int r = 0; r < 4; r++) {
        auto g = groups(0x1234, 5, "RADIO 1 ", "Now playing: test signal");
        bits.insert(bits.end(), g.begin(), g.end());
    }
    for (const uint32_t seed : {1u, 2u, 3u}) {
        RdsDecoder decoder(256000.0);
        feed(decoder, multiplex(bits, 256000.0, 0.03, 0.02, seed));
        CHECK_EQ(decoder.state().pi, 0x1234);
        CHECK_EQ_STR(decoder.state().ps, "RADIO 1 ");
        CHECK_EQ_STR(decoder.state().rt, "Now playing: test signal");
        CHECK(decoder.bad_blocks() * 20 < decoder.good_blocks());
    }
    // Another channel rate, as a 2 Msps SDRplay band gives.
    RdsDecoder other(250000.0);
    feed(other, multiplex(bits, 250000.0, 0.03, 0.02, 9));
    CHECK_EQ_STR(other.state().ps, "RADIO 1 ");
    // The narrowest: a 2 Msps band with the 120 kHz filter gives 125 kHz.
    RdsDecoder narrow(125000.0);
    feed(narrow, multiplex(bits, 125000.0, 0.03, 0.02, 10));
    CHECK_EQ_STR(narrow.state().ps, "RADIO 1 ");
    CHECK_EQ_STR(narrow.state().rt, "Now playing: test signal");
    // Stereo with a loud treble, over ten times the RDS: its difference
    // signal reaches 53 kHz, 4 kHz from the subcarrier. With a clock off by
    // -150 ppm it took the loops off without the channel filter.
    for (const double hz : {8000.0, 12000.0, 14500.0}) {
        RdsDecoder stereo(256000.0);
        feed(stereo, multiplex(bits, 256000.0, 0.03, 0.02, 11, -150.0, 0.5, hz));
        CHECK_EQ_STR(stereo.state().ps, "RADIO 1 ");
        CHECK(stereo.bad_blocks() * 20 < stereo.good_blocks());
    }
    // Nothing but noise and programme: no sync, no text.
    RdsDecoder quiet(256000.0);
    feed(quiet, multiplex(bits, 256000.0, 0.0, 0.02, 4));
    CHECK(quiet.state().ps.empty());
    CHECK_EQ(quiet.state().pi, -1);
}
