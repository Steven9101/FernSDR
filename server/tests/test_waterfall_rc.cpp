#include <cmath>
#include <random>
#include <vector>

#include "../src/codec/waterfall_codec.h"
#include "../src/codec/waterfall_rc.h"
#include "test_util.h"

namespace {

// A waterfall-like row: a noise floor, carriers, a slow fade and a ramp.
std::vector<float> row(size_t width, int line, std::mt19937& rng, float sigma = 2.0f) {
    std::normal_distribution<float> noise(0.0f, sigma);
    std::vector<float> out(width);
    for (size_t i = 0; i < width; i++) {
        out[i] = -110.0f + noise(rng) + 3.0f * std::sin(line * 0.2f) + (i % 97 == 13 ? 45.0f : 0.0f) +
                 (i > width / 2 && i < width / 2 + 40 ? 0.5f * static_cast<float>(i - width / 2) : 0.0f);
    }
    return out;
}

bool same_levels(const std::vector<float>& db, const std::vector<float>& decoded, int step_db) {
    for (size_t i = 0; i < db.size(); i++) {
        if (decoded[i] != static_cast<float>(fernsdr::wfc::quantise_db(db[i], step_db) * step_db)) return false;
    }
    return true;
}

}  // namespace

TEST_CASE(ranged_rows_round_trip_at_every_width_and_step) {
    for (int step : {1, 2}) {
        for (size_t width : {size_t(1), size_t(2), size_t(15), size_t(527), size_t(1536), size_t(4096)}) {
            std::mt19937 rng(static_cast<uint32_t>(width * 7 + step));
            fernsdr::wfc::RangedLineEncoder encoder;
            fernsdr::wfc::RangedLineDecoder decoder;
            std::vector<float> out(width);
            for (int line = 0; line < 60; line++) {
                const auto db = row(width, line, rng);
                const auto& payload = encoder.encode(db.data(), width, false, step);
                CHECK(decoder.decode(payload.data(), payload.size(), width, out.data(), step));
                CHECK(same_levels(db, out, step));
            }
        }
    }
}

TEST_CASE(ranged_rows_carry_the_whole_level_range_and_extreme_jumps) {
    fernsdr::wfc::RangedLineEncoder encoder;
    fernsdr::wfc::RangedLineDecoder decoder;
    const size_t width = 64;
    std::vector<float> out(width);
    for (int line = 0; line < 30; line++) {
        std::vector<float> db(width);
        for (size_t i = 0; i < width; i++) {
            // Alternating floor and ceiling, beyond both, and NaN-free extremes.
            db[i] = (i + line) % 2 ? 150.0f : -300.0f;
            if (i % 5 == 0) db[i] = -200.0f + static_cast<float>(line * 10 % 300);
        }
        for (int step : {1, 2}) {
            const auto& payload = encoder.encode(db.data(), width, false, step);
            CHECK(decoder.decode(payload.data(), payload.size(), width, out.data(), step));
            CHECK(same_levels(db, out, step));
        }
    }
}

TEST_CASE(ranged_rows_need_a_key_row_after_a_loss) {
    std::mt19937 rng(3);
    fernsdr::wfc::RangedLineEncoder encoder;
    fernsdr::wfc::RangedLineDecoder decoder;
    const size_t width = 256;
    std::vector<float> out(width);
    int decoded_after_loss = 0, rejected_after_loss = 0;
    for (int line = 0; line < 3 * fernsdr::wfc::kKeyEvery; line++) {
        const auto db = row(width, line, rng);
        const auto& payload = encoder.encode(db.data(), width, false, 2);
        const bool key = encoder.last_was_key();
        CHECK_EQ(key, line % fernsdr::wfc::kKeyEvery == 0);
        if (line == 5) continue;  // lost
        const bool ok = decoder.decode(payload.data(), payload.size(), width, out.data(), 2);
        if (line > 5 && line < fernsdr::wfc::kKeyEvery) {
            CHECK(!ok);
            rejected_after_loss++;
        } else {
            CHECK(ok);
            CHECK(same_levels(db, out, 2));
            if (line >= fernsdr::wfc::kKeyEvery) decoded_after_loss++;
        }
    }
    CHECK(rejected_after_loss > 0);
    CHECK(decoded_after_loss > 0);
}

TEST_CASE(ranged_rows_start_a_key_row_on_a_forced_reset_width_or_step_change) {
    std::mt19937 rng(4);
    fernsdr::wfc::RangedLineEncoder encoder;
    auto db = row(128, 0, rng);
    encoder.encode(db.data(), 128, false, 1);
    encoder.encode(db.data(), 128, false, 1);
    CHECK(!encoder.last_was_key());
    encoder.encode(db.data(), 128, true, 1);
    CHECK(encoder.last_was_key());
    encoder.encode(db.data(), 128, false, 2);
    CHECK(encoder.last_was_key());
    db = row(100, 1, rng);
    encoder.encode(db.data(), 100, false, 2);
    CHECK(encoder.last_was_key());
}

TEST_CASE(ranged_rows_reject_malformed_payloads) {
    std::mt19937 rng(5);
    fernsdr::wfc::RangedLineEncoder encoder;
    const size_t width = 512;
    const auto db = row(width, 0, rng, 6.0f);
    const auto payload = encoder.encode(db.data(), width, false, 1);
    std::vector<float> out(width);
    fernsdr::wfc::RangedLineDecoder decoder;
    // Reserved header bits.
    auto bad = payload;
    bad[0] |= 2;
    CHECK(!decoder.decode(bad.data(), bad.size(), width, out.data(), 1));
    // Cut short.
    CHECK(!decoder.decode(payload.data(), payload.size() / 2, width, out.data(), 1));
    CHECK(!decoder.decode(payload.data(), 0, width, out.data(), 1));
    // Random bytes never crash and never yield out-of-range levels.
    std::uniform_int_distribution<int> byte(0, 255);
    for (int t = 0; t < 200; t++) {
        std::vector<uint8_t> junk(1 + t % 64);
        for (auto& b : junk) b = static_cast<uint8_t>(byte(rng));
        junk[0] &= 1;
        junk[0] |= 1;
        if (decoder.decode(junk.data(), junk.size(), width, out.data(), 1)) {
            for (float v : out) CHECK(v >= -200.0f && v <= 100.0f);
        }
    }
    // A good key row still decodes afterwards.
    CHECK(decoder.decode(payload.data(), payload.size(), width, out.data(), 1));
}

TEST_CASE(ranged_rows_are_smaller_than_wfc4_on_a_quiet_band) {
    // The regime the lab measured: rows averaged over many transforms, so a
    // bin's level moves by about half a decibel from row to row, in 2 dB
    // steps. On 288 real rows from the lab WFC5 took 31.8 % less than WFC4.
    std::mt19937 rng(6);
    fernsdr::wfc::RangedLineEncoder ranged;
    fernsdr::wfc::LineEncoder rice;
    const size_t width = 1536;
    size_t ranged_bytes = 0, rice_bytes = 0;
    for (int line = 0; line < 96; line++) {
        const auto db = row(width, line, rng, 0.5f);
        ranged_bytes += ranged.encode(db.data(), width, false, 2).size();
        rice_bytes += rice.encode(db.data(), width, false, true, true, 2).size();
    }
    CHECK(ranged_bytes * 5 < rice_bytes * 4);
}
