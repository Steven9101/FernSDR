#include "../src/codec/waterfall_codec.h"
#include "test_util.h"

#include <cmath>
#include <random>
#include <vector>

using fernsdr::wfc::LineDecoder;
using fernsdr::wfc::LineEncoder;

TEST_CASE(waterfall_precision_preserves_bins_and_bounds_direct_rounding_error) {
    for (int step : {1, 2}) for (bool adaptive : {false, true}) {
        LineEncoder encoder;
        LineDecoder decoder;
        std::vector<float> source(2048), out(2048);
        for (int row = 0; row < 8; row++) {
            for (size_t i = 0; i < source.size(); i++) {
                source[i] = -200 + std::fmod(i * 0.137f + row * 0.61f, 300.0f);
            }
            const auto& bytes = encoder.encode(source.data(), source.size(), row == 4, true, adaptive, step);
            CHECK(decoder.decode(bytes.data(), bytes.size(), source.size(), out.data(),
                                 encoder.used_zero_runs(), encoder.used_adaptive(), step));
            for (size_t i = 0; i < source.size(); i++) CHECK_NEAR(out[i], source[i], step * 0.5);
        }
    }
    CHECK_EQ(fernsdr::wfc::quantise_db(-100.51f, 2) * 2, -100);
    CHECK_EQ(fernsdr::wfc::quantise_db(0.51f, 2) * 2, 0);
}

TEST_CASE(waterfall_precision_change_recovers_with_an_independent_row) {
    LineEncoder encoder;
    LineDecoder decoder;
    std::vector<float> source(64), out(64);
    for (size_t i = 0; i < source.size(); i++) source[i] = -180.0f + (i * 73) % 250;
    for (int step : {1, 2, 1}) {
        const auto key = encoder.encode(source.data(), source.size(), false, true, true, step);
        const bool runs = encoder.used_zero_runs(), adaptive = encoder.used_adaptive();
        // Losing the transition row cannot make the decoder reinterpret its
        // previous values in the new units, even when the width is unchanged.
        const auto next = encoder.encode(source.data(), source.size(), false, true, true, step);
        CHECK(!decoder.decode(next.data(), next.size(), source.size(), out.data(),
                              encoder.used_zero_runs(), encoder.used_adaptive(), step));
        CHECK(decoder.decode(key.data(), key.size(), source.size(), out.data(), runs, adaptive, step));
        CHECK(decoder.decode(next.data(), next.size(), source.size(), out.data(),
                             encoder.used_zero_runs(), encoder.used_adaptive(), step));
        for (size_t i = 0; i < source.size(); i++) CHECK_NEAR(out[i], source[i], step * 0.5);
    }
    fernsdr::BitWriter writer;
    writer.put_bits(1, 1);
    writer.put_bits(0, 4);
    writer.put_signed_rice(101, 0); // Seed -50 plus 101 exceeds +100 dB at step 2.
    const auto malformed = writer.finish();
    CHECK(!decoder.decode(malformed.data(), malformed.size(), 1, out.data(), false, false, 2));
    CHECK(encoder.encode(source.data(), source.size(), false, true, true, 0).empty());
    CHECK(!decoder.decode(malformed.data(), malformed.size(), 1, out.data(), false, false, 0));
}

TEST_CASE(waterfall_adaptive_is_exact_and_never_larger_than_the_original) {
    std::mt19937 rng(2323);
    size_t saved = 0, extended = 0;
    for (size_t width : {2, 15, 16, 64, 1024, 4096}) {
        LineEncoder original, adaptive;
        LineDecoder decoder;
        std::vector<float> line(width), out(width);
        for (int row = 0; row < 40; row++) {
            for (size_t i = 0; i < width; i++) {
                line[i] = row < 10 ? -130 + static_cast<int>(i % 100) :
                    row < 20 ? -95 + static_cast<int>(rng() % 7) :
                    -105 + static_cast<int>(i % 3 == 0 ? row / 2 : i % 40);
            }
            const bool key = row % 7 == 0;
            const auto& old = original.encode(line.data(), width, key, true);
            const auto& next = adaptive.encode(line.data(), width, key, true, true);
            CHECK(next.size() <= old.size());
            saved += old.size() - next.size();
            extended += adaptive.used_adaptive();
            if (key) decoder.reset();
            CHECK(decoder.decode(next.data(), next.size(), width, out.data(),
                                  adaptive.used_zero_runs(), adaptive.used_adaptive()));
            CHECK(line == out);
        }
    }
    CHECK(saved > 1000);
    CHECK(extended > 10);
}

TEST_CASE(waterfall_adaptive_recovers_after_corruption_and_format_changes) {
    LineEncoder encoder;
    LineDecoder decoder;
    std::vector<float> line(1024), out(1024);
    for (size_t i = 0; i < line.size(); i++) line[i] = -140.0f + i % 200;
    const auto packet = encoder.encode(line.data(), line.size(), true, true, true);
    CHECK(encoder.used_adaptive());
    CHECK(!decoder.decode(packet.data(), packet.size() / 2, line.size(), out.data(), encoder.used_zero_runs(), true));
    CHECK(decoder.decode(packet.data(), packet.size(), line.size(), out.data(), encoder.used_zero_runs(), true));
    CHECK(out == line);
    for (int row = 0; row < 10; row++) {
        const auto& next = encoder.encode(line.data(), line.size(), false, true, row % 2 == 0);
        CHECK(decoder.decode(next.data(), next.size(), line.size(), out.data(),
                              encoder.used_zero_runs(), encoder.used_adaptive()));
        CHECK(out == line);
    }
    for (int mode : {0, 2}) {
        fernsdr::BitWriter writer;
        writer.put_bits(mode, 2);
        writer.put_bits(0, 4);
        const auto& header = writer.finish();
        decoder.reset();
        CHECK(!decoder.decode(header.data(), header.size(), line.size(), out.data(), false, true));
    }
    CHECK(!decoder.decode(packet.data(), packet.size(), 65536, out.data(), false, true));
}

TEST_CASE(waterfall_zero_runs_keep_all_bins_without_spending_a_bit_per_empty_bin) {
    std::vector<float> line(4096, -105.0f), out(4096);
    line[300] = -42;
    LineEncoder encoder;
    LineDecoder decoder;
    for (int i = 0; i < 8; i++) {
        const auto& payload = encoder.encode(line.data(), line.size(), i == 4, true);
        CHECK(encoder.used_zero_runs());
        CHECK(payload.size() < 32);
        CHECK(decoder.decode(payload.data(), payload.size(), line.size(), out.data(), true));
        CHECK(out == line);
    }
}

TEST_CASE(waterfall_zero_run_cannot_write_past_the_requested_width) {
    fernsdr::BitWriter writer;
    writer.put_bits(1, 1);
    writer.put_bits(0, 4);
    writer.put_bit(0);
    writer.put_exp_golomb(4096);
    const auto& payload = writer.finish();
    LineDecoder decoder;
    std::vector<float> out(32, 123.0f);
    CHECK(!decoder.decode(payload.data(), payload.size(), 16, out.data(), true));
    for (size_t i = 16; i < out.size(); i++) CHECK_NEAR(out[i], 123, 0);
}

namespace {

// A plausible spectrum: a flat-ish noise floor that drifts slowly, plus a few
// carriers.  This is what the compressor is tuned against.
std::vector<std::vector<float>> synthetic_spectrum(size_t lines, size_t width, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.5f);
    std::vector<std::vector<float>> out;
    for (size_t l = 0; l < lines; l++) {
        std::vector<float> line(width);
        const float drift = 2.0f * std::sin(static_cast<float>(l) * 0.05f);
        for (size_t i = 0; i < width; i++) {
            line[i] = -105.0f + drift + noise(rng);
            if (i % 137 == 40) line[i] += 45.0f;   // steady carrier
            if (i % 311 == 100) line[i] += 25.0f;  // weaker carrier
        }
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace

TEST_CASE(waterfall_roundtrip_is_accurate_to_the_quantiser_step) {
    const size_t width = 512;
    auto lines = synthetic_spectrum(30, width, 5);

    LineEncoder enc;
    LineDecoder dec;
    std::vector<float> out(width);

    for (auto& line : lines) {
        const auto& packet = enc.encode(line.data(), width);
        CHECK(dec.decode(packet.data(), packet.size(), width, out.data()));
        for (size_t i = 0; i < width; i++) CHECK_NEAR(out[i], line[i], fernsdr::wfc::kDbStep / 2 + 1e-4);
    }
}

TEST_CASE(waterfall_first_line_is_intra_and_decodes_standalone) {
    const size_t width = 256;
    auto lines = synthetic_spectrum(3, width, 9);

    LineEncoder enc;
    const auto& first = enc.encode(lines[0].data(), width);

    // A fresh decoder, with no history at all, must handle the first line.
    LineDecoder dec;
    std::vector<float> out(width);
    CHECK(dec.decode(first.data(), first.size(), width, out.data()));
    for (size_t i = 0; i < width; i++) CHECK_NEAR(out[i], lines[0][i], fernsdr::wfc::kDbStep / 2 + 1e-4);
}

TEST_CASE(waterfall_temporal_line_is_rejected_without_history) {
    const size_t width = 128;
    auto lines = synthetic_spectrum(2, width, 3);
    LineEncoder enc;
    enc.encode(lines[0].data(), width);
    const auto& second = enc.encode(lines[1].data(), width);

    LineDecoder fresh;
    std::vector<float> out(width);
    CHECK(!fresh.decode(second.data(), second.size(), width, out.data()));
}

TEST_CASE(waterfall_width_change_forces_an_intra_line) {
    // When a user zooms, the line width changes and every decoder's history
    // becomes meaningless.  The encoder must switch to intra so the next line
    // stands alone, otherwise the display would stay broken until a reset.
    auto wide = synthetic_spectrum(1, 512, 1)[0];
    auto narrow = synthetic_spectrum(1, 256, 2)[0];

    LineEncoder enc;
    enc.encode(wide.data(), 512);
    const auto& packet = enc.encode(narrow.data(), 256);

    LineDecoder fresh;  // stands in for any decoder whose history is stale
    std::vector<float> out(256);
    CHECK(fresh.decode(packet.data(), packet.size(), 256, out.data()));
    for (size_t i = 0; i < 256; i++) CHECK_NEAR(out[i], narrow[i], fernsdr::wfc::kDbStep / 2 + 1e-4);
}

TEST_CASE(waterfall_compression_meets_the_bandwidth_budget) {
    // The deployment constraint is a total of well under 100 kbit/s per user,
    // shared with the audio stream.  Check the two profiles the server ships:
    //   standard - 1024 bins at 12 lines/s alongside 48 kbit/s audio
    //   low      -  768 bins at  8 lines/s alongside 32 kbit/s audio
    // The 1.5 dB per-line noise here corresponds to a spectrum averaged over
    // roughly a dozen FFTs, which is what the server actually feeds it.
    auto measure_bits_per_bin = [](size_t width) {
        auto lines = synthetic_spectrum(100, width, 21);
        LineEncoder enc;
        size_t total_bits = 0;
        for (auto& line : lines) total_bits += enc.encode(line.data(), width).size() * 8;
        return static_cast<double>(total_bits) / (lines.size() * width);
    };

    const double standard_bpb = measure_bits_per_bin(1024);
    CHECK(standard_bpb < 3.6);
    const double standard_kbps = standard_bpb * 1024 * 12 / 1000.0 + 48.0;
    CHECK(standard_kbps < 100.0);

    const double low_bpb = measure_bits_per_bin(768);
    const double low_kbps = low_bpb * 768 * 8 / 1000.0 + 32.0;
    CHECK(low_kbps < 55.0);
}

TEST_CASE(waterfall_static_spectrum_costs_almost_nothing) {
    // An unchanging display should cost close to the 1 bit/bin Rice floor.
    const size_t width = 512;
    std::vector<float> line(width);
    for (size_t i = 0; i < width; i++) line[i] = -100.0f + static_cast<float>(i % 13);

    LineEncoder enc;
    enc.encode(line.data(), width);  // intra
    size_t bits = 0;
    for (int i = 0; i < 20; i++) bits += enc.encode(line.data(), width).size() * 8;
    CHECK(static_cast<double>(bits) / (20 * width) < 1.3);
}

TEST_CASE(waterfall_decoder_rejects_truncated_frames) {
    const size_t width = 256;
    auto lines = synthetic_spectrum(1, width, 4);
    LineEncoder enc;
    const auto& packet = enc.encode(lines[0].data(), width);

    LineDecoder dec;
    std::vector<float> out(width);
    CHECK(!dec.decode(packet.data(), packet.size() / 3, width, out.data()));
}

TEST_CASE(waterfall_rice_parameter_choice_beats_neighbours) {
    std::mt19937 rng(17);
    std::normal_distribution<float> dist(0.0f, 6.0f);
    std::vector<int> residuals(400);
    for (auto& r : residuals) r = static_cast<int>(dist(rng));

    const uint32_t k = fernsdr::wfc::choose_rice_param(residuals.data(), residuals.size());
    auto cost = [&](uint32_t kk) {
        uint64_t c = 0;
        for (int r : residuals) c += fernsdr::rice_cost(fernsdr::zigzag_encode(r), kk);
        return c;
    };
    for (uint32_t other = 0; other <= fernsdr::wfc::kMaxRiceParam; other++) {
        CHECK(cost(k) <= cost(other));
    }
}
TEST_CASE(waterfall_extreme_levels_are_clamped_before_integer_conversion) {
    CHECK(fernsdr::wfc::quantise_db(-INFINITY) == fernsdr::wfc::kMinLevelQ);
    CHECK(fernsdr::wfc::quantise_db(INFINITY) == fernsdr::wfc::kMaxLevelQ);
    CHECK(fernsdr::wfc::quantise_db(NAN) == fernsdr::wfc::kMinLevelQ);
}
