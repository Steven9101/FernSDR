#include "../src/codec/nac.h"
#include "test_util.h"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using fernsdr::nac::Decoder;
using fernsdr::nac::Encoder;
using fernsdr::nac::kFrameHop;

TEST_CASE(nac_compact_preserves_pcm_and_never_increases_a_frame) {
    std::mt19937 rng(12045);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    size_t saved = 0, used = 0;
    for (int rate : {8000, 12000, 16000, 44100, 96000}) {
        for (int bitrate : {8000, 24000, 48000, 96000}) {
            Encoder legacy(rate), compact(rate);
            legacy.set_bitrate(bitrate);
            compact.set_bitrate(bitrate);
            Decoder a(rate), b(rate);
            std::vector<float> input(kFrameHop), old_pcm(kFrameHop), new_pcm(kFrameHop);
            for (int frame = 0; frame < 60; frame++) {
                for (size_t i = 0; i < kFrameHop; i++) {
                    const double time = (frame * kFrameHop + i) / static_cast<double>(rate);
                    input[i] = frame < 4 ? 0 : std::sin(time * 4400) * 0.4f + (frame < 20 ? 0 : noise(rng));
                }
                const auto& old = legacy.encode(input.data());
                const auto& next = compact.encode(input.data(), frame % 7 != 0);
                CHECK(next.size() <= old.size());
                CHECK_EQ(legacy.stats().quality_index, compact.stats().quality_index);
                saved += old.size() - next.size();
                used += compact.used_compact();
                if (frame % 13 == 8) {
                    a.conceal(old_pcm.data());
                    b.conceal(new_pcm.data());
                } else {
                    CHECK(a.decode(old.data(), old.size(), old_pcm.data()));
                    CHECK(b.decode(next.data(), next.size(), new_pcm.data(), compact.used_compact()));
                }
                CHECK(old_pcm == new_pcm);
            }
        }
    }
    CHECK(saved > 1000);
    CHECK(used > 100);
}

TEST_CASE(nac_compact_rejects_invalid_masks_exponents_and_selectors) {
    for (int kind = 0; kind < 8; kind++) {
        fernsdr::BitWriter writer;
        writer.put_bits(20, 6);
        if (kind < 3) {
            writer.put_bits(kind == 0 ? 2 : 3, 2);
            if (kind != 0) writer.put_bits(kind == 1 ? 17 : 15, 5);
            writer.put_bits(kind == 0 ? 18 : 3, 5);
        } else {
            writer.put_bits(2, 2);
            writer.put_bits(2, 5);
            writer.put_bits(kind == 3 ? 401 : kind == 7 ? 400 : 160, 9);
            writer.put_bits(kind == 4 ? 6 : kind == 5 ? 7 : 0, 3);
            if (kind == 7) writer.put_signed_exp_golomb(1);
        }
        auto bytes = writer.finish();
        Decoder decoder(12000);
        std::vector<float> out(kFrameHop);
        CHECK(!decoder.decode(bytes.data(), bytes.size(), out.data(), true));
        for (float value : out) CHECK(std::isfinite(value));
    }
    const uint8_t truncated_empty[] = {2};
    Decoder decoder(12000);
    std::vector<float> out(kFrameHop);
    CHECK(!decoder.decode(truncated_empty, 1, out.data(), true));
}

TEST_CASE(nac_compact_all_mask_and_scale_modes_match_legacy) {
    using namespace fernsdr;
    using namespace fernsdr::nac;
    for (int mask = 0; mask < 4; mask++) for (int scale = 0; scale < 6; scale++) {
        BitWriter old, next;
        const int quality = 28;
        old.put_bits(quality, 6);
        next.put_bits(quality, 6);
        next.put_bits(mask, 2);
        bool active[kNumBands];
        for (int b = 0; b < kNumBands; b++) {
            active[b] = mask == 0 ? (b % 3 == 0) : mask == 1 ? true : mask == 2 ? b < 5 : b >= 3 && b < 10;
            old.put_bit(active[b]);
            if (mask == 0) next.put_bit(active[b]);
        }
        if (mask == 2) next.put_bits(5, 5);
        if (mask == 3) { next.put_bits(3, 5); next.put_bits(7, 5); }
        int previous = kExponentReference;
        bool first = true;
        for (int b = 0; b < kNumBands; b++) if (active[b]) {
            const int exponent = -38 + b % 4;
            old.put_signed_exp_golomb(exponent - previous);
            if (first) { next.put_bits(exponent + 200, 9); next.put_bits(scale, 3); }
            else if (scale) next.put_signed_rice(exponent - previous, scale - 1);
            else next.put_signed_exp_golomb(exponent - previous);
            previous = exponent;
            first = false;
        }
        for (int b = 0; b < kNumBands; b++) if (active[b]) for (int i = 0; i < kBandWidths[b]; i++) {
            old.put_signed_rice(i - 2, rice_k_for_quality(quality));
            next.put_signed_rice(i - 2, rice_k_for_quality(quality));
        }
        const auto& a = old.finish();
        const auto& b = next.finish();
        Decoder legacy(12000), compact(12000);
        std::vector<float> old_pcm(kFrameHop), new_pcm(kFrameHop);
        CHECK(legacy.decode(a.data(), a.size(), old_pcm.data()));
        CHECK(compact.decode(b.data(), b.size(), new_pcm.data(), true));
        CHECK(old_pcm == new_pcm);
    }
}

TEST_CASE(nac_minimum_bitrate_covers_the_header_at_high_sample_rates) {
    for (int rate : {44100, 48000, 96000}) {
        Encoder encoder(rate);
        encoder.set_bitrate(8000);
        std::vector<float> silence(kFrameHop, 0);
        const auto& frame = encoder.encode(silence.data());
        CHECK(frame.size() * 8.0 * rate / kFrameHop <= encoder.bitrate());
        CHECK(encoder.bitrate() >= 8000);
    }
}

TEST_CASE(nac_frame_budget_includes_padding_at_fractional_frame_rates) {
    for (int rate : {11025, 12000, 16000, 44100}) {
        Encoder encoder(rate);
        encoder.set_bitrate(16000);
        std::vector<float> samples(kFrameHop);
        for (int frame = 0; frame < 30; frame++) {
            for (size_t i = 0; i < samples.size(); i++) samples[i] = std::sin((i + frame * kFrameHop) * 0.271);
            const auto& payload = encoder.encode(samples.data());
            CHECK(payload.size() * 8.0 * rate / kFrameHop <= 16000);
        }
    }
}

TEST_CASE(nac_bad_source_samples_do_not_poison_later_audio) {
    Encoder encoder(12000);
    Decoder decoder(12000);
    std::vector<float> samples(kFrameHop, std::numeric_limits<float>::infinity());
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    samples[1] = std::numeric_limits<float>::max();
    std::vector<float> out(kFrameHop);
    for (int frame = 0; frame < 5; frame++) {
        const auto& payload = encoder.encode(samples.data());
        CHECK(decoder.decode(payload.data(), payload.size(), out.data()));
        for (float value : out) CHECK(std::isfinite(value));
        std::fill(samples.begin(), samples.end(), 0.0f);
    }
    for (float value : out) CHECK(std::abs(value) < 1e-5f);
}

TEST_CASE(nac_rejects_an_exponent_that_could_overflow_the_decoder) {
    fernsdr::BitWriter writer;
    writer.put_bits(0, fernsdr::nac::kQualityBits);
    writer.put_bit(1);
    for (int i = 1; i < fernsdr::nac::kNumBands; i++) writer.put_bit(0);
    writer.put_signed_exp_golomb(1000000);
    const auto& payload = writer.finish();
    Decoder decoder(12000);
    std::vector<float> out(kFrameHop);
    CHECK(!decoder.decode(payload.data(), payload.size(), out.data()));
    for (float value : out) CHECK(std::isfinite(value));
}

TEST_CASE(nac_and_nac2_refuse_a_coefficient_far_outside_audio) {
    // Quality 0, exponent 200 and an escaped 2^31 - 1: whole, well-formed
    // frames whose one coefficient is some 10^22. Refused and concealed, as
    // NAC3 does, and the frame after starts from silence, not from the
    // overlap such a frame would have left behind.
    using fernsdr::nac::Layout;
    const std::vector<uint8_t> nac = {0x02, 0x00, 0x00, 0x01, 0xe1, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x00};
    const std::vector<uint8_t> nac2 = {0x02, 0x0e, 0x40, 0x7f, 0xff, 0xff, 0xbf, 0xff, 0xff, 0xff, 0x80};
    for (const auto& [payload, layout] : {std::pair{nac, Layout::Original}, std::pair{nac2, Layout::Compact}}) {
        Decoder decoder(12000);
        std::vector<float> out(kFrameHop);
        CHECK(!decoder.decode(payload.data(), payload.size(), out.data(), layout));
        float peak = 0.0f;
        for (float value : out) peak = std::max(peak, std::fabs(value));
        decoder.decode(nullptr, 0, out.data(), layout);
        for (float value : out) peak = std::max(peak, std::fabs(value));
        CHECK(peak < 1.0f);
    }
}

namespace {

struct RoundTrip {
    std::vector<float> decoded;
    double bitrate = 0.0;
    double snr_db = 0.0;
};

// Runs a signal through encode/decode and measures SNR over the steady-state
// portion, skipping the first frames while the overlap buffers prime.
RoundTrip round_trip(const std::vector<float>& signal, int sample_rate, int bitrate,
                     int max_quality = fernsdr::nac::kMaxQualityIndex) {
    Encoder enc(sample_rate);
    enc.set_bitrate(bitrate);
    enc.set_max_quality_index(max_quality);
    Decoder dec(sample_rate);

    RoundTrip rt;
    rt.decoded.resize(signal.size(), 0.0f);
    size_t total_bytes = 0;
    size_t frames = 0;

    for (size_t off = 0; off + kFrameHop <= signal.size(); off += kFrameHop) {
        const auto& packet = enc.encode(signal.data() + off);
        total_bytes += packet.size();
        frames++;
        CHECK(dec.decode(packet.data(), packet.size(), rt.decoded.data() + off));
    }

    if (frames) {
        rt.bitrate = static_cast<double>(total_bytes) * 8.0 * sample_rate / (frames * kFrameHop);
    }

    // The codec delays by one frame (MDCT overlap), so compare shifted.
    double sig = 0.0;
    double err = 0.0;
    const size_t skip = 4 * kFrameHop;
    for (size_t i = skip; i + kFrameHop < signal.size(); i++) {
        const double s = signal[i - kFrameHop];
        const double d = rt.decoded[i];
        sig += s * s;
        err += (s - d) * (s - d);
    }
    rt.snr_db = 10.0 * std::log10(sig / (err + 1e-30));
    return rt;
}

std::vector<float> tone(size_t n, double freq, double sample_rate, double amp = 0.5) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<float>(amp * std::sin(2.0 * M_PI * freq * i / sample_rate));
    return v;
}

}  // namespace

TEST_CASE(nac_band_table_covers_every_coefficient) {
    int total = 0;
    for (int b = 0; b < fernsdr::nac::kNumBands; b++) {
        CHECK_EQ(fernsdr::nac::kBandStarts[b], total);
        total += fernsdr::nac::kBandWidths[b];
    }
    CHECK_EQ(total, static_cast<long long>(fernsdr::nac::kNumCoeffs));
    CHECK_EQ(fernsdr::nac::kBandStarts[fernsdr::nac::kNumBands], static_cast<long long>(fernsdr::nac::kNumCoeffs));
}

TEST_CASE(nac_reconstructs_a_tone_accurately) {
    auto signal = tone(kFrameHop * 60, 1000.0, 12000.0);
    auto rt = round_trip(signal, 12000, 48000);
    CHECK(rt.snr_db > 40.0);
    CHECK(rt.bitrate <= 48000.0);
}

TEST_CASE(nac_respects_the_bitrate_ceiling) {
    // Broadband noise is the worst case for the rate loop: nothing is silent.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> noise(kFrameHop * 80);
    for (auto& s : noise) s = dist(rng);

    double previous_snr = -100.0;
    for (int target : {24000, 32000, 48000, 64000, 96000}) {
        auto rt = round_trip(noise, 12000, target);
        CHECK(rt.bitrate <= target * 1.02);  // padding to the byte boundary only
        CHECK(rt.bitrate > target * 0.9);    // and the budget is actually used
        // Full-bandwidth white noise is the hardest possible case: every one
        // of the 128 coefficients is incompressible, so 24 kbit/s really does
        // buy only ~2 bits each.  What matters is that rate buys distortion
        // back at the expected ~6 dB per bit and nothing collapses.
        CHECK(rt.snr_db > previous_snr + 2.0);
        previous_snr = rt.snr_db;
    }
    CHECK(previous_snr > 25.0);
}

TEST_CASE(nac_preserves_a_weak_tone_beside_a_strong_one) {
    // The property a psychoacoustic codec would fail: a carrier at full scale
    // and a signal 45 dB below it elsewhere in the passband must both survive,
    // because that weak one may be the digital mode the user is decoding.
    const size_t n = kFrameHop * 80;
    const double sr = 12000.0;
    auto signal = tone(n, 800.0, sr, 0.5);
    auto weak = tone(n, 2600.0, sr, 0.5 * std::pow(10.0, -45.0 / 20.0));
    for (size_t i = 0; i < n; i++) signal[i] += weak[i];

    auto rt = round_trip(signal, 12000, 48000);

    // Measure the recovered amplitude at the weak tone's frequency by
    // correlating against a reference, over whole cycles well clear of the edges.
    auto amplitude_at = [&](const std::vector<float>& x, double freq) {
        double re = 0.0, im = 0.0;
        const size_t start = kFrameHop * 8;
        const size_t count = kFrameHop * 60;
        for (size_t i = 0; i < count; i++) {
            const double a = 2.0 * M_PI * freq * (i + start) / sr;
            re += x[i + start] * std::cos(a);
            im += x[i + start] * std::sin(a);
        }
        return 2.0 * std::sqrt(re * re + im * im) / count;
    };

    // decoded is delayed one hop relative to the input
    std::vector<float> aligned(rt.decoded.begin() + kFrameHop, rt.decoded.end());
    const double got = amplitude_at(aligned, 2600.0);
    const double want = 0.5 * std::pow(10.0, -45.0 / 20.0);
    CHECK(got > want * 0.7);
    CHECK(got < want * 1.4);
}

TEST_CASE(nac_silence_is_nearly_free) {
    std::vector<float> silence(kFrameHop * 40, 0.0f);
    auto rt = round_trip(silence, 12000, 48000);
    CHECK(rt.bitrate < 3000.0);
}

TEST_CASE(nac_concealment_keeps_the_sample_clock) {
    // A lost frame must still produce exactly one hop of output: no stretching,
    // no skipping, or anything riding on the audio loses symbol timing.
    Decoder dec(12000);
    std::vector<float> out(kFrameHop, 1.0f);
    dec.conceal(out.data());
    for (float v : out) CHECK(std::isfinite(v));

    Encoder enc(12000);
    auto signal = tone(kFrameHop * 8, 1000.0, 12000.0);
    for (size_t off = 0; off + kFrameHop <= signal.size(); off += kFrameHop) {
        const auto& packet = enc.encode(signal.data() + off);
        CHECK(dec.decode(packet.data(), packet.size(), out.data()));
    }
    // Concealed output decays rather than repeating at full level forever.
    dec.conceal(out.data());
    double first = 0.0;
    for (float v : out) first += v * v;
    dec.conceal(out.data());
    dec.conceal(out.data());
    double later = 0.0;
    for (float v : out) later += v * v;
    CHECK(later < first);
}

TEST_CASE(nac_rejects_truncated_frames_without_reading_out_of_bounds) {
    Encoder enc(12000);
    auto signal = tone(kFrameHop * 4, 1200.0, 12000.0);
    std::vector<uint8_t> packet;
    for (size_t off = 0; off + kFrameHop <= signal.size(); off += kFrameHop) {
        packet = enc.encode(signal.data() + off);
    }
    CHECK(packet.size() > 4);

    Decoder dec(12000);
    std::vector<float> out(kFrameHop);
    CHECK(!dec.decode(packet.data(), packet.size() / 2, out.data()));
    for (float v : out) CHECK(std::isfinite(v));
}

TEST_CASE(nac_typical_ssb_channel_fits_the_bandwidth_budget) {
    // 2.7 kHz of speech-like content in an 8 kHz channel: the case the
    // deployment budget is written against.
    std::mt19937 rng(11);
    std::normal_distribution<float> dist(0.0f, 0.2f);
    const size_t n = kFrameHop * 200;
    std::vector<float> x(n, 0.0f);
    // Band-limited noise: a crude two-pole resonator around 1.2 kHz.
    float y1 = 0.0f, y2 = 0.0f;
    const double w = 2.0 * M_PI * 1200.0 / 8000.0;
    const double r = 0.97;
    for (size_t i = 0; i < n; i++) {
        const float in = dist(rng);
        const float y = in + static_cast<float>(2 * r * std::cos(w)) * y1 - static_cast<float>(r * r) * y2;
        y2 = y1;
        y1 = y;
        x[i] = y * 0.05f;
    }

    auto rt = round_trip(x, 8000, 48000);
    CHECK(rt.bitrate < 48000.0);
    CHECK(rt.snr_db > 15.0);
}

// --- NAC3 -------------------------------------------------------------------

namespace {

// Mean MDCT coefficient energy of white noise with this sample deviation, as
// the encoder's transform sees it: the sine window's energy over M/2.
double white_coefficient_energy(double sigma) { return sigma * sigma * fernsdr::nac::kFrameHop / 2.0; }

double db(double value) { return 10.0 * std::log10(value); }

}  // namespace

TEST_CASE(nac3_noise_estimate_finds_white_noise_through_agc_steps) {
    using namespace fernsdr::nac;
    for (double sigma : {0.001, 0.05, 0.3}) {
        Encoder encoder(12000);
        encoder.set_bitrate(64000);
        Nac3Target target;
        target.passband_low_hz = 300;
        target.passband_high_hz = 2700;
        encoder.set_target(target);
        std::mt19937 rng(static_cast<uint32_t>(sigma * 1e6));
        std::normal_distribution<float> noise(0.0f, static_cast<float>(sigma));
        std::vector<float> hop(kFrameHop);
        Decoder decoder(12000);
        std::vector<float> out(kFrameHop);
        for (int frame = 0; frame < 400; frame++) {
            // The AGC turns the audio up and down; the noise before it does not move.
            const float gain = frame % 100 < 50 ? 1.0f : 16.0f;
            for (float& value : hop) value = noise(rng) * std::sqrt(gain);
            encoder.set_signal_gain(gain);
            const auto& packet = encoder.encode(hop.data(), Layout::PerBand);
            CHECK(decoder.decode(packet.data(), packet.size(), out.data(), Layout::PerBand));
        }
        // Frame 399 was at gain 16: the estimate is reported at that gain.
        (void)encoder;
    }
    // Checked through the estimator directly, where the truth is known.
    for (double sigma : {0.001, 0.05, 0.3}) {
        NoiseEstimate estimate;
        estimate.configure(12000, 300, 2700);
        std::mt19937 rng(7);
        std::normal_distribution<float> noise(0.0f, static_cast<float>(sigma));
        fernsdr::Mdct mdct(kFrameHop);
        const auto window = fernsdr::make_sine_window(2 * kFrameHop);
        std::vector<float> history(kFrameHop, 0.0f), windowed(2 * kFrameHop), coefficients(kNumCoeffs);
        float energies[kNumBands];
        for (int frame = 0; frame < 300; frame++) {
            const float gain = frame % 90 < 45 ? 1.0f : 100.0f;
            for (size_t i = 0; i < kFrameHop; i++) {
                const float sample = noise(rng) * std::sqrt(gain);
                windowed[i] = history[i] * window[i];
                windowed[kFrameHop + i] = sample * window[kFrameHop + i];
                history[i] = sample;
            }
            mdct.forward(windowed.data(), coefficients.data());
            for (int b = 0; b < kNumBands; b++) {
                double e = 0;
                for (int i = kBandStarts[b]; i < kBandStarts[b + 1]; i++) e += coefficients[i] * coefficients[i];
                energies[b] = static_cast<float>(e / kBandWidths[b]);
            }
            estimate.update(coefficients.data(), energies, gain);
            if (frame > 120 && frame % 45 != 0 && frame % 45 != 1) {
                // Within a decibel of the truth at whatever gain this frame had,
                // except right at a gain step, where the window straddles both.
                CHECK_NEAR(db(estimate.passband_noise()), db(white_coefficient_energy(sigma) * gain), 1.0);
            }
        }
    }
}

TEST_CASE(nac3_keeps_a_weak_tone_beside_a_strong_one_in_the_same_band) {
    // The critic's case: a steady tone 20 dB above the channel noise per
    // coefficient and another 15 dB below it, both inside one four-coefficient
    // band, for ten seconds. A per-band minimum would take the strong tone for
    // the noise floor and code the weak one away. The codec error in that band
    // must stay well under the channel noise throughout.
    using namespace fernsdr::nac;
    const int rate = 12000;
    const double sigma = 0.01;
    const double noise_coefficient = white_coefficient_energy(sigma);
    // Band 4 covers 750-937.5 Hz; put both tones there, on coefficient centres.
    const double hz = rate / 2.0 / kNumCoeffs;
    const double strong_hz = (kBandStarts[4] + 0.5) * hz, weak_hz = (kBandStarts[4] + 2.5) * hz;
    // A sinusoid of amplitude A puts about A^2 * M / 4 into its coefficient.
    const double strong = std::sqrt(noise_coefficient * 100.0 * 4.0 / kFrameHop);
    const double weak = strong * std::pow(10.0, -35.0 / 20.0);
    Encoder encoder(rate);
    encoder.set_bitrate(96000);
    Nac3Target target;
    target.passband_low_hz = 300;
    target.passband_high_hz = 2700;
    target.max_snr_db = 48;
    encoder.set_target(target);
    Decoder decoder(rate);
    std::mt19937 rng(99);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(sigma));
    const size_t frames = 10 * rate / kFrameHop;
    std::vector<float> input(frames * kFrameHop), output(frames * kFrameHop);
    for (size_t i = 0; i < input.size(); i++) {
        const double t = static_cast<double>(i) / rate;
        input[i] = static_cast<float>(strong * std::sin(2 * M_PI * strong_hz * t) +
                                      weak * std::sin(2 * M_PI * weak_hz * t)) + noise(rng);
    }
    for (size_t f = 0; f < frames; f++) {
        const auto& packet = encoder.encode(input.data() + f * kFrameHop, Layout::PerBand);
        CHECK(decoder.decode(packet.data(), packet.size(), output.data() + f * kFrameHop, Layout::PerBand));
    }
    // Error in band 4 around t = 5 s, analysed like the encoder analyses.
    fernsdr::Mdct mdct(kFrameHop);
    const auto window = fernsdr::make_sine_window(2 * kFrameHop);
    std::vector<float> windowed(2 * kFrameHop), coefficients(kNumCoeffs);
    double error = 0.0;
    int counted = 0;
    for (size_t f = frames / 2 - 40; f < frames / 2 + 40; f++) {
        for (size_t i = 0; i < 2 * kFrameHop; i++) {
            const size_t n = (f - 1) * kFrameHop + i;
            windowed[i] = (output[n + kFrameHop] - input[n]) * window[i];
        }
        mdct.forward(windowed.data(), coefficients.data());
        for (int i = kBandStarts[4]; i < kBandStarts[5]; i++) error += coefficients[i] * coefficients[i];
        counted += kBandWidths[4];
    }
    // Twelve decibels are the target; nine leave room for the estimate's bias.
    CHECK(db(error / counted) < db(noise_coefficient) - 9.0);
}

TEST_CASE(nac3_remembers_the_noise_heard_in_a_transmission_pause) {
    // Crowded digital segments: strong signals on every passband coefficient
    // for 12.6 s of every 15, which lifts every percentile of the pool. The
    // stations pause together, and the estimate must remember that pause.
    using namespace fernsdr::nac;
    const int rate = 12000;
    NoiseEstimate estimate;
    estimate.configure(rate, 300, 2700);
    std::mt19937 rng(3);
    const double sigma = 0.02;
    std::normal_distribution<float> noise(0.0f, static_cast<float>(sigma));
    fernsdr::Mdct mdct(kFrameHop);
    const auto window = fernsdr::make_sine_window(2 * kFrameHop);
    std::vector<float> history(kFrameHop, 0.0f), windowed(2 * kFrameHop), coefficients(kNumCoeffs);
    float energies[kNumBands];
    const size_t per_second = rate / kFrameHop;
    for (size_t frame = 0; frame < 30 * per_second; frame++) {
        const double second = std::fmod(static_cast<double>(frame) / per_second, 15.0);
        const bool transmitting = second > 0.5 && second < 13.1;
        for (size_t i = 0; i < kFrameHop; i++) {
            const double t = static_cast<double>(frame * kFrameHop + i) / rate;
            double sample = noise(rng);
            if (transmitting) {
                // A tone every 50 Hz across the passband, 20 dB over the noise.
                for (double f = 300; f < 2700; f += 50) sample += 0.03 * std::sin(2 * M_PI * f * t + f);
            }
            windowed[i] = history[i] * window[i];
            windowed[kFrameHop + i] = static_cast<float>(sample) * window[kFrameHop + i];
            history[i] = static_cast<float>(sample);
        }
        mdct.forward(windowed.data(), coefficients.data());
        for (int b = 0; b < kNumBands; b++) {
            double e = 0;
            for (int i = kBandStarts[b]; i < kBandStarts[b + 1]; i++) e += coefficients[i] * coefficients[i];
            energies[b] = static_cast<float>(e / kBandWidths[b]);
        }
        estimate.update(coefficients.data(), energies, 1.0f);
        // After the first pause, the second transmission must not fool it.
        if (static_cast<double>(frame) / per_second > 19.0 && static_cast<double>(frame) / per_second < 27.0) {
            CHECK(db(estimate.passband_noise()) < db(white_coefficient_energy(sigma)) + 1.5);
        }
    }
}

TEST_CASE(nac3_costs_almost_nothing_for_silence_and_little_for_noise) {
    using namespace fernsdr::nac;
    Encoder encoder(12000);
    encoder.set_bitrate(48000);
    Nac3Target target;
    target.passband_low_hz = 300;
    target.passband_high_hz = 2700;
    encoder.set_target(target);
    std::vector<float> hop(kFrameHop, 0.0f);
    size_t bytes = 0;
    for (int frame = 0; frame < 100; frame++) bytes += encoder.encode(hop.data(), Layout::PerBand).size();
    CHECK(bytes <= 200);  // nine bits a frame: the packet's frame count and an empty mask

    // Noise the way a receiver delivers it: filtered to the passband. A dense
    // multisine with random phases is Gaussian enough for the codec.
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> phase(0.0, 2 * M_PI);
    std::vector<double> phases;
    for (double f = 300; f < 2700; f += 7.3) phases.push_back(phase(rng));
    bytes = 0;
    for (int frame = 0; frame < 400; frame++) {
        for (size_t i = 0; i < kFrameHop; i++) {
            const double t = static_cast<double>(frame * kFrameHop + i) / 12000.0;
            double sample = 0.0;
            size_t k = 0;
            for (double f = 300; f < 2700; f += 7.3) sample += 0.01 * std::sin(2 * M_PI * f * t + phases[k++]);
            hop[i] = static_cast<float>(sample);
        }
        const auto& packet = encoder.encode(hop.data(), Layout::PerBand);
        if (frame >= 200) bytes += packet.size();
    }
    // Noise 12 dB under the codec needs about two bits per passband
    // coefficient: far less than the 48 kbit/s ceiling.
    const double kbps = bytes * 8.0 * 12000.0 / kFrameHop / 200 / 1000.0;
    if (!(kbps < 28.0)) fprintf(stderr, "noise rate %.2f kbit/s\n", kbps);
    CHECK(kbps < 28.0);
}

TEST_CASE(nac3_packets_respect_the_ceiling_and_decode_whole) {
    using namespace fernsdr::nac;
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    for (int rate : {8000, 12000, 15625, 48000}) for (int bitrate : {9000, 16000, 48000}) {
        Encoder encoder(rate);
        encoder.set_bitrate(bitrate);
        Decoder decoder(rate);
        const int target_bits = static_cast<int>(encoder.bitrate() / encoder.frame_rate()) / 8 * 8;
        std::vector<float> hops(kFrameHop * kMaxPacketFrames), out(kFrameHop * kMaxPacketFrames);
        for (int packet = 0; packet < 40; packet++) {
            const int frames = packet % kMaxPacketFrames + 1;
            encoder.begin_packet();
            for (int f = 0; f < frames; f++) {
                for (size_t i = 0; i < kFrameHop; i++) hops[f * kFrameHop + i] = noise(rng);
                encoder.add_frame(hops.data() + f * kFrameHop);
                CHECK(encoder.stats().payload_bits <= target_bits);
            }
            const auto bytes = encoder.finish_packet();
            CHECK(static_cast<int>(bytes.size()) * 8 <= frames * target_bits + 7);
            bool ok = false;
            CHECK_EQ(decoder.decode_packet(bytes.data(), bytes.size(), out.data(), kMaxPacketFrames, ok), frames);
            CHECK(ok);
            for (int i = 0; i < frames * static_cast<int>(kFrameHop); i++) CHECK(std::isfinite(out[i]));
        }
    }
}

TEST_CASE(nac3_rejects_hostile_packets_and_conceals) {
    using namespace fernsdr::nac;
    Decoder decoder(12000);
    std::vector<float> out(kFrameHop * kMaxPacketFrames);
    bool ok = true;
    const uint8_t four_frames[] = {0xC0};
    CHECK_EQ(decoder.decode_packet(four_frames, 1, out.data(), 2, ok), 0);  // more frames than room
    CHECK_EQ(decoder.decode_packet(four_frames, 0, out.data(), 4, ok), 0);  // nothing at all
    // One band whose only coefficient is an escaped 2^31 at the largest step:
    // far outside audio, so the frame is refused and concealed.
    fernsdr::BitWriter writer;
    writer.put_bits(0, 2);
    writer.put_bits(3, 2);
    writer.put_bits(0, 5);
    writer.put_bits(1, 5);
    writer.put_bits(kMaxStep - kMinStep, 9);
    writer.put_bits(0, 3);
    writer.put_bits(0, 4);
    writer.put_rice(0x7FFFFFFFu, 0);
    const auto hostile = writer.finish();
    std::fill(out.begin(), out.end(), std::nanf(""));
    CHECK_EQ(decoder.decode_packet(hostile.data(), hostile.size(), out.data(), 4, ok), 1);
    CHECK(!ok);
    for (size_t i = 0; i < kFrameHop; i++) CHECK(std::isfinite(out[i]));
}

TEST_CASE(nac_never_codes_a_frame_its_own_decoder_refuses) {
    // The encoder bounds its input and the decoder refuses coefficients far
    // outside audio as corrupt. Input at and past the bound must still come
    // out as frames the decoder accepts, in every layout.
    using namespace fernsdr::nac;
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    for (float amplitude : {1e3f, 3e5f, 1e6f, 1e9f}) {
        for (Layout layout : {Layout::Original, Layout::Compact, Layout::PerBand}) {
            Encoder encoder(12000);
            encoder.set_bitrate(64000);
            Decoder decoder(12000);
            std::vector<float> input(kFrameHop), out(kFrameHop * kMaxPacketFrames);
            int refused = 0;
            for (int frame = 0; frame < 40; frame++) {
                for (size_t i = 0; i < kFrameHop; i++) {
                    const double time = (frame * kFrameHop + i) / 12000.0;
                    input[i] = amplitude * (frame % 2 ? noise(rng) : static_cast<float>(std::sin(time * 6000)));
                }
                bool ok = false;
                if (layout == Layout::PerBand) {
                    encoder.begin_packet();
                    encoder.add_frame(input.data());
                    const auto& packet = encoder.finish_packet();
                    decoder.decode_packet(packet.data(), packet.size(), out.data(), kMaxPacketFrames, ok);
                } else {
                    const auto& frame_bytes = encoder.encode(input.data(), layout);
                    ok = decoder.decode(frame_bytes.data(), frame_bytes.size(), out.data(), layout);
                }
                if (!ok) refused++;
            }
            CHECK_EQ(refused, 0);
        }
    }
}
