#include "../src/dsp/channelizer.h"
#include "test_util.h"

#include <cmath>
#include <vector>

using fernsdr::cfloat;
using fernsdr::Channel;
using fernsdr::Channelizer;

namespace {

constexpr double kSampleRate = 192000.0;
constexpr size_t kFftSize = 4096;
constexpr size_t kIfftSize = 256;  // -> 12 kHz output

struct Capture {
    std::vector<cfloat> samples;
    double rate = 0.0;
};

// Feeds a complex tone through the channelizer and returns the channel output,
// dropping the blocks needed to prime the overlap history.
Capture run_tone(double tone_hz, double amplitude, double center_hz, double low_hz, double high_hz,
                 size_t blocks = 24) {
    Channelizer ch(kSampleRate, kFftSize);
    Channel chan(ch, kIfftSize);
    chan.set_passband(center_hz, low_hz, high_hz);

    Capture cap;
    cap.rate = chan.output_rate();

    std::vector<cfloat> input(ch.block_size());
    std::vector<cfloat> out(chan.output_per_block());
    size_t n = 0;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < input.size(); i++, n++) {
            const double a = 2.0 * M_PI * tone_hz * static_cast<double>(n) / kSampleRate;
            input[i] = cfloat(static_cast<float>(amplitude * std::cos(a)),
                              static_cast<float>(amplitude * std::sin(a)));
        }
        ch.process(input.data());
        chan.pull(ch, out.data());
        if (b >= 4) cap.samples.insert(cap.samples.end(), out.begin(), out.end());
    }
    return cap;
}

// Amplitude of `x` at baseband frequency `freq`, by correlation.
double amplitude_at(const std::vector<cfloat>& x, double freq, double rate) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < x.size(); i++) {
        const double a = -2.0 * M_PI * freq * static_cast<double>(i) / rate;
        const double c = std::cos(a), s = std::sin(a);
        re += x[i].real() * c - x[i].imag() * s;
        im += x[i].real() * s + x[i].imag() * c;
    }
    return std::sqrt(re * re + im * im) / x.size();
}

double rms(const std::vector<cfloat>& x) {
    double acc = 0.0;
    for (auto& v : x) acc += static_cast<double>(v.real()) * v.real() + static_cast<double>(v.imag()) * v.imag();
    return std::sqrt(acc / x.size());
}

}  // namespace

TEST_CASE(channelizer_output_rate_follows_the_transform_ratio) {
    Channelizer ch(kSampleRate, kFftSize);
    Channel chan(ch, kIfftSize);
    CHECK_NEAR(chan.output_rate(), 12000.0, 1e-6);
    CHECK_EQ(static_cast<long long>(chan.output_per_block()), 128);
    CHECK_EQ(static_cast<long long>(ch.block_size()), 2048);
    // One block of input and one block of output must cover the same wall time.
    CHECK_NEAR(ch.block_seconds(), 128.0 / 12000.0, 1e-9);
}

TEST_CASE(channelizer_preserves_amplitude_of_a_centred_tone) {
    // A tone exactly on the tuning point must come out at DC with its
    // amplitude intact: this pins the 1/K scaling through both transforms.
    const double bin = kSampleRate / kFftSize;
    const double tone = 40.0 * bin;  // exactly on a bin
    auto cap = run_tone(tone, 0.25, tone, -1500.0, 1500.0);
    CHECK_NEAR(amplitude_at(cap.samples, 0.0, cap.rate), 0.25, 0.01);
}

TEST_CASE(channelizer_translates_an_offset_tone_correctly) {
    const double bin = kSampleRate / kFftSize;
    const double center = 40.0 * bin;
    auto cap = run_tone(center + 500.0, 0.25, center, -2000.0, 2000.0);
    CHECK_NEAR(amplitude_at(cap.samples, 500.0, cap.rate), 0.25, 0.01);
    CHECK_NEAR(amplitude_at(cap.samples, -500.0, cap.rate), 0.0, 0.005);
}

TEST_CASE(channelizer_tunes_between_bins_exactly) {
    // Sub-bin tuning is what makes fine tuning usable; without the time-domain
    // rotation the tone would sit up to half a bin off DC.
    const double bin = kSampleRate / kFftSize;
    const double center = 40.0 * bin + 0.37 * bin;  // deliberately off-grid
    auto cap = run_tone(center, 0.25, center, -1500.0, 1500.0);
    CHECK_NEAR(amplitude_at(cap.samples, 0.0, cap.rate), 0.25, 0.01);
}

TEST_CASE(channelizer_rejects_signals_outside_the_passband) {
    const double bin = kSampleRate / kFftSize;
    const double center = 60.0 * bin;

    auto in_band = run_tone(center + 1000.0, 0.25, center, -2000.0, 2000.0);
    auto out_of_band = run_tone(center + 6000.0, 0.25, center, -2000.0, 2000.0);

    const double rejection_db = 20.0 * std::log10(rms(in_band.samples) / (rms(out_of_band.samples) + 1e-20));
    CHECK(rejection_db > 60.0);
}

TEST_CASE(channelizer_filter_edges_describe_half_gain_cutoffs) {
    for (const double center : {2812.5, 2830.0}) {
        for (const double sign : {-1.0, 1.0}) {
            const double low = sign > 0 ? 300.0 : -2700.0;
            const double high = sign > 0 ? 2700.0 : -300.0;
            const auto middle = run_tone(center + sign * 1500, 0.25, center, low, high);
            for (double edge : {low, high}) {
                const auto at_edge = run_tone(center + edge, 0.25, center, low, high);
                CHECK_NEAR(rms(at_edge.samples) / rms(middle.samples), 0.5, 0.08);
            }
        }
    }
}

TEST_CASE(channelizer_single_sideband_passband_is_asymmetric) {
    // USB: energy below the tuning point must be rejected, above it passed.
    const double bin = kSampleRate / kFftSize;
    const double center = 50.0 * bin;

    auto upper = run_tone(center + 1500.0, 0.25, center, 300.0, 2700.0);
    auto lower = run_tone(center - 1500.0, 0.25, center, 300.0, 2700.0);

    CHECK(rms(upper.samples) > 0.2);
    const double rejection_db = 20.0 * std::log10(rms(upper.samples) / (rms(lower.samples) + 1e-20));
    CHECK(rejection_db > 60.0);
}

TEST_CASE(channelizer_retuning_stays_phase_continuous) {
    // Retuning must not produce a discontinuity that would click in the audio.
    Channelizer ch(kSampleRate, kFftSize);
    Channel chan(ch, kIfftSize);
    const double bin = kSampleRate / kFftSize;
    chan.set_passband(40.0 * bin, -2000.0, 2000.0);

    std::vector<cfloat> input(ch.block_size());
    std::vector<cfloat> out(chan.output_per_block());
    std::vector<cfloat> tail;
    size_t n = 0;
    for (size_t b = 0; b < 20; b++) {
        for (size_t i = 0; i < input.size(); i++, n++) {
            const double a = 2.0 * M_PI * (40.0 * bin) * static_cast<double>(n) / kSampleRate;
            input[i] = cfloat(static_cast<float>(0.25 * std::cos(a)), static_cast<float>(0.25 * std::sin(a)));
        }
        ch.process(input.data());
        if (b == 12) chan.set_passband(40.0 * bin, -1200.0, 1200.0);  // bandwidth change only
        chan.pull(ch, out.data());
        if (b >= 14) tail.insert(tail.end(), out.begin(), out.end());
    }
    // Still a clean DC tone at full amplitude after the change.
    CHECK_NEAR(amplitude_at(tail, 0.0, chan.output_rate()), 0.25, 0.01);
}

TEST_CASE(channelizer_fine_tuning_matches_direct_rotation_through_retunes) {
    for (auto kind : {fernsdr::SignalKind::Iq, fernsdr::SignalKind::Real}) {
        for (size_t length : {size_t{8}, size_t{256}, size_t{4096}}) {
            Channelizer parent(kSampleRate, kFftSize, kind);
            Channel channel(parent, length);
            Channel reference(parent, length);
            std::vector<cfloat> input(parent.block_size());
            std::vector<float> real_input(parent.block_size());
            std::vector<cfloat> actual(channel.output_per_block());
            std::vector<cfloat> expected(actual.size());
            const double centers[] = {40.49, -80.49, 2047.25, -2047.4, 0.17};
            const double width = channel.output_rate() * 0.18;
            double phase = 0.0;
            double step = 0.0;
            double peak = 0.0;
            uint32_t random = 42;
            for (size_t block = 0; block < 160; block++) {
                if (block % 31 == 0) {
                    const double center = centers[(block / 31) % 5] * parent.bin_hz();
                    const double coarse = std::round(center / parent.bin_hz()) * parent.bin_hz();
                    const double residual = center - coarse;
                    channel.set_passband(center, -width, width);
                    // The reference has the same spectral slice and mask,
                    // but leaves fine tuning to direct trigonometry below.
                    reference.set_passband(coarse, -width + residual, width + residual);
                    step = -2.0 * M_PI * residual / channel.output_rate();
                }
                for (size_t i = 0; i < input.size(); i++) {
                    random = random * 1664525u + 1013904223u;
                    const float re = static_cast<float>(random >> 8) / 16777216.0f - 0.5f;
                    random = random * 1664525u + 1013904223u;
                    const float im = static_cast<float>(random >> 8) / 16777216.0f - 0.5f;
                    input[i] = cfloat(re, im);
                    real_input[i] = re;
                }
                if (kind == fernsdr::SignalKind::Real) parent.process_real(real_input.data());
                else parent.process(input.data());
                channel.pull(parent, actual.data());
                reference.pull(parent, expected.data());
                double error = 0.0;
                for (size_t i = 0; i < actual.size(); i++) {
                    const cfloat rotation(static_cast<float>(std::cos(phase)),
                                          static_cast<float>(std::sin(phase)));
                    error = std::max(error, static_cast<double>(std::abs(actual[i] - expected[i] * rotation)));
                    peak = std::max(peak, static_cast<double>(std::abs(actual[i])));
                    phase += step;
                    if (phase < -2.0 * M_PI) phase += 2.0 * M_PI;
                    if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
                }
                CHECK(error < 2e-6);
            }
            CHECK(peak > 1e-3);
        }
    }
}

// --- real-input front ends --------------------------------------------------

namespace {

// A real front end covering 0 - 1.024 MHz. The wideband case (an RX-888 at
// 64 Msps) is the same code with a larger transform; it is measured by
// tools/benchmark rather than here, because a 2^20-point FFT does not belong
// in a unit test.
//
// The ratio that matters is bins per channel: the channelizer can only carve
// out a channel several bins wide, so the transform must be sized to give
// roughly 50 Hz bins whatever the input rate. At 64 Msps that means K = 2^20.
constexpr double kRealRate = 2048000.0;
constexpr size_t kRealFft = 32768;   // 62.5 Hz bins
constexpr size_t kRealIfft = 256;    // -> 16 kHz channel

// Feeds a real cosine through a real-input channelizer.
Capture run_real_tone(double tone_hz, double amplitude, double center_hz, double low_hz,
                      double high_hz, size_t blocks = 24) {
    Channelizer ch(kRealRate, kRealFft, fernsdr::SignalKind::Real);
    Channel chan(ch, kRealIfft);
    chan.set_passband(center_hz, low_hz, high_hz);

    Capture cap;
    cap.rate = chan.output_rate();

    std::vector<float> input(ch.block_size());
    std::vector<cfloat> out(chan.output_per_block());
    size_t n = 0;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < input.size(); i++, n++) {
            input[i] = static_cast<float>(
                amplitude * std::cos(2.0 * M_PI * tone_hz * static_cast<double>(n) / kRealRate));
        }
        ch.process_real(input.data());
        chan.pull(ch, out.data());
        if (b >= 6) cap.samples.insert(cap.samples.end(), out.begin(), out.end());
    }
    return cap;
}

}  // namespace

TEST_CASE(channelizer_real_input_recovers_a_tone_at_full_amplitude) {
    // A real cosine has half its energy at -f. Keeping only the positive bins
    // and doubling them is the analytic signal, so the recovered amplitude
    // must equal the cosine's amplitude, not half of it.
    const double bin = kRealRate / kRealFft;
    const double tone = 8000.0 * bin;  // 500 kHz, well clear of DC and Nyquist
    auto cap = run_real_tone(tone, 0.25, tone, -3000.0, 3000.0);
    CHECK_NEAR(amplitude_at(cap.samples, 0.0, cap.rate), 0.25, 0.02);
}

TEST_CASE(channelizer_real_input_translates_an_offset_tone) {
    const double bin = kRealRate / kRealFft;
    const double center = 8000.0 * bin;
    auto cap = run_real_tone(center + 1000.0, 0.25, center, -3000.0, 3000.0);
    CHECK_NEAR(amplitude_at(cap.samples, 1000.0, cap.rate), 0.25, 0.02);
    // And nothing at the image frequency: a real input must not fold.
    CHECK_NEAR(amplitude_at(cap.samples, -1000.0, cap.rate), 0.0, 0.01);
}

TEST_CASE(channelizer_real_input_has_no_negative_frequency_image) {
    // The property that distinguishes a correct real front end from one that
    // simply pretends the samples are I with Q = 0: tuning below the signal
    // must not pick up its mirror.
    const double bin = kRealRate / kRealFft;
    const double tone = 8000.0 * bin;

    auto wanted = run_real_tone(tone, 0.25, tone - 2000.0, 500.0, 4000.0);
    // Tune the same distance the other side, where only an image could appear.
    auto image = run_real_tone(tone, 0.25, tone + 2000.0, 500.0, 4000.0);

    const double rejection_db = 20.0 * std::log10(rms(wanted.samples) / (rms(image.samples) + 1e-20));
    CHECK(rejection_db > 55.0);
}

TEST_CASE(channelizer_real_input_covers_dc_to_nyquist) {
    const double bin = kRealRate / kRealFft;
    // Near the bottom of the range, where a naive slice would wrap into the
    // zeroed upper half.
    auto low = run_real_tone(300.0 * bin, 0.25, 300.0 * bin, -3000.0, 3000.0);
    CHECK_NEAR(amplitude_at(low.samples, 0.0, low.rate), 0.25, 0.03);

    // Near the top, just below Nyquist.
    auto high = run_real_tone(16100.0 * bin, 0.25, 16100.0 * bin, -3000.0, 3000.0);
    CHECK_NEAR(amplitude_at(high.samples, 0.0, high.rate), 0.25, 0.03);
}

TEST_CASE(channelizer_skipped_blocks_leave_the_next_transform_as_if_run) {
    // Nothing but the block index carries over between blocks, so a band
    // that skips the transform while nobody listens gets the same spectrum
    // from the next pair as one that transformed every block.
    Channelizer every(kSampleRate, kFftSize), skipping(kSampleRate, kFftSize);
    std::vector<std::vector<cfloat>> blocks(6, std::vector<cfloat>(every.block_size()));
    uint32_t state = 12345;
    for (auto& block : blocks)
        for (auto& s : block) {
            state = state * 1664525u + 1013904223u;
            const float re = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            state = state * 1664525u + 1013904223u;
            s = cfloat(re, static_cast<float>(state >> 8) / 16777216.0f - 0.5f);
        }
    for (size_t b = 1; b < blocks.size(); b++) {
        every.process_pair(blocks[b - 1].data(), blocks[b].data());
        if (b + 1 < blocks.size()) skipping.skip_block();
        else skipping.process_pair(blocks[b - 1].data(), blocks[b].data());
    }
    CHECK_EQ(every.block_index(), skipping.block_index());
    bool same = true;
    for (size_t i = 0; i < kFftSize; i++)
        same = same && every.spectrum_re()[i] == skipping.spectrum_re()[i] &&
               every.spectrum_im()[i] == skipping.spectrum_im()[i];
    CHECK(same);
}

