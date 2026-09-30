#include "../src/dsp/spectrum.h"
#include "../src/dsp/fft_split.h"
#include "../src/dsp/simd.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <vector>

using fernsdr::cfloat;
using fernsdr::SpectrumAnalyzer;

TEST_CASE(spectrum_native_viewport_keeps_bins_and_frequency_alignment) {
    std::vector<float> original(1024);
    for (size_t i = 0; i < original.size(); i++) original[i] = -90 + 20 * std::sin(i * 0.2);
    original[525] = -12;
    fernsdr::SpectrumPyramid pyramid;
    const double low = 7000000 - 5, high = low + 10240;
    pyramid.build(original.data(), original.size(), low, high);
    for (double view_low : {low, low + 10.125, low + 5231.75, high - 10}) {
        for (double span : {0.125, 1.0, 9.0}) {
            std::vector<float> native, before(256), after(256);
            double row_low = 0, row_high = 0;
            CHECK(pyramid.native_viewport(view_low, view_low + span, before.size(), native, row_low, row_high));
            CHECK(native.size() >= 2 && native.size() <= 3);
            CHECK(row_low <= view_low && row_high >= view_low + span);
            CHECK_NEAR((row_high - row_low) / native.size(), 10.0, 1e-9);
            pyramid.render(view_low, view_low + span, before.data(), before.size());
            fernsdr::render_viewport(native.data(), native.size(), row_low, row_high,
                                     view_low, view_low + span, after.data(), after.size());
            for (size_t i = 0; i < before.size(); i++) CHECK_NEAR(before[i], after[i], 1e-5);
        }
    }
    std::vector<float> untouched{123};
    double a = 0, b = 0;
    CHECK(!pyramid.native_viewport(low, high, 256, untouched, a, b));
    CHECK(!pyramid.native_viewport(low - 1, low + 100, 256, untouched, a, b));
    CHECK_EQ(untouched.size(), 1);
    CHECK_NEAR(untouched[0], 123, 0);
}

namespace {

constexpr double kRate = 192000.0;
constexpr size_t kBins = 4096;

// Runs a complex tone through the analyser and returns the first line.
std::vector<float> line_for_tone(double tone_hz, double amplitude, int averages = 4) {
    SpectrumAnalyzer analyzer(kRate, kBins, 20.0, averages);
    std::vector<cfloat> block(4096);
    std::vector<float> line;
    size_t n = 0;
    for (int b = 0; b < 200 && line.empty(); b++) {
        for (size_t i = 0; i < block.size(); i++, n++) {
            const double a = 2.0 * M_PI * tone_hz * static_cast<double>(n) / kRate;
            block[i] = cfloat(static_cast<float>(amplitude * std::cos(a)),
                              static_cast<float>(amplitude * std::sin(a)));
        }
        analyzer.push(block.data(), block.size());
        std::vector<float> out;
        if (analyzer.take_line(out)) line = out;
    }
    return line;
}

size_t peak_bin(const std::vector<float>& line) {
    size_t best = 0;
    for (size_t i = 1; i < line.size(); i++)
        if (line[i] > line[best]) best = i;
    return best;
}

}  // namespace

TEST_CASE(spectrum_reports_full_scale_tone_at_zero_dbfs) {
    auto line = line_for_tone(30000.0, 1.0);
    CHECK(!line.empty());
    CHECK_NEAR(line[peak_bin(line)], 0.0, 0.2);
}

TEST_CASE(spectrum_level_scales_with_amplitude) {
    auto full = line_for_tone(30000.0, 1.0);
    auto down40 = line_for_tone(30000.0, 0.01);
    CHECK_NEAR(down40[peak_bin(down40)] - full[peak_bin(full)], -40.0, 0.3);
}

TEST_CASE(spectrum_places_the_tone_at_the_right_frequency) {
    // Index 0 is the lowest frequency: -rate/2.
    const double tone = 30000.0;
    auto line = line_for_tone(tone, 1.0);
    const double bin_hz = kRate / kBins;
    const double got_hz = -kRate / 2.0 + static_cast<double>(peak_bin(line)) * bin_hz;
    CHECK_NEAR(got_hz, tone, bin_hz);

    auto negative = line_for_tone(-45000.0, 1.0);
    const double got_neg = -kRate / 2.0 + static_cast<double>(peak_bin(negative)) * bin_hz;
    CHECK_NEAR(got_neg, -45000.0, bin_hz);
}

TEST_CASE(spectrum_window_keeps_sidelobes_far_down) {
    // A strong carrier must not paint a skirt across the display.
    auto line = line_for_tone(30000.0, 1.0);
    const size_t peak = peak_bin(line);
    float worst = -200.0f;
    for (size_t i = 0; i < line.size(); i++) {
        if (i + 20 > peak && i < peak + 20) continue;  // skip the main lobe
        worst = std::max(worst, line[i]);
    }
    CHECK(worst < -85.0);
}

TEST_CASE(spectrum_line_rate_matches_the_request) {
    SpectrumAnalyzer analyzer(kRate, kBins, 20.0, 4);
    std::vector<cfloat> block(static_cast<size_t>(kRate));  // exactly one second
    for (auto& s : block) s = cfloat(0.001f, 0.0f);

    int lines = 0;
    std::vector<float> out;
    // Push in chunks so take_line gets a chance between transforms.
    const size_t chunk = 1024;
    for (size_t off = 0; off < block.size(); off += chunk) {
        analyzer.push(block.data() + off, std::min(chunk, block.size() - off));
        while (analyzer.take_line(out)) lines++;
    }
    // The first line needs a full FFT of history before it can be produced.
    CHECK(lines >= 18 && lines <= 21);
}

TEST_CASE(spectrum_leaves_out_averages_that_overlap_past_three_quarters) {
    // A 2.048 Msps band: 25 lines of 81920 samples, 65536-point transforms.
    // Eight averages would overlap by 84 %; five hop exactly a quarter.
    SpectrumAnalyzer narrow(2048000.0, 65536, 25.0, 8);
    CHECK_EQ(narrow.averages(), 5);
    // A wide band's hop is longer than its transform: every average counts.
    SpectrumAnalyzer wide(64000000.0, 65536, 12.0, 8);
    CHECK_EQ(wide.averages(), 8);
    // The line rate does not change with it.
    SpectrumAnalyzer analyzer(kRate, kBins, 20.0, 16);
    CHECK_EQ(analyzer.averages(), 9);
    std::vector<cfloat> block(static_cast<size_t>(kRate), cfloat(0.001f, 0.0f));
    int lines = 0;
    std::vector<float> out;
    for (size_t off = 0; off < block.size(); off += 1024) {
        analyzer.push(block.data() + off, std::min<size_t>(1024, block.size() - off));
        while (analyzer.take_line(out)) lines++;
    }
    CHECK(lines >= 18 && lines <= 21);
}

TEST_CASE(spectrum_stride_skips_transforms_and_returns_at_once) {
    // 20 lines a second of 4 averages; a stride of 10 leaves two lines a
    // second and a tenth of the transforms.
    SpectrumAnalyzer analyzer(kRate, kBins, 20.0, 4);
    std::vector<cfloat> chunk(512, cfloat(0.001f, 0.0f));
    std::vector<float> out;
    const auto run = [&](double seconds, int& lines) {
        const size_t pushes = static_cast<size_t>(seconds * kRate / static_cast<double>(chunk.size()));
        for (size_t i = 0; i < pushes; i++) {
            analyzer.push(chunk.data(), chunk.size());
            while (analyzer.take_line(out)) lines++;
        }
    };
    int lines = 0;
    run(1.0, lines);
    analyzer.set_stride(10);
    const uint64_t before = analyzer.transforms();
    lines = 0;
    run(2.0, lines);
    CHECK(lines >= 3 && lines <= 5);
    CHECK(analyzer.transforms() - before <= 20);
    CHECK_NEAR(analyzer.last_line_seconds(), 0.5, 0.01);

    // Back to every line: the next one is one line's worth of hops away.
    analyzer.set_stride(1);
    size_t pushed = 0;
    lines = 0;
    while (lines == 0 && pushed < static_cast<size_t>(kRate)) {
        analyzer.push(chunk.data(), chunk.size());
        pushed += chunk.size();
        while (analyzer.take_line(out)) lines++;
    }
    CHECK(pushed <= static_cast<size_t>(kRate / 20.0) + chunk.size());
    lines = 0;
    run(1.0, lines);
    CHECK(lines >= 19 && lines <= 21);
}

TEST_CASE(spectrum_stride_finishes_the_line_under_way) {
    // A tone line half averaged when the stride goes up still gets all its
    // averages from consecutive hops, not some from a second later.
    SpectrumAnalyzer steady(kRate, kBins, 20.0, 4), strided(kRate, kBins, 20.0, 4);
    std::vector<cfloat> sample(1);
    std::vector<float> a, b;
    bool compared = false;
    for (size_t n = 0; n < static_cast<size_t>(kRate) && !compared; n++) {
        const double phase = 2 * M_PI * 1234.5 * static_cast<double>(n) / kRate;
        sample[0] = cfloat(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
        steady.push(sample.data(), 1);
        strided.push(sample.data(), 1);
        if (n == static_cast<size_t>(kRate * 0.3) + 1200) strided.set_stride(10);
        const bool got_a = steady.take_line(a), got_b = strided.take_line(b);
        CHECK_EQ(got_a, got_b);
        if (got_a && n > static_cast<size_t>(kRate * 0.3) + 1200) {
            CHECK(a == b);
            compared = true;
        }
    }
    CHECK(compared);
}

TEST_CASE(spectrum_bulk_input_keeps_hop_timing_and_levels_across_ring_wraps) {
    for (auto kind : {fernsdr::SignalKind::Iq, fernsdr::SignalKind::Real}) {
        for (int averages : {1, 8}) {
            SpectrumAnalyzer bulk(12000, 256, 31, averages, 0.5f, kind);
            SpectrumAnalyzer single(12000, 256, 31, averages, 0.5f, kind);
            std::vector<cfloat> iq(701);
            std::vector<float> real(iq.size());
            size_t position = 0;
            for (int block = 0; block < 16; block++) {
                for (size_t i = 0; i < iq.size(); i++, position++) {
                    const double phase = 2 * M_PI * 813.7 * position / 12000;
                    real[i] = static_cast<float>(std::cos(phase));
                    iq[i] = cfloat(real[i], static_cast<float>(std::sin(phase)));
                    if (kind == fernsdr::SignalKind::Iq) single.push(&iq[i], 1);
                    else single.push_real(&real[i], 1);
                }
                if (kind == fernsdr::SignalKind::Iq) bulk.push(iq.data(), iq.size());
                else bulk.push_real(real.data(), real.size());
                std::vector<float> a, b;
                CHECK_EQ(bulk.take_line(a), single.take_line(b));
                CHECK(a == b);
            }
        }
    }
}

TEST_CASE(viewport_full_span_preserves_the_line) {
    std::vector<float> src(1024);
    for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<float>(i % 50) - 100.0f;
    std::vector<float> dst(1024);
    fernsdr::render_viewport(src.data(), src.size(), -1000.0, 1000.0, -1000.0, 1000.0, dst.data(), dst.size());
    for (size_t i = 0; i < src.size(); i++) CHECK_NEAR(dst[i], src[i], 1e-4);
}

TEST_CASE(viewport_zoomed_out_keeps_narrow_carriers_visible) {
    // The property that matters when scanning a band: a single-bin carrier
    // must survive being squeezed into a display 8x narrower than the data.
    std::vector<float> src(4096, -110.0f);
    src[2001] = -20.0f;
    std::vector<float> dst(512);
    fernsdr::render_viewport(src.data(), src.size(), -100000.0, 100000.0, -100000.0, 100000.0, dst.data(), dst.size());

    float peak = -200.0f;
    size_t peak_index = 0;
    for (size_t i = 0; i < dst.size(); i++) {
        if (dst[i] > peak) { peak = dst[i]; peak_index = i; }
    }
    CHECK_NEAR(peak, -20.0, 1e-4);
    CHECK_EQ(static_cast<long long>(peak_index), 2001 / 8);
}

TEST_CASE(viewport_zoomed_in_interpolates) {
    std::vector<float> src(1024);
    for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<float>(i);
    std::vector<float> dst(256);
    // Ten source bins stretched across 256 output pixels.
    fernsdr::render_viewport(src.data(), src.size(), 0.0, 1024.0, 100.0, 110.0, dst.data(), dst.size());
    CHECK(dst[0] > 99.0 && dst[0] < 101.0);
    CHECK(dst[255] > 109.0 && dst[255] < 111.0);
    for (size_t i = 1; i < dst.size(); i++) CHECK(dst[i] >= dst[i - 1] - 1e-4);
}

TEST_CASE(viewport_outside_the_data_is_floored) {
    std::vector<float> src(256, -50.0f);
    std::vector<float> dst(64);
    fernsdr::render_viewport(src.data(), src.size(), -1000.0, 1000.0, 5000.0, 6000.0, dst.data(), dst.size());
    for (float v : dst) CHECK_NEAR(v, -160.0, 1e-4);
}

// --- Pyramid ---------------------------------------------------------------

namespace {

// A line with a few narrow carriers on a noise floor: the case where averaging
// instead of peak-holding would lose the signal.
std::vector<float> carrier_line(size_t bins) {
    std::vector<float> line(bins, -110.0f);
    for (size_t i = 0; i < bins; i += 4093) line[i] = -20.0f;
    line[bins / 3] = -6.0f;
    return line;
}

}  // namespace

TEST_CASE(pyramid_matches_full_resolution_when_zoomed_out) {
    const size_t bins = 1 << 16;
    const std::vector<float> line = carrier_line(bins);
    fernsdr::SpectrumPyramid pyramid;
    pyramid.build(line.data(), bins, 0.0, 32e6);

    // Every zoom level from whole-band down to one part in 4096.
    for (int step = 0; step < 12; step++) {
        const double span = 32e6 / static_cast<double>(1 << step);
        const double low = 4e6;
        std::vector<float> from_pyramid(1024), reference(1024);
        pyramid.render(low, low + span, from_pyramid.data(), from_pyramid.size());
        fernsdr::render_viewport(line.data(), bins, 0.0, 32e6, low, low + span, reference.data(),
                                 reference.size());
        for (size_t i = 0; i < reference.size(); i++) {
            // The coarse level's bins are power-of-two aligned and the output
            // pixels are not, so a carrier can land one pixel over. What must
            // never happen is a carrier disappearing, so compare against the
            // reference's immediate neighbourhood.
            float best = -1e9f;
            const size_t from = i > 0 ? i - 1 : 0;
            const size_t to = std::min(reference.size(), i + 2);
            for (size_t j = from; j < to; j++) best = std::max(best, reference[j]);
            CHECK(from_pyramid[i] <= best + 1e-3f);
            CHECK(from_pyramid[i] >= reference[i] - 1e-3f);
        }
    }
}

TEST_CASE(pyramid_reads_far_fewer_bins_than_the_line_holds) {
    const size_t bins = 1 << 21;  // a 32 MHz front end
    const std::vector<float> line = carrier_line(bins);
    fernsdr::SpectrumPyramid pyramid;
    pyramid.build(line.data(), bins, 0.0, 32e6);

    // Whole band into 1500 pixels: the chosen level must be small enough that
    // a listener costs O(width), not O(bins). Anything above ~4x the width
    // would mean the per-listener scan still scales with the front end.
    std::vector<float> dst(1500);
    pyramid.render(0.0, 32e6, dst.data(), dst.size());
    size_t chosen = bins;
    for (size_t level = 0; level < pyramid.levels(); level++) {
        const size_t size = bins >> level;
        if (size <= dst.size() * 2) { chosen = size; break; }
    }
    CHECK(chosen <= dst.size() * 2);
    CHECK(pyramid.levels() > 10);

    // The strongest carrier still shows at full strength after ten halvings.
    float peak = -1e9f;
    for (float v : dst) peak = std::max(peak, v);
    CHECK_NEAR(peak, -6.0, 1e-3);
}

TEST_CASE(pyramid_zoomed_in_uses_the_full_resolution_line) {
    const size_t bins = 4096;
    std::vector<float> line(bins, -110.0f);
    line[2000] = -10.0f;
    line[2001] = -80.0f;
    fernsdr::SpectrumPyramid pyramid;
    pyramid.build(line.data(), bins, 0.0, 4096.0);

    // Two adjacent bins across 512 pixels: at this zoom the pair must stay
    // distinct, which only level 0 can do.
    std::vector<float> dst(512);
    pyramid.render(1998.0, 2004.0, dst.data(), dst.size());
    float low = 1e9f;
    for (float v : dst) low = std::min(low, v);
    CHECK(low < -70.0f);
}

TEST_CASE(pyramid_rebuild_shrinks_with_the_line) {
    fernsdr::SpectrumPyramid pyramid;
    std::vector<float> big(4096, -100.0f);
    pyramid.build(big.data(), big.size(), 0.0, 1000.0);
    const size_t deep = pyramid.levels();
    std::vector<float> small(64, -100.0f);
    pyramid.build(small.data(), small.size(), 0.0, 1000.0);
    CHECK(pyramid.levels() < deep);
    std::vector<float> dst(16);
    pyramid.render(0.0, 1000.0, dst.data(), dst.size());
    for (float v : dst) CHECK_NEAR(v, -100.0, 1e-4);
}

// A line is what a windowed transform of those samples gives, whatever the
// ring's wrap: checked against the transform done by hand, for windows that
// start at the ring's beginning and ones that start a quarter, a half and
// three quarters into it.
TEST_CASE(spectrum_line_is_the_windowed_transform_of_its_samples) {
    const size_t n = 1024;
    const size_t hop = n / 4;
    SpectrumAnalyzer analyzer(256000, n, 256000.0 / hop, 1, 0.0f);
    CHECK_EQ(analyzer.averages(), 1);
    std::vector<cfloat> x(n + 4 * hop);
    uint32_t seed = 12345;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
    };
    for (auto& s : x) s = cfloat(next(), next());
    const auto window = fernsdr::make_blackman_harris_window(n);
    double gain = 0.0;
    for (float w : window) gain += w;
    fernsdr::FftSplit fft(n);
    std::vector<float> line;
    int checked = 0;
    for (size_t pushed = 0; pushed < x.size(); pushed++) {
        analyzer.push(&x[pushed], 1);
        if (!analyzer.take_line(line)) continue;
        const size_t start = pushed + 1 - n;
        std::vector<float> re(n), im(n), power(n), db(n);
        for (size_t e = 0; e < n; e++) {
            re[e] = x[start + e].real() * window[e];
            im[e] = x[start + e].imag() * window[e];
        }
        fft.forward(re.data(), im.data());
        for (size_t e = 0; e < n; e++) power[e] = re[e] * re[e] + im[e] * im[e];
        fernsdr::simd::power_to_db(power.data(), n, 1.0f, static_cast<float>(20.0 * std::log10(gain)), db.data());
        CHECK_EQ(line.size(), n);
        float worst = 0.0f;
        for (size_t i = 0; i < n; i++) worst = std::max(worst, std::fabs(line[i] - db[(i + n / 2) & (n - 1)]));
        CHECK(worst < 1e-3f);
        checked++;
    }
    CHECK_EQ(checked, 5);
}

TEST_CASE(spectrum_pyramid_keeps_a_carrier_in_the_last_bin_of_an_odd_line) {
    // Halving a line of odd length leaves one bin over; a carrier in it must
    // still show at every level the renderer may choose.
    for (size_t bins : {9559u, 1025u, 4097u}) {
        std::vector<float> line(bins, -120.0f);
        line[bins - 1] = -40.0f;
        fernsdr::SpectrumPyramid pyramid;
        pyramid.build(line.data(), bins, 0.0, static_cast<double>(bins));
        for (size_t width : {1000u, 300u, 64u, 16u}) {
            std::vector<float> pixels(width);
            pyramid.render(0.0, static_cast<double>(bins), pixels.data(), width);
            CHECK(*std::max_element(pixels.begin(), pixels.end()) > -41.0f);
        }
    }
}
