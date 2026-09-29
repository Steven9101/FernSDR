// Objective audio-quality measurement for the whole receive chain.
//
// Runs a signal through exactly what a listener hears - channelizer, blanker,
// AGC, demodulator, codec, decoder - and compares the result against the
// signal that went in. Because the input is generated here, there is ground
// truth to measure against, which a recording cannot give you.
//
// It also reads real IQ, so the same measurements can be run on your own
// off-air recording:
//
//   server/build/audio-quality --iq capture.s16 --rate 1536000
//                              --center 7100000 --tune 7100000 --mode usb
//
// The synthetic path adds two filtered Gaussian fading taps and impulsive
// noise. It is a repeatable stress fixture, not a conformance implementation
// of a standard HF channel model.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <algorithm>
#include <string>
#include <vector>

#include "../src/codec/nac.h"
#include "../src/dsp/agc.h"
#include "../src/dsp/channel_noise.h"
#include "../src/dsp/cw_filter.h"
#include "../src/dsp/channelizer.h"
#include "../src/dsp/demod.h"
#include "../src/dsp/fft_split.h"

using namespace fernsdr;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Options {
    std::string iq_path;
    std::string format = "s16";
    double rate = 384000.0;
    double center = 7100000.0;
    double tune = 7100000.0;
    std::string mode = "usb";
    double seconds = 8.0;
    int bitrate = 48000;
    /** Repeat the codec row at every bitrate, for comparing one change to another. */
    bool sweep = false;
    /** FM deviation, Hz. The receiver's discriminator is normalised to 3 kHz. */
    double deviation = 3000.0;
    double snr_db = 20.0;
    bool fading = true;
    bool impulses = true;
};

// A fading tap made by low-pass filtering complex Gaussian noise.
class FadingTap {
public:
    FadingTap(double doppler_hz, double sample_rate, uint32_t seed)
        : rng_(seed), coefficient_(std::exp(-2.0 * kPi * doppler_hz / sample_rate)) {}

    cfloat next() {
        std::normal_distribution<float> gaussian(0.0f, 1.0f);
        const float a = static_cast<float>(coefficient_);
        state_ = cfloat(a * state_.real() + (1.0f - a) * gaussian(rng_),
                        a * state_.imag() + (1.0f - a) * gaussian(rng_));
        // Normalised so the tap has unit mean power whatever the Doppler.
        const float scale = static_cast<float>(std::sqrt((1.0 + coefficient_) / (1.0 - coefficient_)));
        return state_ * scale;
    }

private:
    std::mt19937 rng_;
    double coefficient_;
    cfloat state_{0.0f, 0.0f};
};

// Speech-like audio built as a bank of tones, so that the analytic (single
// sideband) version of it is exact rather than approximated by a Hilbert
// filter. That exactness is the whole point: it gives the measurement a
// ground truth to compare against, which no recording can.
//
// Formant-shaped amplitudes, slow per-tone phase drift and syllabic gating
// give it speech's dynamics, which is what the AGC and the codec respond to.
struct Voice {
    std::vector<double> frequency;
    std::vector<double> amplitude;
    std::vector<double> phase;
    double rate = 12000.0;

    static Voice make(double rate, uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> uniform(0.0, 2.0 * kPi);
        Voice voice;
        voice.rate = rate;
        const double formants[] = {500.0, 1500.0, 2400.0};
        // Kept well clear of the passband edges. The channel filter rolls off
        // over a 300 Hz transition at each side, so a voice reaching to 2700
        // would have its top tones attenuated and the comparison would score
        // the filter shape as distortion.
        for (double f = 700.0; f < 2250.0; f += 55.0) {
            double gain = 0.05;
            for (double centre : formants) {
                gain += 1.0 / (1.0 + std::pow((f - centre) / 180.0, 2.0));
            }
            voice.frequency.push_back(f);
            voice.amplitude.push_back(gain);
            voice.phase.push_back(uniform(rng));
        }
        double total = 0.0;
        for (double a : voice.amplitude) total += a;
        for (double& a : voice.amplitude) a /= total;
        return voice;
    }

    // Syllables at about four per second, with real gaps between them: the
    // gaps are where an AGC either behaves or pumps.
    double envelope(double t) const {
        const double syllable = std::fmod(t * 4.0, 1.0);
        return syllable < 0.62 ? std::sin(kPi * syllable / 0.62) : 0.0;
    }

    double real_at(double index) const {
        const double t = static_cast<double>(index) / rate;
        const double e = envelope(t);
        double sum = 0.0;
        for (size_t k = 0; k < frequency.size(); k++) {
            sum += amplitude[k] * std::cos(2.0 * kPi * frequency[k] * t + phase[k]);
        }
        return sum * e;
    }

    // The analytic version: identical spectrum, one-sided. Transmitted on a
    // carrier this is exactly upper sideband.
    cfloat analytic_at(double t) const {
        const double e = envelope(t);
        double re = 0.0, im = 0.0;
        for (size_t k = 0; k < frequency.size(); k++) {
            const double a = 2.0 * kPi * frequency[k] * t + phase[k];
            re += amplitude[k] * std::cos(a);
            im += amplitude[k] * std::sin(a);
        }
        return cfloat(static_cast<float>(re * e), static_cast<float>(im * e));
    }
};

double cw_envelope(double t) {
    // SOS at 20 WPM, including inter-letter and word spacing. Five millisecond
    // cosine edges keep the transmitter from adding artificial key clicks.
    static constexpr int lengths[] = {1, 1, 1, 1, 1, 3, 3, 1, 3, 1, 3, 3, 1, 1, 1, 1, 1, 7};
    double position = std::fmod(t, 34.0 * 0.06);
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        const double duration = lengths[i] * 0.06;
        if (position < duration) {
            if (i & 1) return 0.0;
            const double edge = std::min(position, duration - position);
            return edge >= 0.005 ? 1.0 : 0.5 - 0.5 * std::cos(kPi * edge / 0.005);
        }
        position -= duration;
    }
    return 0.0;
}

// Correlates `got` against `want`, returning the best-fit gain and the
// residual-to-signal ratio in dB. Alignment is searched because the chain
// delays by a codec frame plus the AGC's look-ahead.
struct Comparison {
    double snr_db = 0.0;
    double gain = 1.0;
    size_t lag = 0;
};

// Finds the delay that best aligns `got` with `want`, to the nearest sample.
size_t best_lag(const std::vector<float>& want, const std::vector<float>& got, size_t max_lag) {
    const size_t usable = std::min(want.size(), got.size());
    if (usable < max_lag + 4000) return 0;
    const size_t count = usable - max_lag - 1;
    const size_t start = count / 4;

    size_t best = 0;
    double best_score = -1.0;
    for (size_t lag = 0; lag <= max_lag; lag++) {
        double dot = 0.0, energy = 0.0;
        for (size_t i = start; i < count; i += 3) {
            dot += static_cast<double>(want[i]) * got[i + lag];
            energy += static_cast<double>(got[i + lag]) * got[i + lag];
        }
        // The comparison below fits a positive gain. An anti-correlated lag
        // can win after squaring the dot product, especially with FM's phase
        // response, then make that comparison discard the useful windows.
        const double score = energy > 0.0 && dot > 0.0 ? (dot * dot) / energy : 0.0;
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    return best;
}

// Estimates the residual sub-sample delay between two aligned signals from the
// slope of their cross-spectrum phase, then removes it with a phase ramp.
//
// This matters more than it sounds. The receive chain delays by a fraction of
// a sample, and at 2.4 kHz even half a sample is 0.6 radians of phase error -
// which alone caps any waveform comparison at about 20 dB and makes a
// transparent codec look mediocre. Aligning properly is the difference
// between measuring the codec and measuring the alignment.
void remove_fractional_delay(std::vector<float>& signal, const std::vector<float>& reference,
                             double audio_rate) {
    size_t n = 1;
    while (n * 2 <= std::min(signal.size(), reference.size()) && n < (1u << 16)) n *= 2;
    if (n < 4096) return;

    FftSplit fft(n);
    std::vector<float> a_re(reference.begin(), reference.begin() + n), a_im(n, 0.0f);
    std::vector<float> b_re(signal.begin(), signal.begin() + n), b_im(n, 0.0f);
    fft.forward(a_re.data(), a_im.data());
    fft.forward(b_re.data(), b_im.data());

    // Weighted least-squares fit of phase slope over the occupied band.
    double sum_weight = 0.0, sum_wk = 0.0, sum_wkk = 0.0, sum_wp = 0.0, sum_wkp = 0.0;
    double previous_phase = 0.0;
    double unwrapped = 0.0;
    const size_t low = static_cast<size_t>(300.0 / audio_rate * n);
    const size_t high = static_cast<size_t>(3000.0 / audio_rate * n);
    for (size_t k = low; k < std::min(high, n / 2); k++) {
        // Cross-spectrum: got * conj(want).
        const double cr = static_cast<double>(b_re[k]) * a_re[k] + static_cast<double>(b_im[k]) * a_im[k];
        const double ci = static_cast<double>(b_im[k]) * a_re[k] - static_cast<double>(b_re[k]) * a_im[k];
        const double weight = static_cast<double>(a_re[k]) * a_re[k] + static_cast<double>(a_im[k]) * a_im[k];
        if (weight <= 0.0) continue;

        double phase = std::atan2(ci, cr);
        // Unwrap as k advances so the fit sees a straight line.
        double delta = phase - previous_phase;
        while (delta > kPi) delta -= 2.0 * kPi;
        while (delta < -kPi) delta += 2.0 * kPi;
        unwrapped += delta;
        previous_phase = phase;

        const double kk = static_cast<double>(k);
        sum_weight += weight;
        sum_wk += weight * kk;
        sum_wkk += weight * kk * kk;
        sum_wp += weight * unwrapped;
        sum_wkp += weight * kk * unwrapped;
    }
    const double denominator = sum_weight * sum_wkk - sum_wk * sum_wk;
    if (std::fabs(denominator) < 1e-12) return;
    const double slope = (sum_weight * sum_wkp - sum_wk * sum_wp) / denominator;
    const double delay = -slope * n / (2.0 * kPi);
    if (!std::isfinite(delay) || std::fabs(delay) > 4.0) return;

    // Apply the opposite shift with a windowed-sinc fractional delay. Doing it
    // block-wise in the frequency domain is tempting and wrong: each block's
    // phase ramp wraps circularly, and any tail past the last whole block is
    // left unprocessed - which in an earlier version silently zeroed most of
    // the signal and made every measurement look terrible.
    constexpr int kTaps = 16;
    std::vector<double> kernel(2 * kTaps + 1);
    double kernel_sum = 0.0;
    for (int k = -kTaps; k <= kTaps; k++) {
        const double x = k + delay;
        const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double w = 0.5 * (1.0 + std::cos(kPi * k / (kTaps + 1.0)));
        kernel[k + kTaps] = sinc * w;
        kernel_sum += sinc * w;
    }
    for (double& v : kernel) v /= kernel_sum;

    std::vector<float> out(signal.size(), 0.0f);
    for (size_t i = 0; i < signal.size(); i++) {
        double acc = 0.0;
        for (int k = -kTaps; k <= kTaps; k++) {
            const long j = static_cast<long>(i) + k;
            if (j < 0 || j >= static_cast<long>(signal.size())) continue;
            acc += kernel[k + kTaps] * signal[static_cast<size_t>(j)];
        }
        out[i] = static_cast<float>(acc);
    }
    signal.swap(out);
}

// Residual-to-signal ratio, with the gain fitted per short window.
//
// A single gain for the whole recording cannot work: the AGC is deliberately
// varying the gain, and the codec is measured through it. Fitting per window
// separates "the level changed", which is the AGC doing its job, from "the
// waveform changed", which is the only thing that counts as distortion.
// Windows with no signal are skipped, so the gaps between syllables do not
// dominate the average.
/**
 * Mean squared difference after the best single gain, which is the thing a
 * delay correction is supposed to reduce. Used only to decide whether the
 * correction helped, so it does not need the windowing the real measure has.
 */
double residual_against(const std::vector<float>& want, const std::vector<float>& got) {
    const size_t n = std::min(want.size(), got.size());
    if (n == 0) return 1e30;
    double cross = 0.0, energy = 0.0;
    for (size_t i = 0; i < n; i++) {
        cross += static_cast<double>(want[i]) * got[i];
        energy += static_cast<double>(got[i]) * got[i];
    }
    if (!(energy > 0.0)) return 1e30;
    const double gain = cross / energy;
    double residual = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = want[i] - gain * got[i];
        residual += d * d;
    }
    return residual / static_cast<double>(n);
}

Comparison compare(const std::vector<float>& want, const std::vector<float>& got, size_t max_lag,
                   size_t window, double audio_rate) {
    Comparison best;
    best.lag = best_lag(want, got, max_lag);

    std::vector<float> aligned(got.begin() + static_cast<long>(best.lag), got.end());

    // The fractional-delay fit estimates a phase slope, and on a quiet
    // passband carrying a real fading signal that estimate can be wrong: on
    // one recording it turned a codec into a -18.3 dB result, which is not
    // something a codec can do to its own input. So the correction has to earn
    // its place. Keep it only if the residual it leaves is smaller than the
    // residual without it.
    std::vector<float> corrected = aligned;
    remove_fractional_delay(corrected, want, audio_rate);
    if (residual_against(want, corrected) < residual_against(want, aligned)) {
        aligned = std::move(corrected);
    }

    const size_t usable = std::min(want.size(), aligned.size());
    const size_t start = usable / 5;

    double total_signal = 0.0;
    double total_error = 0.0;
    double reference_peak = 0.0;
    for (size_t i = start; i < usable; i++) reference_peak = std::max<double>(reference_peak, std::fabs(want[i]));
    const double floor = reference_peak * 0.05;

    for (size_t begin = start; begin + window < usable; begin += window) {
        double dot = 0.0, energy = 0.0, want_peak = 0.0;
        for (size_t i = begin; i < begin + window; i++) {
            dot += static_cast<double>(want[i]) * aligned[i];
            energy += static_cast<double>(want[i]) * want[i];
            want_peak = std::max(want_peak, std::fabs((double)want[i]));
        }
        if (energy <= 0.0 || want_peak < floor) continue;  // a gap, not a syllable
        const double gain = dot / energy;
        if (gain <= 0.0) continue;

        for (size_t i = begin; i < begin + window; i++) {
            const double reference = gain * want[i];
            const double difference = aligned[i] - reference;
            total_error += difference * difference;
            total_signal += reference * reference;
        }
    }

    best.snr_db = (total_error > 0.0 && total_signal > 0.0)
                      ? 10.0 * std::log10(total_signal / total_error)
                      : 0.0;
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--iq") options.iq_path = next();
        else if (arg == "--format") options.format = next();
        else if (arg == "--rate") options.rate = std::stod(next());
        else if (arg == "--center") options.center = std::stod(next());
        else if (arg == "--tune") options.tune = std::stod(next());
        else if (arg == "--mode") options.mode = next();
        else if (arg == "--seconds") options.seconds = std::stod(next());
        else if (arg == "--bitrate") options.bitrate = std::stoi(next());
        else if (arg == "--sweep") options.sweep = true;
        else if (arg == "--deviation") options.deviation = std::stod(next());
        else if (arg == "--snr") options.snr_db = std::stod(next());
        else if (arg == "--no-fading") options.fading = false;
        else if (arg == "--no-impulses") options.impulses = false;
        else if (arg == "--help") {
            printf("usage: audio-quality [--iq FILE --rate HZ --format s16 --center HZ --tune HZ]\n"
                   "                     [--mode usb] [--seconds N] [--bitrate BPS] [--snr DB]\n"
                   "                     [--sweep]\n"
                   "                     [--no-fading] [--no-impulses]\n");
            return 0;
        }
    }

    Mode mode = Mode::Usb;
    if (!mode_from_name(options.mode, mode)) {
        fprintf(stderr, "unknown mode '%s'\n", options.mode.c_str());
        return 2;
    }

    printf("FernSDR audio quality\n");
    printf("=====================\n\n");

    const double rate = options.rate;
    // Not const: a recording shorter than --seconds shortens the run. It used
    // to shorten the buffer instead and leave everything downstream iterating
    // to the original length, which walked off the end of it and aborted with
    // a heap error. A tool that crashes on its own documented input is a tool
    // nobody has run on a real recording.
    size_t total = static_cast<size_t>(rate * options.seconds);

    // --- build the band ---
    std::vector<cfloat> band(total, cfloat(0.0f, 0.0f));
    std::vector<float> reference;  // the audio that went in, when we know it

    if (!options.iq_path.empty()) {
        std::ifstream file(options.iq_path, std::ios::binary);
        if (!file) {
            fprintf(stderr, "cannot open %s\n", options.iq_path.c_str());
            return 1;
        }
        std::vector<int16_t> raw(total * 2);
        file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
        const size_t got = static_cast<size_t>(file.gcount()) / 4;
        if (got == 0) {
            fprintf(stderr, "%s holds no complete samples\n", options.iq_path.c_str());
            return 1;
        }
        total = got;
        band.resize(total);
        for (size_t i = 0; i < got; i++) {
            band[i] = cfloat(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
        }
        printf("input          %s, %zu samples (%.2f s) at %.0f Hz\n", options.iq_path.c_str(), got,
               got / rate, rate);
        options.seconds = static_cast<double>(got) / rate;
        printf("               a recording has no ground truth, so the chain cannot be\n"
               "               scored against what was transmitted. The codec still can:\n"
               "               the * row is measured against the audio fed into it.\n\n");
    } else {
        const double audio_rate = 12000.0;
        const Voice voice = Voice::make(audio_rate, 7);

        FadingTap direct(0.5, rate, 11);
        FadingTap delayed(0.5, rate, 12);
        std::mt19937 rng(3);
        std::normal_distribution<float> noise(0.0f, 1.0f);

        const double signal_amplitude = 0.05;
        const double noise_amplitude = signal_amplitude * std::pow(10.0, -options.snr_db / 20.0);
        const double offset = options.tune - options.center;
        const size_t path_delay = static_cast<size_t>(rate * 0.002);
        std::vector<cfloat> delayed_path(std::max<size_t>(1, path_delay));

        double phase = 0.0;
        double fm_phase = 0.0;
        for (size_t i = 0; i < total; i++) {
            const double t = static_cast<double>(i) / rate;

            // The transmitter has to match the mode being received, or the
            // measurement is of the wrong demodulator pointed at the wrong
            // signal rather than of anything a listener would hear.
            cfloat baseband;
            switch (mode) {
                case Mode::Am:
                case Mode::Sam:
                    // A carrier at unity with the audio on it, 70% modulated,
                    // which is about what a station actually runs.
                    baseband = cfloat(static_cast<float>(1.0 + 0.7 * voice.real_at(t * voice.rate)), 0.0f);
                    break;
                case Mode::Nfm: {
                    // Amateur narrow FM: 3 kHz deviation, which is what the
                    // receiver's discriminator gain is normalised to.
                    fm_phase += 2.0 * kPi * options.deviation * voice.real_at(t * voice.rate) / rate;
                    baseband = cfloat(static_cast<float>(std::cos(fm_phase)),
                                      static_cast<float>(std::sin(fm_phase)));
                    break;
                }
                case Mode::Lsb:
                    baseband = std::conj(voice.analytic_at(t));
                    break;
                case Mode::Dsb:
                    baseband = cfloat(static_cast<float>(voice.real_at(t * voice.rate)), 0.0f);
                    break;
                case Mode::Cw:
                case Mode::CwL: {
                    const double angle = (mode == Mode::Cw ? 1.0 : -1.0) * 2.0 * kPi * 700.0 * t;
                    const double envelope = cw_envelope(t);
                    baseband = cfloat(static_cast<float>(envelope * std::cos(angle)),
                                      static_cast<float>(envelope * std::sin(angle)));
                    break;
                }
                default:
                    // Upper sideband is the analytic audio riding on the carrier.
                    baseband = voice.analytic_at(t);
                    break;
            }

            if (options.fading) {
                cfloat faded = baseband * direct.next();
                if (i >= path_delay) {
                    // A second path 2 ms later is what makes HF selective
                    // rather than flat, and it is what a receiver's AGC and
                    // codec actually have to cope with.
                    faded += delayed_path[i % delayed_path.size()] * delayed.next() * 0.6f;
                }
                delayed_path[i % delayed_path.size()] = baseband;
                baseband = faded * 0.5f;
            }

            phase += 2.0 * kPi * offset / rate;
            const cfloat carrier(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
            // Imaginary part first, as GCC on x86-64 drew it when these were
            // argument lists, whose order C++ leaves open: the baselines were
            // measured that way.
            const float noise_im = noise(rng);
            const float noise_re = noise(rng);
            band[i] = baseband * carrier * static_cast<float>(signal_amplitude) +
                      cfloat(noise_re, noise_im) * static_cast<float>(noise_amplitude);

            if (options.impulses && i % static_cast<size_t>(rate * 0.017) == 0) {
                const float impulse_im = noise(rng);
                const float impulse_re = noise(rng);
                band[i] += cfloat(impulse_re, impulse_im) * static_cast<float>(signal_amplitude * 60.0);
            }
        }

        // The audio a perfect receiver would produce.
        const size_t audio_samples = static_cast<size_t>(audio_rate * options.seconds);
        reference.resize(audio_samples);
        for (size_t i = 0; i < audio_samples; i++) {
            const double t = static_cast<double>(i) / audio_rate;
            reference[i] = static_cast<float>(mode == Mode::Cw || mode == Mode::CwL
                ? cw_envelope(t) * std::cos(2.0 * kPi * 700.0 * t) : voice.real_at(i));
        }

        printf("input          synthetic %s %s, %.1f s at %.0f Hz\n", mode_name(mode),
               mode == Mode::Cw || mode == Mode::CwL ? "keyed tone" : "voice", options.seconds, rate);
        printf("channel        %s, %s, %.0f dB SNR\n",
               options.fading ? "two fading taps (0.5 Hz low-pass, 2 ms delay)" : "flat",
               options.impulses ? "impulsive noise every 17 ms" : "no impulses", options.snr_db);
        printf("\n");
    }

    // --- run it through the receiver ---
    size_t fft_size = 1024;
    while (fft_size * 2 <= rate / 50.0 && fft_size < (1u << 20)) fft_size *= 2;
    size_t decimation = 1;
    while (decimation * 2 <= static_cast<size_t>(rate / 12000.0 * 1.4142)) decimation *= 2;
    const size_t ifft_size = std::max<size_t>(16, fft_size / decimation);

    Channelizer channelizer(rate, fft_size, SignalKind::Iq);
    Channel channel(channelizer, ifft_size);
    const Passband passband = default_passband(mode, 700.0);
    channel.set_passband(options.tune - options.center, passband.low, passband.high);

    const double audio_rate = rate * ifft_size / fft_size;

    struct Variant {
        const char* label;
        AgcProfile agc;
        float blanker;
        bool codec;
        int bitrate;   // only meaningful when `codec` is set
    };
    // With --sweep the codec row is repeated at every bitrate the encoder
    // offers. That is the table codec work is actually judged on: a change is
    // only an improvement if it moves this curve, and a ratio quoted without
    // the quality beside it is a trade nobody measured.
    static char sweep_labels[8][48];
    std::vector<Variant> variants = {
        {"channel only (no AGC, no blanker, no codec)", AgcProfile::Off, 0.0f, false, 0},
        {"AGC only", AgcProfile::Slow, 0.0f, false, 0},
        {"AGC + blanker", AgcProfile::Slow, 0.7f, false, 0},
    };
    if (options.sweep) {
        const int rates[] = {16000, 24000, 32000, 48000, 64000, 96000};
        size_t slot = 0;
        for (int rate_bps : rates) {
            snprintf(sweep_labels[slot], sizeof(sweep_labels[slot]),
                     "AGC + blanker + codec at %d kbit/s", rate_bps / 1000);
            variants.push_back({sweep_labels[slot], AgcProfile::Slow, 0.7f, true, rate_bps});
            slot++;
        }
    } else {
        variants.push_back(
            {"AGC + blanker + codec", AgcProfile::Slow, 0.7f, true, options.bitrate});
    }

    printf("%-44s %10s %10s %10s %12s\n", "chain", "chain SNR", "codec SNR", "peak", "bitrate");
    printf("%-44s %10s %10s %10s %12s\n", "-----", "---------", "---------", "----", "-------");

    // The audio that reaches the encoder, kept from the run that does
    // everything except encode. With a recording there is no ground truth for
    // the chain - nobody knows what was transmitted - but the codec has a
    // perfect reference regardless: the audio that went into it. That is the
    // number an operator feeding in their own capture actually wants.
    std::vector<float> pre_codec;

    for (const auto& variant : variants) {
        Channel local(channelizer, ifft_size);
        const bool narrow_cw = mode == Mode::Cw || mode == Mode::CwL;
        local.set_passband(options.tune - options.center, narrow_cw ? -audio_rate / 2 : passband.low,
                           narrow_cw ? audio_rate / 2 : passband.high);
        CwFilter selector;
        selector.configure(audio_rate, passband.low, passband.high, narrow_cw);
        Demodulator demodulator;
        demodulator.configure(mode, audio_rate);
        Agc agc;
        agc.configure(audio_rate);
        // As a listener sets it up (Listener::apply): no gain control for
        // NFM, and AM and SAM following their carrier.
        agc.set_profile(mode == Mode::Nfm ? AgcProfile::Off : variant.agc);
        agc.set_follow_carrier(mode == Mode::Am || mode == Mode::Sam);
        agc.set_manual_gain_db(variant.agc == AgcProfile::Off && mode != Mode::Nfm ? 20.0f : 0.0f);
        // And told the noise around the channel, as a listener's is.
        ChannelNoise noise;
        const double noise_width = narrow_cw ? passband.high - passband.low : local.noise_bandwidth_hz();
        // The blanker belongs on the band's raw input, before any channel
        // filtering; see NoiseBlanker.
        NoiseBlanker blanker;
        blanker.configure(rate);
        blanker.set_strength(variant.blanker);
        nac::Encoder encoder(static_cast<int>(std::lround(audio_rate)));
        encoder.set_bitrate(variant.bitrate > 0 ? variant.bitrate : options.bitrate);
        nac::Decoder decoder(static_cast<int>(std::lround(audio_rate)));

        Channelizer local_channelizer(rate, fft_size, SignalKind::Iq);

        std::vector<cfloat> baseband(ifft_size / 2);
        std::vector<float> audio(ifft_size / 2);
        std::vector<float> output;
        std::vector<float> frame(nac::kFrameHop);
        std::vector<float> pending;
        size_t coded_bytes = 0;
        size_t frames = 0;

        const size_t block = local_channelizer.block_size();
        std::vector<cfloat> wideband(block);
        for (size_t offset = 0; offset + block <= band.size(); offset += block) {
            std::copy(band.begin() + offset, band.begin() + offset + block, wideband.begin());
            blanker.process(wideband.data(), wideband.size());
            local_channelizer.process(wideband.data());
            local.pull(local_channelizer, baseband.data());
            selector.process(baseband.data(), baseband.size());
            if (agc.profile() != AgcProfile::Off) {
                noise.update(local_channelizer.current_block(),
                             options.tune - options.center + 0.5 * (passband.low + passband.high), false);
                agc.set_noise_power(noise.channel_power(noise_width));
            }
            agc.process(baseband.data(), baseband.size());
            demodulator.process(baseband.data(), baseband.size(), audio.data());

            if (!variant.codec) {
                output.insert(output.end(), audio.begin(), audio.end());
                continue;
            }
            pending.insert(pending.end(), audio.begin(), audio.end());
            size_t consumed = 0;
            while (pending.size() - consumed >= nac::kFrameHop) {
                const auto& payload = encoder.encode(pending.data() + consumed);
                coded_bytes += payload.size();
                frames++;
                decoder.decode(payload.data(), payload.size(), frame.data());
                output.insert(output.end(), frame.begin(), frame.end());
                consumed += nac::kFrameHop;
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<long>(consumed));
        }

        const double peak = [&] {
            double p = 0.0;
            for (size_t i = output.size() / 4; i < output.size(); i++) p = std::max(p, std::fabs((double)output[i]));
            return p;
        }();

        char bitrate_text[32] = "-";
        if (frames) {
            snprintf(bitrate_text, sizeof(bitrate_text), "%.1f kbit/s",
                     coded_bytes * 8.0 * audio_rate / (frames * nac::kFrameHop) / 1000.0);
        }

        // Remember the last uncoded chain, which is the codec's reference.
        if (!variant.codec && variant.blanker > 0.0f) pre_codec = output;

        // What the codec alone did, against the audio handed to it.
        //
        // This column exists because the chain SNR cannot answer the question
        // codec work asks. Measured end to end on a faded channel the fading
        // is most of the error, and the reading barely moves: across a six to
        // one range of bitrate it shifts by two tenths of a dB, which is
        // indistinguishable from noise in the measurement. Against its own
        // input the codec has a perfect reference and the number moves with
        // the bits, which is the whole point of having one.
        char codec_text[32] = "-";
        if (variant.codec && !pre_codec.empty()) {
            const Comparison result =
                compare(pre_codec, output, static_cast<size_t>(audio_rate * 0.06),
                        static_cast<size_t>(audio_rate * 0.02), audio_rate);
            snprintf(codec_text, sizeof(codec_text), "%.1f dB", result.snr_db);
        }

        char snr_text[32] = "n/a";
        if (reference.empty() && variant.codec && !pre_codec.empty()) {
            snprintf(snr_text, sizeof(snr_text), "%s*", codec_text);
        } else if (!reference.empty()) {
            std::vector<float> want(output.size());
            for (size_t i = 0; i < want.size(); i++) {
                const double index = static_cast<double>(i) * 12000.0 / audio_rate;
                const size_t base = static_cast<size_t>(index);
                const double fraction = index - base;
                const float a = base < reference.size() ? reference[base] : 0.0f;
                const float b = base + 1 < reference.size() ? reference[base + 1] : 0.0f;
                want[i] = static_cast<float>(a * (1.0 - fraction) + b * fraction);
            }
            // 20 ms windows: long enough to fit a gain, short enough that the
            // AGC's own movement inside one is negligible.
            const Comparison result = compare(want, output, static_cast<size_t>(audio_rate * 0.06),
                                              static_cast<size_t>(audio_rate * 0.02), audio_rate);
            snprintf(snr_text, sizeof(snr_text), "%.1f dB", result.snr_db);
        }

        printf("%-44s %10s %10s %10.3f %12s\n", variant.label, snr_text, codec_text, peak,
               bitrate_text);
    }

    printf("\naudio rate     %.0f Hz\n", audio_rate);
    printf("channelizer    FFT %zu, %.1f Hz bins, %.2f ms blocks\n", fft_size, channelizer.bin_hz(),
           channelizer.block_seconds() * 1000.0);
    printf("\nBoth columns are residual-to-signal after the best-fit gain and delay\n"
           "are removed. Chain SNR is against the audio that was transmitted, so on\n"
           "a faded channel the fading counts as error and the figure is pessimistic;\n"
           "it compares the rows above the codec, not the codec itself. Codec SNR is\n"
           "against the audio the encoder was handed, which is the only reference the\n"
           "codec can be judged by, and the only column that moves with the bitrate.\n");
    return 0;
}
