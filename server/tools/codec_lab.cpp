// What the audio codec costs, and what it changes, on real recordings.
//
// Codec work was judged on one synthetic voice fixture and on whole-signal
// SNR. That rewards spending bits on the channel's own noise, which nobody
// can hear or decode, and it hides the case that matters on a crowded band:
// a weak signal sharing a codec band with a strong one. This tool runs real
// off-air IQ through the same receive chain a listener has, then reports for
// every codec setting:
//
//   payload / framed  kbit/s of NAC frames, and with the 4-byte audio header
//                     and WebSocket framing a browser actually receives
//   snr               waveform SNR against the codec's own input
//   rise              how far the codec raises the noise floor: for every
//                     band and frame, 10*log10((N + D) / N), where N is the
//                     recording's local noise floor in that band and D the
//                     codec error there. 0 dB means indistinguishable from the
//                     receiver's own noise; +3 dB means the codec doubled it.
//
// The noise floor is a local low percentile of each band's energy, so it
// follows the AGC. It is an estimate; it is used to compare codec settings
// against each other, not as an absolute calibration.
//
//   codec-lab --iq capture.wav --mode usb [--offset 0] [--rates 16000,32000]
//   codec-lab --audio ft8.wav [--write-dir /tmp/out]
//
// With --write-dir the decoded audio of every row is written as a WAV, so an
// external decoder (jt9, decode_ft8) can count what survived.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/codec/nac.h"
#include "../src/dsp/agc.h"
#include "../src/dsp/channel_noise.h"
#include "../src/dsp/channelizer.h"
#include "../src/dsp/cw_filter.h"
#include "../src/dsp/demod.h"
#include "../src/dsp/mdct.h"
#include "wav.h"

using namespace fernsdr;

namespace {

struct Options {
    std::string iq_path;
    std::string audio_path;
    std::string mode_name = "usb";
    double offset_hz = 0.0;
    int requested_rate = 12000;
    double seconds = 0.0;  // 0: whole file
    std::vector<int> rates = {16000, 24000, 32000, 48000, 64000};
    std::vector<std::string> variants = {"nac2"};
    std::string write_dir;
    std::string label;
    bool show_bands = false;
    std::string truth_path;  // noise-only companion of --audio, for --noise-study
    bool noise_study = false;
    int bench = 0;  // encode the input this many times per row and time it
    // Audio passband handed to NAC3, Hz. Negative: derive from the mode.
    float passband_low = -1.0f;
    float passband_high = -1.0f;
};

std::vector<int> parse_rates(const std::string& text) {
    std::vector<int> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        out.push_back(std::atoi(text.substr(start, comma - start).c_str()));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::vector<std::string> parse_list(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        out.push_back(text.substr(start, comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// The receive chain a listener has, minus the network: channel slice, narrow
// CW selector, AGC, demodulator. Band input is two-channel 16-bit IQ.
std::vector<float> demodulate(const wav::Audio& iq, Mode mode, double offset_hz, int requested_rate,
                              int& audio_rate, std::vector<float>& power_gain) {
    const double rate = iq.rate;
    size_t fft_size = 1024;
    while (fft_size * 2 <= rate / 50.0 && fft_size < (1u << 20)) fft_size *= 2;
    size_t decimation = 1;
    while (decimation * 2 <= static_cast<size_t>(rate / requested_rate * 1.4142) &&
           decimation < fft_size / 16) {
        decimation *= 2;
    }
    const size_t ifft_size = std::max<size_t>(16, fft_size / decimation);

    Channelizer channelizer(rate, fft_size, SignalKind::Iq);
    Channel channel(channelizer, ifft_size);
    const double output_rate = channel.output_rate();
    audio_rate = static_cast<int>(std::lround(output_rate));
    const Passband passband = default_passband(mode, 700.0);
    const bool narrow_cw = (mode == Mode::Cw || mode == Mode::CwL) && passband.high - passband.low <= 1000;
    channel.set_passband(offset_hz, narrow_cw ? -output_rate / 2 : passband.low,
                         narrow_cw ? output_rate / 2 : passband.high);
    CwFilter selector;
    selector.configure(output_rate, passband.low, passband.high, narrow_cw);
    Agc agc;
    agc.configure(output_rate);
    // As a listener sets it up (Listener::apply): no gain control for NFM,
    // and AM and SAM following their carrier.
    agc.set_profile(mode == Mode::Nfm ? AgcProfile::Off : AgcProfile::Slow);
    agc.set_follow_carrier(mode == Mode::Am || mode == Mode::Sam);
    // And told the noise around the channel, as a listener's is.
    ChannelNoise noise;
    const double noise_width = narrow_cw ? passband.high - passband.low : channel.noise_bandwidth_hz();
    Demodulator demodulator;
    demodulator.configure(mode, output_rate);

    const size_t block = channelizer.block_size();
    std::vector<cfloat> input(block);
    std::vector<cfloat> baseband(channel.output_per_block());
    std::vector<float> audio(channel.output_per_block());
    std::vector<float> out;
    const size_t frames = iq.frames();
    for (size_t offset = 0; offset + block <= frames; offset += block) {
        for (size_t i = 0; i < block; i++) {
            input[i] = cfloat(iq.samples[2 * (offset + i)], iq.samples[2 * (offset + i) + 1]);
        }
        channelizer.process(input.data());
        channel.pull(channelizer, baseband.data());
        selector.process(baseband.data(), baseband.size());
        if (agc.profile() != AgcProfile::Off) {
            noise.update(channelizer.current_block(), offset_hz + 0.5 * (passband.low + passband.high), false);
            agc.set_noise_power(noise.channel_power(noise_width));
        }
        agc.process(baseband.data(), baseband.size());
        demodulator.process(baseband.data(), baseband.size(), audio.data());
        out.insert(out.end(), audio.begin(), audio.end());
        // What the listener hands the encoder: the AGC's gain after this block.
        power_gain.insert(power_gain.end(), audio.size(), std::pow(10.0f, agc.gain_db() / 10.0f));
    }
    return out;
}

// Mean energy per coefficient in each NAC band, one row per 128-sample hop.
// Frame f analyses samples [128*(f-1), 128*(f+1)), the same window the encoder
// uses for its frame f.
std::vector<std::vector<float>> band_energies(const std::vector<float>& signal, size_t frames) {
    Mdct mdct(nac::kFrameHop);
    const auto window = make_sine_window(2 * nac::kFrameHop);
    std::vector<float> windowed(2 * nac::kFrameHop), coefficients(nac::kNumCoeffs);
    std::vector<std::vector<float>> out(frames, std::vector<float>(nac::kNumBands, 0.0f));
    for (size_t f = 0; f < frames; f++) {
        for (size_t i = 0; i < 2 * nac::kFrameHop; i++) {
            const long index = static_cast<long>(f * nac::kFrameHop + i) - static_cast<long>(nac::kFrameHop);
            const float value = index >= 0 && static_cast<size_t>(index) < signal.size() ? signal[index] : 0.0f;
            windowed[i] = value * window[i];
        }
        mdct.forward(windowed.data(), coefficients.data());
        for (int b = 0; b < nac::kNumBands; b++) {
            double energy = 0.0;
            for (int i = 0; i < nac::kBandWidths[b]; i++) {
                const double v = coefficients[nac::kBandStarts[b] + i];
                energy += v * v;
            }
            out[f][b] = static_cast<float>(energy / nac::kBandWidths[b]);
        }
    }
    return out;
}

// Local noise floor per band: the 15th percentile of the five-frame average
// energy within about a second either side, scaled by a fixed bias factor.
// A low percentile rather than the minimum, because a four-coefficient band's
// energy fluctuates wildly frame to frame; local rather than global, because
// the AGC moves the floor whenever a strong signal comes and goes.
std::vector<std::vector<float>> noise_floor(const std::vector<std::vector<float>>& energy, double frame_rate) {
    const size_t frames = energy.size();
    const long half_window = static_cast<long>(std::lround(frame_rate * 1.0));
    std::vector<std::vector<float>> smoothed(frames, std::vector<float>(nac::kNumBands, 0.0f));
    for (size_t f = 0; f < frames; f++) {
        for (int b = 0; b < nac::kNumBands; b++) {
            double sum = 0.0;
            int count = 0;
            for (long d = -2; d <= 2; d++) {
                const long g = static_cast<long>(f) + d;
                if (g < 0 || g >= static_cast<long>(frames)) continue;
                sum += energy[g][b];
                count++;
            }
            smoothed[f][b] = static_cast<float>(sum / std::max(1, count));
        }
    }
    std::vector<std::vector<float>> floor(frames, std::vector<float>(nac::kNumBands, 0.0f));
    std::vector<float> window;
    // Evaluate on a stride and hold, which keeps this fast on long captures
    // without changing the answer: the floor moves over seconds, not frames.
    const size_t stride = 8;
    for (size_t f = 0; f < frames; f += stride) {
        const long lo = std::max<long>(0, static_cast<long>(f) - half_window);
        const long hi = std::min<long>(static_cast<long>(frames) - 1, static_cast<long>(f) + half_window);
        for (int b = 0; b < nac::kNumBands; b++) {
            window.clear();
            for (long g = lo; g <= hi; g++) window.push_back(smoothed[g][b]);
            const size_t k = window.size() * 15 / 100;
            std::nth_element(window.begin(), window.begin() + k, window.end());
            const float value = window[k] * 1.3f;
            for (size_t g = f; g < std::min(frames, f + stride); g++) floor[g][b] = value;
        }
    }
    return floor;
}

struct Row {
    std::string variant;
    int ceiling = 0;
    double payload_kbps = 0.0;
    double framed_kbps = 0.0;
    double snr_db = 0.0;
    double rise_mean = 0.0;
    double rise_p90 = 0.0;
    double rise_p99 = 0.0;
    // Among cells at least 10 dB above their noise floor: the 10th percentile
    // of signal-to-codec-error, i.e. how badly the worst strong cells fare.
    double strong_snr_p10 = 0.0;
    size_t cells = 0;
    // Per band: mean and 99th-percentile rise, and the strong cells' 10th
    // percentile SNR; empty vectors for bands that were not measured.
    std::vector<std::vector<double>> band_rises, band_strong;
    std::vector<bool> band_inside;
    // NAC3 only: side information, the coefficients as coded, and what an
    // ideal coder with a Gaussian or geometric model would need for them.
    double side_kbps = 0.0, rice_kbps = 0.0, gaussian_kbps = 0.0, laplace_kbps = 0.0;
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    const size_t k = std::min(values.size() - 1, static_cast<size_t>(p * (values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + k, values.end());
    return values[k];
}

// "nac", "nac2", or "nac3" optionally followed by /m<margin dB>, /x<max snr
// dB> and /n<min snr dB>, for example nac3/m12/x42/n12.
bool configure_variant(const std::string& name, nac::Encoder& encoder, nac::Layout& layout,
                       float passband_low, float passband_high, int& group) {
    group = 1;
    if (name == "nac") { layout = nac::Layout::Original; return true; }
    if (name == "nac2") { layout = nac::Layout::Compact; return true; }
    if (name.rfind("nac3", 0) != 0) return false;
    layout = nac::Layout::PerBand;
    nac::Nac3Target target;
    target.passband_low_hz = passband_low;
    target.passband_high_hz = passband_high;
    size_t position = 4;
    while (position < name.size()) {
        if (name[position] != '/' || position + 2 > name.size()) return false;
        const char key = name[position + 1];
        const size_t end = name.find('/', position + 1);
        const float value = static_cast<float>(std::atof(name.substr(position + 2, end - position - 2).c_str()));
        if (key == 'm') target.noise_margin_db = value;
        else if (key == 'x') target.max_snr_db = value;
        else if (key == 'n') target.min_snr_db = value;
        else if (key == 'p') target.band_minimum = value != 0.0f;
        else if (key == 'g') group = std::clamp(static_cast<int>(value), 1, nac::kMaxPacketFrames);
        else return false;
        position = end == std::string::npos ? name.size() : end;
    }
    encoder.set_target(target);
    return true;
}

Row run(const std::vector<float>& input, const std::vector<float>& power_gain, int rate,
        const std::string& variant, int ceiling, const std::vector<std::vector<float>>& input_energy,
        const std::vector<std::vector<float>>& floor, float passband_low, float passband_high,
        std::vector<float>* decoded_out) {
    Row row;
    row.variant = variant;
    row.ceiling = ceiling;
    nac::Encoder encoder(rate);
    encoder.set_bitrate(ceiling);
    nac::Layout layout = nac::Layout::Original;
    int group = 1;
    if (!configure_variant(variant, encoder, layout, passband_low, passband_high, group)) {
        std::fprintf(stderr, "unknown variant '%s'\n", variant.c_str());
        std::exit(2);
    }
    nac::Decoder decoder(rate);

    const size_t frames = input.size() / nac::kFrameHop;
    std::vector<float> decoded(frames * nac::kFrameHop);
    size_t payload = 0, framed = 0;
    double side = 0.0, rice = 0.0, gaussian = 0.0, laplace = 0.0;
    for (size_t f = 0; f < frames; f++) {
        encoder.set_signal_gain(power_gain[f * nac::kFrameHop + nac::kFrameHop - 1]);
        if (layout == nac::Layout::PerBand && group > 1) {
            // A packet of `group` frames: sent when full, decoded as a whole.
            encoder.add_frame(input.data() + f * nac::kFrameHop);
            const int in_packet = encoder.packet_frames();
            if (in_packet == group || f + 1 == frames) {
                const auto& bytes = encoder.finish_packet();
                payload += bytes.size();
                const size_t message = bytes.size() + 4;
                framed += message + (message < 126 ? 2 : 4);
                bool ok = false;
                const size_t first = f + 1 - static_cast<size_t>(in_packet);
                if (decoder.decode_packet(bytes.data(), bytes.size(), decoded.data() + first * nac::kFrameHop,
                                          in_packet, ok) != in_packet || !ok) {
                    std::fprintf(stderr, "packet decode failed at frame %zu\n", f);
                    std::exit(1);
                }
            }
            side += encoder.stats().side_bits;
            continue;
        }
        const auto& bytes = encoder.encode(input.data() + f * nac::kFrameHop, layout);
        payload += bytes.size();
        if (layout == nac::Layout::PerBand) {
            // What the coefficients cost as Rice codes, against what an ideal
            // coder would spend on them with each band's own spread known:
            // a discretised Gaussian, and a two-sided geometric distribution.
            side += encoder.stats().side_bits;
            const int32_t* q = encoder.last_quantised();
            for (int b = 0; b < nac::kNumBands; b++) {
                if (!encoder.last_band_active(b)) continue;
                double energy = 0.0, magnitude = 0.0;
                const int width = nac::kBandWidths[b];
                for (int i = nac::kBandStarts[b]; i < nac::kBandStarts[b + 1]; i++) {
                    rice += rice_cost(zigzag_encode(q[i]), static_cast<uint32_t>(encoder.last_band_rice(b)));
                    energy += static_cast<double>(q[i]) * q[i];
                    magnitude += std::abs(q[i]);
                }
                const double sigma = std::max(0.3, std::sqrt(energy / width));
                const double mean = magnitude / width;
                // Two-sided geometric with the same mean magnitude.
                const double theta = mean / (1.0 + mean);
                const double p_zero = (1.0 - theta) / (1.0 + theta);
                for (int i = nac::kBandStarts[b]; i < nac::kBandStarts[b + 1]; i++) {
                    const double x = std::abs(q[i]);
                    // Probability of this signed integer under a Gaussian of
                    // the band's own spread, rounded to the nearest step.
                    const double p_gauss = 0.5 * (std::erf((x + 0.5) / (sigma * std::sqrt(2.0))) -
                                                  std::erf((x - 0.5) / (sigma * std::sqrt(2.0))));
                    gaussian += -std::log2(std::max(p_gauss, 1e-12));
                    const double p_geometric = x == 0 ? p_zero : p_zero * std::pow(theta, x);
                    laplace += -std::log2(std::max(p_geometric, 1e-12));
                }
            }
        }
        const size_t message = bytes.size() + 4;
        framed += message + (message < 126 ? 2 : 4);
        if (!decoder.decode(bytes.data(), bytes.size(), decoded.data() + f * nac::kFrameHop,
                            encoder.layout_used())) {
            std::fprintf(stderr, "decode failed at frame %zu\n", f);
            std::exit(1);
        }
    }
    const double seconds = static_cast<double>(frames * nac::kFrameHop) / rate;
    row.payload_kbps = payload * 8.0 / seconds / 1000.0;
    row.side_kbps = side / seconds / 1000.0;
    row.rice_kbps = rice / seconds / 1000.0;
    row.gaussian_kbps = gaussian / seconds / 1000.0;
    row.laplace_kbps = laplace / seconds / 1000.0;
    row.framed_kbps = framed * 8.0 / seconds / 1000.0;

    // Decoder output lags its input by exactly one hop: frame f completes the
    // overlap-add of input hop f-1.
    std::vector<float> error(input.size(), 0.0f);
    double signal = 0.0, noise = 0.0;
    for (size_t i = 0; i + nac::kFrameHop < decoded.size(); i++) {
        const double want = input[i];
        const double got = decoded[i + nac::kFrameHop];
        error[i] = static_cast<float>(got - want);
        signal += want * want;
        noise += (got - want) * (got - want);
    }
    row.snr_db = noise > 0.0 ? 10.0 * std::log10(signal / noise) : 200.0;

    const auto error_energy = band_energies(error, frames);
    // Only bands inside the receiver's passband count. Outside it the channel
    // filter has already removed everything, the encoder rightly codes those
    // bands as silent, and "raising" a floor 60 dB below the passband is not
    // something anyone can hear or decode. A band overlapping the passband
    // counts; with no passband given, every band the filter left audible does.
    const float coefficient_hz = static_cast<float>(rate) / 2.0f / nac::kNumCoeffs;
    auto inside = [&](int b) {
        if (!(passband_high > passband_low)) return true;
        return nac::kBandStarts[b + 1] * coefficient_hz > passband_low &&
               nac::kBandStarts[b] * coefficient_hz < passband_high;
    };
    row.band_inside.assign(nac::kNumBands, false);
    for (int b = 0; b < nac::kNumBands; b++) row.band_inside[b] = inside(b);
    std::vector<float> typical(nac::kNumBands, 0.0f);
    for (int b = 0; b < nac::kNumBands; b++) {
        std::vector<double> levels;
        for (size_t f = 0; f < frames; f++) levels.push_back(floor[f][b]);
        typical[b] = static_cast<float>(percentile(levels, 0.5));
    }
    const float loudest = *std::max_element(typical.begin(), typical.end());
    std::vector<double> rises, strong;
    row.band_rises.assign(nac::kNumBands, {});
    row.band_strong.assign(nac::kNumBands, {});
    // Skip the first and last second: the AGC is settling and the percentile
    // window is one-sided there.
    const size_t guard = static_cast<size_t>(rate / nac::kFrameHop);
    for (size_t f = guard; f + guard < frames; f++) {
        for (int b = 0; b < nac::kNumBands; b++) {
            const double n = floor[f][b];
            const double e = input_energy[f][b];
            const double d = error_energy[f][b];
            // Bands the channel filter emptied have no noise to compare
            // against; nothing there is audible or decodable.
            if (!(n > 1e-14) || typical[b] < loudest * 1e-3f) continue;
            const double rise = 10.0 * std::log10((n + d) / n);
            row.band_rises[b].push_back(rise);
            if (row.band_inside[b]) rises.push_back(rise);
            if (e > 10.0 * n) {
                const double snr = 10.0 * std::log10(e / std::max(d, 1e-30));
                row.band_strong[b].push_back(snr);
                if (row.band_inside[b]) strong.push_back(snr);
            }
        }
    }
    row.cells = rises.size();
    double sum = 0.0;
    for (double r : rises) sum += r;
    row.rise_mean = rises.empty() ? 0.0 : sum / rises.size();
    row.rise_p90 = percentile(rises, 0.90);
    row.rise_p99 = percentile(rises, 0.99);
    row.strong_snr_p10 = percentile(strong, 0.10);
    if (decoded_out) {
        decoded_out->assign(decoded.begin() + nac::kFrameHop, decoded.end());
    }
    return row;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--iq") options.iq_path = next();
        else if (arg == "--audio") options.audio_path = next();
        else if (arg == "--mode") options.mode_name = next();
        else if (arg == "--offset") options.offset_hz = std::atof(next().c_str());
        else if (arg == "--audio-rate") options.requested_rate = std::atoi(next().c_str());
        else if (arg == "--seconds") options.seconds = std::atof(next().c_str());
        else if (arg == "--rates") options.rates = parse_rates(next());
        else if (arg == "--variants") options.variants = parse_list(next());
        else if (arg == "--write-dir") options.write_dir = next();
        else if (arg == "--label") options.label = next();
        else if (arg == "--bands") options.show_bands = true;
        else if (arg == "--noise-study") options.noise_study = true;
        else if (arg == "--bench") options.bench = std::atoi(next().c_str());
        else if (arg == "--truth") options.truth_path = next();
        else if (arg == "--passband") {
            const std::string value = next();
            const size_t comma = value.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "--passband wants LOW,HIGH in Hz\n");
                return 2;
            }
            options.passband_low = static_cast<float>(std::atof(value.substr(0, comma).c_str()));
            options.passband_high = static_cast<float>(std::atof(value.substr(comma + 1).c_str()));
            if (!(options.passband_low >= 0.0f && options.passband_high > options.passband_low)) {
                std::fprintf(stderr, "--passband wants LOW,HIGH with 0 <= LOW < HIGH\n");
                return 2;
            }
        }
        else if (arg == "--help" || arg == "-h") {
            std::printf("usage: codec-lab (--iq FILE.wav --mode MODE [--offset HZ] | --audio FILE.wav)\n"
                        "                 [--rates 16000,32000,...] [--variants nac2,...]\n"
                        "                 [--seconds N] [--write-dir DIR] [--label NAME]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        }
    }
    if (options.iq_path.empty() == options.audio_path.empty()) {
        std::fprintf(stderr, "give exactly one of --iq or --audio\n");
        return 2;
    }

    std::string error;
    std::vector<float> input;
    std::vector<float> power_gain;
    int rate = 0;
    if (!options.iq_path.empty()) {
        wav::Audio iq;
        if (!wav::load(options.iq_path, iq, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (iq.channels != 2) {
            std::fprintf(stderr, "%s: IQ needs two channels, found %d\n", options.iq_path.c_str(), iq.channels);
            return 1;
        }
        if (options.seconds > 0) iq.samples.resize(std::min(iq.samples.size(),
            static_cast<size_t>(options.seconds * iq.rate) * 2));
        Mode mode;
        if (!mode_from_name(options.mode_name, mode)) {
            std::fprintf(stderr, "unknown mode %s\n", options.mode_name.c_str());
            return 2;
        }
        input = demodulate(iq, mode, options.offset_hz, options.requested_rate, rate, power_gain);
    } else {
        wav::Audio audio;
        if (!wav::load(options.audio_path, audio, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        rate = audio.rate;
        input.resize(audio.frames());
        for (size_t i = 0; i < input.size(); i++) input[i] = audio.samples[i * audio.channels];
        if (options.seconds > 0) input.resize(std::min(input.size(), static_cast<size_t>(options.seconds * rate)));
    }
    power_gain.resize(input.size(), 1.0f);
    input.resize(input.size() - input.size() % nac::kFrameHop);
    const size_t frames = input.size() / nac::kFrameHop;
    if (frames < 4 * static_cast<size_t>(rate / nac::kFrameHop)) {
        std::fprintf(stderr, "need at least four seconds of audio\n");
        return 1;
    }
    const std::string label = !options.label.empty() ? options.label
        : (!options.iq_path.empty() ? options.iq_path : options.audio_path);

    if (options.passband_low < 0) {
        // The audio passband a listener in this mode has by default: where the
        // encoder should look for the channel noise.
        Mode mode = Mode::Usb;
        mode_from_name(options.mode_name, mode);
        const Passband passband = default_passband(mode, 700.0);
        if (!options.audio_path.empty()) {
            options.passband_low = 0;
            options.passband_high = 0;
        } else if (mode == Mode::Am || mode == Mode::Sam || mode == Mode::Dsb) {
            options.passband_low = 0;
            options.passband_high = static_cast<float>(passband.high);
        } else if (mode == Mode::Nfm) {
            options.passband_low = 0;
            options.passband_high = 0;
        } else {
            options.passband_low = static_cast<float>(std::min(std::fabs(passband.low), std::fabs(passband.high)));
            options.passband_high = static_cast<float>(std::max(std::fabs(passband.low), std::fabs(passband.high)));
        }
    }
    const auto energy = band_energies(input, frames);
    const auto floor = noise_floor(energy, static_cast<double>(rate) / nac::kFrameHop);

    if (!options.write_dir.empty()) {
        if (!wav::save_mono(options.write_dir + "/input.wav", rate, input, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }

    std::printf("# %s: %.1f s at %d Hz\n", label.c_str(), static_cast<double>(input.size()) / rate, rate);
    if (options.show_bands) {
        // Median band energy and median noise floor, relative to the loudest
        // band's median energy: where the passband is and how deep the
        // channel filter's stopband sits in the codec's input.
        std::vector<double> medians(nac::kNumBands), floors(nac::kNumBands);
        for (int b = 0; b < nac::kNumBands; b++) {
            std::vector<double> e, n;
            for (size_t f = 0; f < frames; f++) { e.push_back(energy[f][b]); n.push_back(floor[f][b]); }
            medians[b] = percentile(e, 0.5);
            floors[b] = percentile(n, 0.5);
        }
        const double top = *std::max_element(medians.begin(), medians.end());
        std::printf("# band  from-to Hz      energy dB  floor dB\n");
        for (int b = 0; b < nac::kNumBands; b++) {
            const double hz = static_cast<double>(rate) / (2.0 * nac::kNumCoeffs);
            std::printf("# %4d %5.0f-%-5.0f %10.1f %9.1f\n", b, nac::kBandStarts[b] * hz,
                        nac::kBandStarts[b + 1] * hz, 10 * std::log10(medians[b] / top + 1e-30),
                        10 * std::log10(floors[b] / top + 1e-30));
        }
    }
    if (options.noise_study) {
        // How far each noise estimator lands from the real noise, second by
        // second. The truth is the noise-only companion file, analysed the same
        // way; without one only the estimates are printed.
        std::vector<float> truth;
        if (!options.truth_path.empty()) {
            wav::Audio audio;
            if (!wav::load(options.truth_path, audio, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            truth.resize(audio.frames());
            for (size_t i = 0; i < truth.size(); i++) truth[i] = audio.samples[i * audio.channels];
        }
        const float hz = rate / 2.0f / nac::kNumCoeffs;
        const int first = std::max(0, static_cast<int>(std::ceil(options.passband_low / hz - 0.5f)));
        const int last = options.passband_high > 0
            ? std::min<int>(nac::kNumCoeffs, static_cast<int>(std::floor(options.passband_high / hz - 0.5f)) + 1)
            : static_cast<int>(nac::kNumCoeffs);
        if (first >= last) {
            std::fprintf(stderr, "the passband holds no coefficient below %.0f Hz\n", rate / 2.0);
            return 2;
        }
        Mdct mdct(nac::kFrameHop);
        const auto window = make_sine_window(2 * nac::kFrameHop);
        auto coefficients_of = [&](const std::vector<float>& signal, size_t f, std::vector<float>& out) {
            std::vector<float> windowed(2 * nac::kFrameHop);
            for (size_t i = 0; i < 2 * nac::kFrameHop; i++) {
                const long index = static_cast<long>(f * nac::kFrameHop + i) - static_cast<long>(nac::kFrameHop);
                windowed[i] = (index >= 0 && static_cast<size_t>(index) < signal.size() ? signal[index] : 0.0f) * window[i];
            }
            out.resize(nac::kNumCoeffs);
            mdct.forward(windowed.data(), out.data());
        };
        nac::NoiseEstimate estimate;
        estimate.configure(rate, options.passband_low, options.passband_high);
        const size_t per_second = static_cast<size_t>(rate / nac::kFrameHop);
        std::vector<float> coefficients, truth_coefficients, energies(nac::kNumBands);
        std::vector<double> pooled;
        double truth_energy = 0.0;
        size_t truth_count = 0;
        std::printf("# second  estimator  P5/chi2  P10/chi2  P20/chi2  truth   (dB, energy per coefficient)\n");
        for (size_t f = 0; f < frames; f++) {
            coefficients_of(input, f, coefficients);
            for (int b = 0; b < nac::kNumBands; b++) {
                double e = 0;
                for (int i = nac::kBandStarts[b]; i < nac::kBandStarts[b + 1]; i++) e += coefficients[i] * coefficients[i];
                energies[b] = static_cast<float>(e / nac::kBandWidths[b]);
            }
            estimate.update(coefficients.data(), energies.data(), power_gain[f * nac::kFrameHop]);
            for (int c = first; c < last; c++) pooled.push_back(coefficients[c] * coefficients[c]);
            if (!truth.empty()) {
                coefficients_of(truth, f, truth_coefficients);
                for (int c = first; c < last; c++) truth_energy += truth_coefficients[c] * truth_coefficients[c];
                truth_count += last - first;
            }
            if ((f + 1) % per_second == 0) {
                auto quantile = [&](double p) {
                    std::vector<double> copy = pooled;
                    const size_t k = static_cast<size_t>(p * (copy.size() - 1));
                    std::nth_element(copy.begin(), copy.begin() + k, copy.end());
                    return copy[k];
                };
                const auto db = [](double v) { return 10.0 * std::log10(std::max(v, 1e-30)); };
                std::printf("%8zu %10.2f %8.2f %9.2f %9.2f %7.2f\n", (f + 1) / per_second,
                            db(estimate.passband_noise()), db(quantile(0.05) / 0.00393),
                            db(quantile(0.10) / 0.01579), db(quantile(0.20) / 0.06418),
                            truth_count ? db(truth_energy / truth_count) : 0.0);
                pooled.clear();
                truth_energy = 0.0;
                truth_count = 0;
            }
        }
        return 0;
    }
    if (options.bench > 0) {
        // Encoder time per frame, median of the repetitions. Decoding and the
        // quality analysis are left out: this is the per-listener server cost.
        for (const std::string& variant : options.variants) {
            for (int ceiling : options.rates) {
                std::vector<double> per_frame;
                for (int repetition = 0; repetition < options.bench; repetition++) {
                    nac::Encoder encoder(rate);
                    encoder.set_bitrate(ceiling);
                    nac::Layout layout = nac::Layout::Original;
                    int group = 1;
                    configure_variant(variant, encoder, layout, options.passband_low, options.passband_high, group);
                    const size_t frames = input.size() / nac::kFrameHop;
                    const auto start = std::chrono::steady_clock::now();
                    size_t bytes = 0;
                    for (size_t f = 0; f < frames; f++) {
                        encoder.set_signal_gain(power_gain[f * nac::kFrameHop]);
                        if (layout == nac::Layout::PerBand && group > 1) {
                            encoder.add_frame(input.data() + f * nac::kFrameHop);
                            if (encoder.packet_frames() == group) bytes += encoder.finish_packet().size();
                        } else {
                            bytes += encoder.encode(input.data() + f * nac::kFrameHop, layout).size();
                        }
                    }
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    per_frame.push_back(seconds / frames * 1e6);
                    if (bytes == 0) std::printf("#\n");
                }
                std::printf("%-16s %8d  encode %.2f us/frame (median of %d)\n", variant.c_str(), ceiling,
                            percentile(per_frame, 0.5), options.bench);
            }
        }
        return 0;
    }
    std::printf("%-16s %8s %9s %9s %8s %9s %8s %8s %11s\n", "variant", "ceiling", "payload", "framed",
                "snr", "rise", "p90", "p99", "strong p10");
    for (const std::string& variant : options.variants) {
        for (int ceiling : options.rates) {
            std::vector<float> decoded;
            const Row row = run(input, power_gain, rate, variant, ceiling, energy, floor, options.passband_low,
                                options.passband_high, options.write_dir.empty() ? nullptr : &decoded);
            std::printf("%-16s %8d %9.2f %9.2f %8.2f %9.3f %8.3f %8.3f %11.2f", row.variant.c_str(),
                        row.ceiling, row.payload_kbps, row.framed_kbps, row.snr_db, row.rise_mean,
                        row.rise_p90, row.rise_p99, row.strong_snr_p10);
            if (row.rice_kbps > 0) {
                std::printf("   side %.2f rice %.2f gauss %.2f geom %.2f", row.side_kbps, row.rice_kbps,
                            row.gaussian_kbps, row.laplace_kbps);
            }
            std::printf("\n");
            if (options.show_bands) {
                for (int b = 0; b < nac::kNumBands; b++) {
                    const auto& band = row.band_rises[b];
                    if (band.empty()) continue;
                    double total = 0.0;
                    for (double r : band) total += r;
                    std::printf("#   band %2d  rise %6.3f  p99 %6.3f  strong cells %5zu  p10 %6.2f%s\n", b,
                                total / band.size(), percentile(band, 0.99), row.band_strong[b].size(),
                                percentile(row.band_strong[b], 0.10),
                                row.band_inside[b] ? "" : "  outside the passband, not counted");
                }
            }
            if (!options.write_dir.empty()) {
                std::string file = variant;
                std::replace(file.begin(), file.end(), '/', '_');
                const std::string path = options.write_dir + "/" + file + "-" + std::to_string(ceiling) + ".wav";
                if (!wav::save_mono(path, rate, decoded, error)) {
                    std::fprintf(stderr, "%s\n", error.c_str());
                    return 1;
                }
            }
        }
    }
    return 0;
}
