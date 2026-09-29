#include "../src/util/bitio.h"
#include "test_util.h"

#include <algorithm>
#include <random>

using fernsdr::BitReader;
using fernsdr::BitWriter;

TEST_CASE(bitio_raw_bits_roundtrip) {
    BitWriter w;
    w.put_bits(0b101, 3);
    w.put_bits(0xDEADBEEF, 32);
    w.put_bit(1);
    w.put_bits(0, 5);
    auto bytes = w.finish();

    BitReader r(bytes.data(), bytes.size());
    CHECK_EQ(r.get_bits(3), 0b101);
    CHECK_EQ(r.get_bits(32), 0xDEADBEEF);
    CHECK_EQ(r.get_bit(), 1);
    CHECK_EQ(r.get_bits(5), 0);
    CHECK(!r.overrun());
}

TEST_CASE(bitio_rice_roundtrip_including_escape) {
    std::mt19937 rng(1234);
    std::vector<uint32_t> values;
    std::vector<uint32_t> ks;
    for (int i = 0; i < 500; i++) {
        uint32_t k = rng() % 12;
        // Mix of small values and ones large enough to trip the escape path.
        uint32_t v = (i % 7 == 0) ? rng() : (rng() % (1u << (k + 3)));
        values.push_back(v);
        ks.push_back(k);
    }

    BitWriter w;
    for (size_t i = 0; i < values.size(); i++) w.put_rice(values[i], ks[i]);
    auto bytes = w.finish();

    BitReader r(bytes.data(), bytes.size());
    for (size_t i = 0; i < values.size(); i++) CHECK_EQ(r.get_rice(ks[i]), values[i]);
    CHECK(!r.overrun());
}

TEST_CASE(bitio_a_rice_run_writes_the_bits_of_single_codes) {
    std::mt19937 rng(77);
    for (uint32_t k = 0; k <= 31; k++) {
        std::vector<int32_t> values;
        for (int i = 0; i < 200; i++) {
            // Small values, ones that cross a word, escapes, and fields longer
            // than 32 bits that are not escapes.
            const int kind = static_cast<int>(rng() % 4);
            const int32_t magnitude = kind == 0 ? static_cast<int32_t>(rng() % (2u << std::min(k, 29u)))
                                    : kind == 1 ? static_cast<int32_t>(rng() >> 1)
                                    : kind == 2 ? static_cast<int32_t>((rng() % 30) << std::min(k, 26u))
                                                : static_cast<int32_t>(rng() % 3);
            values.push_back(rng() % 2 ? magnitude : -magnitude);
        }
        // Started at every alignment of the accumulator.
        for (uint32_t lead = 0; lead < 40; lead += 13) {
            BitWriter single, run;
            single.put_bits(0x5A5A5A5Au, std::min(lead, 32u));
            run.put_bits(0x5A5A5A5Au, std::min(lead, 32u));
            for (int32_t v : values) single.put_signed_rice(v, k);
            run.put_signed_rice_run(values.data(), values.size(), k);
            single.put_bits(5, 3);
            run.put_bits(5, 3);
            CHECK_EQ(run.bits_written(), single.bits_written());
            CHECK(run.finish() == single.finish());
        }
    }
}

TEST_CASE(bitio_zero_runs_write_the_bits_of_single_codes) {
    std::mt19937 rng(78);
    for (uint32_t k = 0; k <= 31; k++) {
        std::vector<int32_t> values;
        for (int i = 0; i < 400; i++) {
            const int kind = static_cast<int>(rng() % 6);
            if (kind <= 1) {
                // A run of zeros, now and then a long one.
                const int run = kind == 0 ? 1 + static_cast<int>(rng() % 5) : static_cast<int>(rng() % 700);
                values.insert(values.end(), static_cast<size_t>(run), 0);
                continue;
            }
            const int32_t magnitude = kind == 2 ? 1 + static_cast<int32_t>(rng() % (2u << std::min(k, 29u)))
                                    : kind == 3 ? static_cast<int32_t>(rng() >> 1) | 1
                                    : kind == 4 ? static_cast<int32_t>((1 + rng() % 30) << std::min(k, 26u))
                                                : 1;
            values.push_back(rng() % 2 ? magnitude : -magnitude);
        }
        for (uint32_t lead = 0; lead < 40; lead += 13) {
            BitWriter single, run;
            single.put_bits(0xA5A5A5A5u, std::min(lead, 32u));
            run.put_bits(0xA5A5A5A5u, std::min(lead, 32u));
            for (size_t i = 0; i < values.size();) {
                if (values[i] == 0) {
                    const size_t start = i++;
                    while (i < values.size() && values[i] == 0) i++;
                    single.put_bit(0);
                    single.put_exp_golomb(static_cast<uint32_t>(i - start - 1));
                } else {
                    single.put_bit(1);
                    single.put_signed_rice(values[i++], k);
                }
            }
            run.put_signed_rice_with_zero_runs(values.data(), values.size(), k);
            single.put_bits(3, 2);
            run.put_bits(3, 2);
            CHECK_EQ(run.bits_written(), single.bits_written());
            CHECK(run.finish() == single.finish());
        }
    }
}

TEST_CASE(bitio_rice_cost_matches_actual_length) {
    for (uint32_t k = 0; k < 8; k++) {
        for (uint32_t v : {0u, 1u, 5u, 100u, 100000u, 0xFFFFFFFFu}) {
            BitWriter w;
            size_t before = w.bits_written();
            w.put_rice(v, k);
            CHECK_EQ(static_cast<long long>(w.bits_written() - before), fernsdr::rice_cost(v, k));
        }
    }
}

TEST_CASE(bitio_signed_and_exp_golomb_roundtrip) {
    std::mt19937 rng(99);
    std::vector<int32_t> values;
    for (int i = 0; i < 300; i++) values.push_back(static_cast<int32_t>(rng()) / (1 << (rng() % 20)));

    BitWriter w;
    for (int32_t v : values) { w.put_signed_exp_golomb(v); w.put_signed_rice(v, 4); }
    auto bytes = w.finish();

    BitReader r(bytes.data(), bytes.size());
    for (int32_t v : values) {
        CHECK_EQ(r.get_signed_exp_golomb(), v);
        CHECK_EQ(r.get_signed_rice(4), v);
    }
    CHECK(!r.overrun());
}

TEST_CASE(bitio_reader_flags_overrun_instead_of_reading_past_end) {
    uint8_t data[2] = {0xFF, 0xFF};
    BitReader r(data, sizeof(data));
    r.get_bits(16);
    CHECK(!r.overrun());
    r.get_bits(8);
    CHECK(r.overrun());
}
