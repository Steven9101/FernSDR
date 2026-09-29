// How each gain control profile treats signals whose right answer is known.
//
//   make -C server agc-lab
//
// Every scene is played into a real Band with one Listener per profile on it,
// and each listener's stream is decoded the way the browser decodes it, so
// what is measured is what ships, codec included. The scenes are synthetic
// and built from fixed seeds: the level each part should come out at is
// known, and a run is repeatable. Recordings confirm a choice made here; they
// cannot make it, because they carry no right answer.
//
//   talkers   two SSB stations taking turns, one 20 dB over the other and
//             both well over the noise. A gain control should bring both to
//             the same loudness, and the weaker one up soon after the
//             stronger one stops.
//   qsb       one SSB station fading 20 dB in a second, holding, and back.
//   pauses    one SSB station, 3 s of speech and 3 s of silence: how far the
//             noise rises in a pause between overs, and when.
//   qrn       the same with static in the silence, 30 ms crashes 12 dB over
//             the noise twice a second: the noise between them.
//   weak      one SSB station at 3, 10, 20 and 30 dB over the noise: output
//             level against SNR.
//   cw        Morse at 25 WPM (cw-speeds: 15 and 35), 20 dB over the noise,
//             then with a neighbour 20 dB stronger keying 150 Hz away, inside
//             the 500 Hz filter.
//   am        a carrier modulated 80% by a 400 Hz tone, fading 12 dB and back;
//   am-depth  and unfaded at 30, 80 and 100% by 400 and 80 Hz: distortion.
//   digital   a steady tone, as RTTY's mark or an FT8 tone: the gain must not
//             move under it.
//   qso PATH  a recording in cs16 at 48 kHz, I and Q swapped, with a USB
//             station 14.45 kHz under its centre, as the 20 m QSO by DK3QN
//             that tuned Steady is. No right answer there: how far under the
//             speech the pauses sit (the 95th minus the 5th percentile of
//             32 ms frames) and how steady their noise is (the spread of the
//             quietest quarter), against no gain control at all.
//   delays    the chain's delay per profile and mode, which every measurement
//             above takes off before it reads a window; without that, a 48 ms
//             Morse dot was read half in the gap after it.
//
// `make agc-lab` runs all but cw-speeds and qso, which take an argument or
// more time; `build/agc-lab NAME` runs one.
//
// The synthetic voice is 40 tones between 300 and 2700 Hz, falling at 6 dB
// an octave above 500 Hz, under an envelope of syllables, words and pauses:
// speech's spectrum and rhythm, which are what a gain control reacts to,
// without its content.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../src/codec/nac.h"
#include "../src/core/band.h"
#include "../src/core/listener.h"
#include "../src/core/protocol.h"
#include "../src/dsp/agc.h"
#include "../src/dsp/demod.h"
#include "../src/util/config.h"

using namespace fernsdr;

namespace {

constexpr double kRate = 48000.0;
constexpr double kCenter = 7100000.0;
// Every signal sits this far above the band's centre, away from anything a
// receiver does at zero frequency.
constexpr double kOffset = 12000.0;
constexpr double kPi = 3.14159265358979323846;
// Per component, so the noise's power in a channel of bandwidth B is
// 2 * kNoise^2 * B / kRate.
constexpr double kNoise = 0.0005;

// What is compared: every profile a listener can choose. Auto is Slow.
struct Variant {
    std::string label;
    AgcProfile profile;
};

std::vector<Variant> variants() {
    std::vector<Variant> out;
    for (const AgcProfile p : {AgcProfile::Off, AgcProfile::Fast, AgcProfile::Medium, AgcProfile::Slow, AgcProfile::Long}) {
        out.push_back({agc_profile_name(p), p});
    }
    // AGC_LAB_ONLY=off,slow keeps those labels only.
    if (const char* only = std::getenv("AGC_LAB_ONLY")) {
        const std::string list = std::string(",") + only + ",";
        std::vector<Variant> kept;
        for (const Variant& v : out) {
            if (list.find("," + v.label + ",") != std::string::npos) kept.push_back(v);
        }
        out = kept;
    }
    return out;
}

const std::vector<Variant> kVariants = variants();

double db(double power) { return 10.0 * std::log10(power + 1e-30); }

// The amplitude that puts a unit-power signal `snr_db` over the noise in
// `bandwidth_hz`.
double amplitude_for(double snr_db, double bandwidth_hz) {
    const double noise_power = 2.0 * kNoise * kNoise * bandwidth_hz / kRate;
    return std::sqrt(noise_power * std::pow(10.0, snr_db / 10.0));
}

class SceneSource final : public Source {
public:
    explicit SceneSource(const std::vector<cfloat>& samples) : samples_(samples) {}
    bool start(std::string&) override { return true; }
    void stop() override {}
    SignalKind kind() const override { return SignalKind::Iq; }
    bool read(cfloat* out, size_t count) override {
        for (size_t i = 0; i < count; i++) out[i] = position_ < samples_.size() ? samples_[position_++] : cfloat(0, 0);
        return true;
    }
    double sample_rate() const override { return kRate; }
    double center_hz() const override { return kCenter; }
    SourceStats stats() const override { return {}; }
    const char* kind_name() const override { return "scene"; }
    bool done() const { return position_ >= samples_.size(); }

private:
    const std::vector<cfloat>& samples_;
    size_t position_ = 0;
};

// A speech-like signal: tones with a speech spectrum, unit power while
// sounding, analytic by construction so it lands on one side of `offset_hz`.
class Voice {
public:
    explicit Voice(unsigned seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<double> hz(300.0, 2700.0), turn(0.0, 2.0 * kPi);
        double power = 0;
        for (int i = 0; i < 40; i++) {
            const double f = hz(random);
            frequency_.push_back(f);
            amplitude_.push_back(f > 500.0 ? 500.0 / f : 1.0);
            phase_.push_back(turn(random));
            power += amplitude_.back() * amplitude_.back();
        }
        for (double& a : amplitude_) a /= std::sqrt(power);
    }
    cfloat at(size_t n, double offset_hz) const {
        const double t = static_cast<double>(n) / kRate;
        double re = 0, im = 0;
        for (size_t i = 0; i < frequency_.size(); i++) {
            const double angle = 2.0 * kPi * (frequency_[i] + offset_hz) * t + phase_[i];
            re += amplitude_[i] * std::cos(angle);
            im += amplitude_[i] * std::sin(angle);
        }
        return cfloat(static_cast<float>(re), static_cast<float>(im));
    }

private:
    std::vector<double> frequency_, amplitude_, phase_;
};

struct Span {
    size_t begin, end;  // samples at kRate
};

// Syllables of 100 to 300 ms, 50 to 150 ms apart, four or five to a word,
// words 300 to 700 ms apart, each syllable up to 3 dB off the mean level.
// Writes the envelope for [from, to) and returns the syllables.
std::vector<Span> speak(std::mt19937& random, std::vector<float>& envelope, size_t from, size_t to) {
    std::uniform_real_distribution<double> syllable(0.10, 0.30), gap(0.05, 0.15), pause(0.30, 0.70), level(-3, 3);
    std::uniform_int_distribution<int> per_word(4, 5);
    std::vector<Span> spans;
    size_t at = from;
    while (at < to) {
        const int count = per_word(random);
        for (int s = 0; s < count && at < to; s++) {
            const size_t n = std::min(static_cast<size_t>(syllable(random) * kRate), to - at);
            const double gain = std::pow(10.0, level(random) / 20.0);
            const double edge = 0.01 * kRate;
            for (size_t i = 0; i < n; i++) {
                const double rise = std::min({1.0, static_cast<double>(i) / edge, static_cast<double>(n - i) / edge});
                envelope[at + i] = static_cast<float>(gain * rise);
            }
            spans.push_back({at, at + n});
            at += n + static_cast<size_t>(gap(random) * kRate);
        }
        at += static_cast<size_t>(pause(random) * kRate);
    }
    return spans;
}

void add_noise(std::vector<cfloat>& samples, unsigned seed) {
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(kNoise));
    // Imaginary part first, as GCC on x86-64 drew it when this was an
    // argument list, whose order C++ leaves open: the recorded figures were
    // measured that way.
    for (cfloat& s : samples) {
        const float im = noise(random);
        const float re = noise(random);
        s += cfloat(re, im);
    }
}

struct Heard {
    std::string label;
    std::vector<float> audio;
    // How far the stream lags the scene: the channel filter, the
    // demodulator, the codec and any look-ahead in the gain control.
    double delay_s = 0;
};

std::vector<Heard> play_variants(const std::vector<cfloat>& scene, Mode mode, double& audio_rate);

// Power of one frequency in a window, by a single DFT bin under a Hann
// window, whose sidelobes keep a neighbour a few bins away out of it.
// A sine of amplitude A reads A * A / 2.
double tone_power(const std::vector<float>& audio, double audio_rate, size_t begin, size_t length, double hz) {
    std::complex<double> sum = 0;
    double weight = 0;
    for (size_t i = 0; i < length && begin + i < audio.size(); i++) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(length));
        const double angle = -2.0 * kPi * hz * static_cast<double>(i) / audio_rate;
        sum += w * static_cast<double>(audio[begin + i]) * std::complex<double>(std::cos(angle), std::sin(angle));
        weight += w;
    }
    return weight > 0 ? 2.0 * std::norm(sum) / (weight * weight) : 0.0;
}

// The chain's delay for each profile in `mode`: a tone burst from 1 s to 2 s,
// and where the tone first reaches half the level it settles at in each
// decoded stream. The measurements shift their windows by it; without that,
// a 48 ms Morse dot is measured half in the gap after it.
std::vector<double> chain_delays(Mode mode, double audio_hz) {
    const size_t length = static_cast<size_t>(2.5 * kRate);
    std::vector<cfloat> scene(length);
    for (size_t n = 0; n < length; n++) {
        const double t = static_cast<double>(n) / kRate;
        if (t < 1.0 || t > 2.0) continue;
        const double edge = std::min(1.0, (t - 1.0) / 0.005);
        const std::complex<double> s =
            mode == Mode::Am
                ? 0.05 * edge * (1.0 + 0.5 * std::cos(2.0 * kPi * audio_hz * t)) * std::polar(1.0, 2.0 * kPi * kOffset * t)
                : 0.05 * edge * std::polar(1.0, 2.0 * kPi * (kOffset + audio_hz) * t);
        scene[n] = cfloat(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    }
    add_noise(scene, 99);
    double rate = 0;
    std::vector<double> delays;
    for (const Heard& heard : play_variants(scene, mode, rate)) {
        const size_t window = static_cast<size_t>(0.01 * rate), hop = std::max<size_t>(1, static_cast<size_t>(0.0005 * rate));
        std::vector<double> level;
        for (size_t at = 0; at + window < heard.audio.size(); at += hop) {
            level.push_back(tone_power(heard.audio, rate, at, window, audio_hz));
        }
        // Where it has settled, well inside the burst for any delay under
        // 0.4 s.
        std::vector<double> settled;
        for (size_t i = 0; i < level.size(); i++) {
            const double t = (static_cast<double>(i * hop) + 0.5 * static_cast<double>(window)) / rate;
            if (t > 1.4 && t < 1.9) settled.push_back(level[i]);
        }
        std::sort(settled.begin(), settled.end());
        const double half = settled.empty() ? 0 : 0.25 * settled[settled.size() / 2];  // half the amplitude
        double delay = -1;
        for (size_t i = 0; i < level.size(); i++) {
            if (level[i] >= half && half > 0) {
                delay = (static_cast<double>(i * hop) + 0.5 * static_cast<double>(window)) / rate - 1.0025;
                break;
            }
        }
        delays.push_back(delay);
    }
    return delays;
}

// Plays the scene through a band with one listener per profile, with each
// stream's delay for the mode, measured once.
std::vector<Heard> play(const std::vector<cfloat>& scene, Mode mode, double& audio_rate) {
    static std::map<Mode, std::vector<double>> delays;
    if (!delays.count(mode)) delays[mode] = chain_delays(mode, mode == Mode::Cw ? 700.0 : 1000.0);
    std::vector<Heard> heard = play_variants(scene, mode, audio_rate);
    for (size_t i = 0; i < heard.size(); i++) heard[i].delay_s = delays[mode][i];
    return heard;
}

// Plays the scene through a band with one listener per profile and returns
// what each listener's decoded stream holds, at `audio_rate`.
std::vector<Heard> play_once(const std::vector<cfloat>& scene, Mode mode, const std::vector<Variant>& list,
                             double& audio_rate) {
    ConfigSection section("band:lab");
    auto owned = std::make_unique<SceneSource>(scene);
    SceneSource* source = owned.get();
    Band band("lab", "lab", std::move(owned), section);
    std::vector<std::shared_ptr<Listener>> listeners;
    std::vector<std::vector<std::vector<uint8_t>>> messages;
    uint64_t id = 1;
    for (const Variant& variant : list) {
        const AgcProfile profile = variant.profile;
        auto listener = std::make_shared<Listener>(id++, band);
        ChannelSettings channel = listener->channel();
        channel.frequency_hz = kCenter + kOffset;
        channel.mode = mode;
        const Passband passband = default_passband(mode, channel.cw_pitch_hz);
        channel.bandwidth_low = passband.low;
        channel.bandwidth_high = passband.high;
        channel.agc = profile;
        listener->set_channel(channel);
        ViewportSettings view = listener->viewport();
        view.enabled = false;
        listener->set_viewport(view);
        band.add_listener(listener);
        listeners.push_back(listener);
        messages.emplace_back();
    }
    while (!source->done()) {
        if (!band.process_one_block()) break;
        for (size_t i = 0; i < listeners.size(); i++) listeners[i]->drain(messages[i]);
    }
    std::vector<Heard> out;
    for (size_t i = 0; i < listeners.size(); i++) {
        audio_rate = listeners[i]->actual_audio_rate();
        nac::Decoder decoder(static_cast<int>(audio_rate));
        std::vector<float> frame(nac::kFrameHop), audio;
        for (const auto& message : messages[i]) {
            if (message.empty() || message[0] != proto::kStreamAudio) continue;
            decoder.decode(message.data() + proto::kAudioHeaderBytes, message.size() - proto::kAudioHeaderBytes,
                           frame.data(), (message[1] & 2) != 0);
            audio.insert(audio.end(), frame.begin(), frame.end());
        }
        out.push_back({list[i].label, std::move(audio)});
    }
    return out;
}

// Every variant, in kVariants' order, each with a listener of its own on one
// band.
std::vector<Heard> play_variants(const std::vector<cfloat>& scene, Mode mode, double& audio_rate) {
    return play_once(scene, mode, kVariants, audio_rate);
}

// Where a span of the scene lies in a stream, trimmed at both ends so the
// filters' edges do not blur it: [begin, end) in stream samples.
std::pair<size_t, size_t> in_stream(const Heard& heard, double audio_rate, Span span, double trim_s) {
    const double begin = static_cast<double>(span.begin) / kRate + heard.delay_s + trim_s;
    const double end = static_cast<double>(span.end) / kRate + heard.delay_s - trim_s;
    return {static_cast<size_t>(std::max(0.0, begin) * audio_rate), static_cast<size_t>(std::max(0.0, end) * audio_rate)};
}

// Mean power of a stream over a span of the scene.
double power_over(const Heard& heard, double audio_rate, Span span, double trim_s = 0.03) {
    const auto [begin, end] = in_stream(heard, audio_rate, span, trim_s);
    if (end <= begin || end > heard.audio.size()) return 0;
    double sum = 0;
    for (size_t i = begin; i < end; i++) sum += static_cast<double>(heard.audio[i]) * heard.audio[i];
    return sum / static_cast<double>(end - begin);
}

// Power of one tone in a stream over a span of the scene.
double tone_over(const Heard& heard, double audio_rate, Span span, double hz, double trim_s) {
    const auto [begin, end] = in_stream(heard, audio_rate, span, trim_s);
    if (end <= begin || end > heard.audio.size()) return 0;
    return tone_power(heard.audio, audio_rate, begin, end - begin, hz);
}

double median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void talkers() {
    std::printf("\ntalkers: A, then B 20 dB weaker, four turns each of 6 s; B is 25 dB over the noise\n");
    std::printf("%-8s %9s %11s %12s %10s\n", "profile", "B - A dB", "B late, s", "pause dB", "A dBFS");
    const size_t turn = static_cast<size_t>(6 * kRate), turns = 8;
    std::vector<cfloat> scene(turn * turns);
    std::vector<float> envelope(scene.size(), 0.0f);
    std::mt19937 random(7);
    std::vector<std::vector<Span>> spans;
    const size_t half_second = static_cast<size_t>(kRate / 2);
    for (size_t t = 0; t < turns; t++) spans.push_back(speak(random, envelope, t * turn, (t + 1) * turn - half_second));
    const Voice a(1), b(2);
    const double weak = amplitude_for(25.0, 2400.0), strong = weak * 10.0;
    for (size_t n = 0; n < scene.size(); n++) {
        const bool a_turn = (n / turn) % 2 == 0;
        scene[n] = static_cast<float>((a_turn ? strong : weak) * envelope[n]) * (a_turn ? a.at(n, kOffset) : b.at(n, kOffset));
    }
    add_noise(scene, 11);
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        std::vector<double> level_a, level_b, late, pauses;
        for (size_t t = 1; t < turns; t++) {
            const bool a_turn = t % 2 == 0;
            std::vector<double> levels;
            for (const Span& s : spans[t]) levels.push_back(db(power_over(heard, rate, s)));
            // Steady level: syllables from the third second of the turn on.
            std::vector<double> steady;
            for (size_t i = 0; i < spans[t].size(); i++) {
                if (spans[t][i].begin >= t * turn + 2 * static_cast<size_t>(kRate)) steady.push_back(levels[i]);
            }
            const double settled = median(steady);
            (a_turn ? level_a : level_b).push_back(settled);
            if (!a_turn) {
                // Until the first syllable within 3 dB of where B settles.
                double first = 6.0;
                for (size_t i = 0; i < spans[t].size(); i++) {
                    if (levels[i] > settled - 3.0) {
                        first = static_cast<double>(spans[t][i].begin - t * turn) / kRate;
                        break;
                    }
                }
                late.push_back(first);
            }
            // The last half second of each turn is silent: what the noise does there.
            pauses.push_back(db(power_over(heard, rate, {(t + 1) * turn - half_second + 4800, (t + 1) * turn})) -
                             settled);
        }
        std::printf("%-8s %9.1f %11.2f %12.1f %10.1f\n", heard.label.c_str(),
                    median(level_b) - median(level_a), median(late), median(pauses), median(level_a));
    }
}

void qsb() {
    std::printf("\nqsb: one station 35 dB over the noise, down 20 dB over 1 s at 10 s, back over 1 s at 16 s\n");
    std::printf("%-8s %14s %16s\n", "profile", "faded dB", "syllables > 3 dB low");
    const size_t length = static_cast<size_t>(26 * kRate);
    std::vector<cfloat> scene(length);
    std::vector<float> envelope(length, 0.0f);
    std::mt19937 random(8);
    const std::vector<Span> spans = speak(random, envelope, 0, length);
    const Voice voice(3);
    const double level = amplitude_for(35.0, 2400.0);
    const auto fade = [](double t) {
        if (t < 10.0 || t > 17.0) return 1.0;
        if (t < 11.0) return std::pow(10.0, -20.0 * (t - 10.0) / 20.0);
        if (t < 16.0) return 0.1;
        return std::pow(10.0, -20.0 * (17.0 - t) / 20.0);
    };
    for (size_t n = 0; n < length; n++) {
        scene[n] = static_cast<float>(level * envelope[n] * fade(static_cast<double>(n) / kRate)) * voice.at(n, kOffset);
    }
    add_noise(scene, 12);
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        std::vector<double> before, during;
        for (const Span& s : spans) {
            const double t = static_cast<double>(s.begin) / kRate;
            const double level_db = db(power_over(heard, rate, s));
            if (t > 3.0 && t < 10.0) before.push_back(level_db);
            if (t > 11.0 && t < 16.0) during.push_back(level_db);
        }
        const double reference = median(before);
        int low = 0;
        for (const double d : during) low += d < reference - 3.0;
        std::printf("%-8s %14.1f %15.0f%%\n", heard.label.c_str(), median(during) - reference,
                    during.empty() ? 0.0 : 100.0 * low / static_cast<double>(during.size()));
    }
}

void pauses() {
    std::printf("\npauses: one station 25 dB over the noise, 3 s of speech then 3 s of silence, eight times;\n"
                "the noise in each half second of the silence, dB against the speech\n");
    std::printf("%-8s %7s %7s %7s %7s %7s %7s\n", "profile", "0-0.5", "0.5-1", "1-1.5", "1.5-2", "2-2.5", "2.5-3");
    const size_t turn = static_cast<size_t>(6 * kRate), turns = 8, talk = static_cast<size_t>(3 * kRate);
    std::vector<cfloat> scene(turn * turns);
    std::vector<float> envelope(scene.size(), 0.0f);
    std::mt19937 random(21);
    std::vector<std::vector<Span>> spans;
    for (size_t t = 0; t < turns; t++) spans.push_back(speak(random, envelope, t * turn, t * turn + talk));
    const Voice voice(5);
    const double level = amplitude_for(25.0, 2400.0);
    for (size_t n = 0; n < scene.size(); n++) scene[n] = static_cast<float>(level * envelope[n]) * voice.at(n, kOffset);
    add_noise(scene, 17);
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        std::vector<double> speech;
        std::vector<std::vector<double>> quiet(6);
        for (size_t t = 1; t < turns; t++) {
            for (const Span& s : spans[t]) speech.push_back(db(power_over(heard, rate, s)));
            const size_t last = spans[t].back().end;
            for (size_t h = 0; h < 6; h++) {
                const size_t from = last + h * static_cast<size_t>(kRate / 2), to = from + static_cast<size_t>(kRate / 2);
                if (to <= (t + 1) * turn) quiet[h].push_back(db(power_over(heard, rate, {from, to}, 0.01)));
            }
        }
        const double reference = median(speech);
        std::printf("%-8s", heard.label.c_str());
        for (const auto& q : quiet) std::printf(" %7.1f", q.empty() ? 0.0 : median(q) - reference);
        std::printf("\n");
    }
}

void qrn() {
    std::printf("\nqrn: as pauses, with 30 ms crashes 12 dB over the noise twice a second in the silence;\n"
                "the noise between the crashes in each half second, dB against the speech\n");
    std::printf("%-8s %7s %7s %7s %7s %7s %7s\n", "profile", "0-0.5", "0.5-1", "1-1.5", "1.5-2", "2-2.5", "2.5-3");
    const size_t turn = static_cast<size_t>(6 * kRate), turns = 8, talk = static_cast<size_t>(3 * kRate);
    std::vector<cfloat> scene(turn * turns);
    std::vector<float> envelope(scene.size(), 0.0f);
    std::mt19937 random(21);
    std::vector<std::vector<Span>> spans;
    for (size_t t = 0; t < turns; t++) spans.push_back(speak(random, envelope, t * turn, t * turn + talk));
    const Voice voice(5);
    const double level = amplitude_for(25.0, 2400.0);
    for (size_t n = 0; n < scene.size(); n++) scene[n] = static_cast<float>(level * envelope[n]) * voice.at(n, kOffset);
    add_noise(scene, 17);
    // Crashes: band-wide bursts, the band noise 12 dB up for 30 ms, starting
    // 0.2 s after the last syllable and every half second after.
    std::mt19937 crash_random(22);
    std::normal_distribution<float> crash(0.0f, static_cast<float>(kNoise * std::pow(10.0, 12.0 / 20.0)));
    const size_t crash_length = static_cast<size_t>(0.03 * kRate), every = static_cast<size_t>(kRate / 2);
    std::vector<std::vector<Span>> crashes(turns);
    for (size_t t = 0; t < turns; t++) {
        for (size_t start = spans[t].back().end + static_cast<size_t>(0.2 * kRate); start + crash_length < (t + 1) * turn;
             start += every) {
            for (size_t i = start; i < start + crash_length; i++) scene[i] += cfloat(crash(crash_random), crash(crash_random));
            crashes[t].push_back({start, start + crash_length});
        }
    }
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        std::vector<double> speech;
        std::vector<std::vector<double>> quiet(6);
        for (size_t t = 1; t < turns; t++) {
            for (const Span& s : spans[t]) speech.push_back(db(power_over(heard, rate, s)));
            const size_t last = spans[t].back().end;
            for (size_t h = 0; h < 6; h++) {
                const size_t from = last + h * static_cast<size_t>(kRate / 2), to = from + static_cast<size_t>(kRate / 2);
                if (to > (t + 1) * turn) continue;
                // The half second, less the crashes and 20 ms either side.
                std::vector<Span> clear;
                size_t at = from;
                for (const Span& c : crashes[t]) {
                    const size_t guard = static_cast<size_t>(0.02 * kRate);
                    if (c.end + guard <= from || c.begin >= to + guard) continue;
                    if (c.begin > at + guard) clear.push_back({at, c.begin - guard});
                    at = std::max(at, c.end + guard);
                }
                if (to > at) clear.push_back({at, to});
                double sum = 0;
                size_t pieces = 0;
                for (const Span& piece : clear) {
                    if (piece.end - piece.begin < static_cast<size_t>(0.05 * kRate)) continue;
                    sum += power_over(heard, rate, piece, 0.005);
                    pieces++;
                }
                if (pieces > 0) quiet[h].push_back(db(sum / static_cast<double>(pieces)));
            }
        }
        const double reference = median(speech);
        std::printf("%-8s", heard.label.c_str());
        for (const auto& q : quiet) std::printf(" %7.1f", q.empty() ? 0.0 : median(q) - reference);
        std::printf("\n");
    }
}

void weak() {
    std::printf("\nweak: output speech and pause level against the station's SNR\n");
    std::printf("%-8s", "profile");
    const double snrs[] = {3, 10, 20, 30};
    for (const double snr : snrs) std::printf("   %2.0f dB: speech pause", snr);
    std::printf("\n");
    std::vector<std::vector<std::pair<double, double>>> table(kVariants.size());
    for (const double snr : snrs) {
        const size_t length = static_cast<size_t>(20 * kRate);
        std::vector<cfloat> scene(length);
        std::vector<float> envelope(length, 0.0f);
        std::mt19937 random(9);
        const std::vector<Span> spans = speak(random, envelope, 0, length);
        const Voice voice(4);
        const double level = amplitude_for(snr, 2400.0);
        for (size_t n = 0; n < length; n++) scene[n] = static_cast<float>(level * envelope[n]) * voice.at(n, kOffset);
        add_noise(scene, 13);
        double rate = 0;
        const std::vector<Heard> heard = play(scene, Mode::Usb, rate);
        for (size_t p = 0; p < heard.size(); p++) {
            std::vector<double> speech, pause;
            for (size_t i = 0; i < spans.size(); i++) {
                if (static_cast<double>(spans[i].begin) / kRate < 3.0) continue;
                speech.push_back(db(power_over(heard[p], rate, spans[i])));
                if (i + 1 < spans.size() && spans[i + 1].begin > spans[i].end + static_cast<size_t>(0.3 * kRate)) {
                    pause.push_back(db(power_over(heard[p], rate, {spans[i].end, spans[i + 1].begin})));
                }
            }
            table[p].push_back({median(speech), median(pause)});
        }
    }
    for (size_t p = 0; p < kVariants.size(); p++) {
        std::printf("%-8s", kVariants[p].label.c_str());
        for (const auto& [speech, pause] : table[p]) std::printf("   %12.1f %5.1f", speech, pause);
        std::printf("\n");
    }
}

// PARIS at `wpm`: marks of 1 or 3 dits, 1 dit apart, 3 between letters and 7
// between words, with 5 ms edges. Returns the marks.
std::vector<Span> key(std::vector<float>& envelope, size_t from, size_t to, double wpm, double level) {
    const size_t dit = static_cast<size_t>(1.2 / wpm * kRate), edge = static_cast<size_t>(0.005 * kRate);
    const char* word[] = {".--.", ".-", ".-.", "..", "..."};
    std::vector<Span> marks;
    size_t at = from;
    while (at + 60 * dit < to) {
        for (const char* letter : word) {
            for (const char* e = letter; *e; e++) {
                const size_t n = (*e == '-' ? 3 : 1) * dit;
                for (size_t i = 0; i < n; i++) {
                    const double rise = std::min({1.0, static_cast<double>(i) / edge, static_cast<double>(n - i) / edge});
                    envelope[at + i] = static_cast<float>(level * 0.5 * (1.0 - std::cos(kPi * rise)));
                }
                marks.push_back({at, at + n});
                at += n + dit;
            }
            at += 2 * dit;
        }
        at += 4 * dit;
    }
    return marks;
}

void cw(double wpm) {
    std::printf("\ncw: PARIS at %.0f WPM, 20 dB over the noise in 500 Hz; from 20 s a neighbour 20 dB stronger\n"
                "keys 150 Hz away at 18 WPM for a few words. Marks are read on the 700 Hz tone, gaps as all\n"
                "they hold; \"after it\" is from the neighbour's last mark.\n", wpm);
    std::printf("%-8s %13s %13s %15s %14s\n", "profile", "mark spread", "gap dB", "marks with", "after it, s");
    const size_t length = static_cast<size_t>(40 * kRate);
    std::vector<float> wanted(length, 0.0f), neighbour(length, 0.0f);
    const std::vector<Span> marks = key(wanted, 0, length, wpm, 1.0);
    const std::vector<Span> theirs = key(neighbour, static_cast<size_t>(20 * kRate), static_cast<size_t>(32 * kRate), 18.0, 1.0);
    const double their_end = static_cast<double>(theirs.back().end) / kRate;
    const double level = amplitude_for(20.0, 500.0);
    std::vector<cfloat> scene(length);
    for (size_t n = 0; n < length; n++) {
        const double t = static_cast<double>(n) / kRate;
        // The wanted carrier lands on the 700 Hz pitch, the neighbour on 850.
        const std::complex<double> a = level * wanted[n] * std::polar(1.0, 2.0 * kPi * (kOffset + 700.0) * t);
        const std::complex<double> b = level * 10.0 * neighbour[n] * std::polar(1.0, 2.0 * kPi * (kOffset + 850.0) * t);
        scene[n] = cfloat(static_cast<float>((a + b).real()), static_cast<float>((a + b).imag()));
    }
    add_noise(scene, 14);
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Cw, rate)) {
        std::vector<double> alone, beside, whole, gaps;
        std::vector<std::pair<double, double>> after;
        for (size_t i = 0; i < marks.size(); i++) {
            const double t = static_cast<double>(marks[i].begin) / kRate;
            const double tone = db(tone_over(heard, rate, marks[i], 700.0, 0.008));
            if (t > 3.0 && t < 20.0) {
                alone.push_back(tone);
                whole.push_back(db(power_over(heard, rate, marks[i], 0.008)));
                if (i + 1 < marks.size() && marks[i + 1].begin > marks[i].end + static_cast<size_t>(0.2 * kRate)) {
                    gaps.push_back(db(power_over(heard, rate, {marks[i].end, marks[i + 1].begin}, 0.02)));
                }
            } else if (t > 21.0 && t < their_end) {
                beside.push_back(tone);
            } else if (t > their_end) {
                after.push_back({t, tone});
            }
        }
        const double reference = median(alone);
        double spread = 0;
        for (const double l : alone) spread += (l - reference) * (l - reference);
        spread = alone.empty() ? 0 : std::sqrt(spread / static_cast<double>(alone.size()));
        double back = 8.0;
        for (const auto& [t, tone] : after) {
            if (tone > reference - 3.0) {
                back = t - their_end;
                break;
            }
        }
        std::printf("%-8s %12.1f %13.1f %14.1f %14.2f\n", heard.label.c_str(), spread,
                    median(gaps) - median(whole), median(beside) - reference, back);
    }
}

void am() {
    std::printf("\nam: carrier 30 dB over the noise in 9 kHz, 80%% at 400 Hz, down 12 dB over 2 s at 8 s,\n"
                "back over 2 s at 14 s. Levels of the 400 Hz tone: before, faded and after, and the widest\n"
                "departure from before, in 100 ms windows from 3 s on.\n");
    std::printf("%-8s %8s %8s %8s %8s %10s %9s\n", "profile", "before", "faded", "after", "widest", "THD", "< 50 Hz");
    const size_t length = static_cast<size_t>(22 * kRate);
    const double level = amplitude_for(30.0, 9000.0);
    std::vector<cfloat> scene(length);
    const auto fade = [](double t) {
        if (t < 8.0 || t > 16.0) return 1.0;
        if (t < 10.0) return std::pow(10.0, -12.0 * (t - 8.0) / 2.0 / 20.0);
        if (t < 14.0) return std::pow(10.0, -12.0 / 20.0);
        return std::pow(10.0, -12.0 * (16.0 - t) / 2.0 / 20.0);
    };
    for (size_t n = 0; n < length; n++) {
        const double t = static_cast<double>(n) / kRate;
        const std::complex<double> s = level * fade(t) * (1.0 + 0.8 * std::cos(2.0 * kPi * 400.0 * t)) *
                                       std::polar(1.0, 2.0 * kPi * kOffset * t);
        scene[n] = cfloat(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    }
    add_noise(scene, 15);
    double rate = 0;
    const bool series = std::getenv("AGC_LAB_SERIES") != nullptr;
    const auto seconds = [](double from, double to) {
        return Span{static_cast<size_t>(from * kRate), static_cast<size_t>(to * kRate)};
    };
    for (const Heard& heard : play(scene, Mode::Am, rate)) {
        const double before = db(tone_over(heard, rate, seconds(5, 7.5), 400.0, 0));
        const double faded = db(tone_over(heard, rate, seconds(11, 13.5), 400.0, 0));
        const double after = db(tone_over(heard, rate, seconds(19, 21.5), 400.0, 0));
        double widest = 0;
        std::string trace;
        for (double t = 3.0; t < 21.5; t += 0.1) {
            const double l = db(tone_over(heard, rate, seconds(t, t + 0.1), 400.0, 0));
            if (std::fabs(l - before) > std::fabs(widest)) widest = l - before;
            if (series && std::fmod(t + 1e-9, 0.5) < 0.1) trace += " " + std::to_string(static_cast<int>(std::lround(l - before)));
        }
        const Span second = seconds(5, 6);
        const double fundamental = tone_over(heard, rate, second, 400.0, 0);
        double harmonics = 0, rumble = 0;
        for (int h = 2; h <= 5; h++) harmonics += tone_over(heard, rate, second, 400.0 * h, 0);
        for (int hz = 2; hz < 50; hz++) rumble += tone_over(heard, rate, second, hz, 0);
        std::printf("%-8s %8.1f %8.1f %8.1f %8.1f %9.2f%% %9.1f\n", heard.label.c_str(), before,
                    faded - before, after - before, widest, 100.0 * std::sqrt(harmonics / fundamental),
                    db(rumble) - db(fundamental));
        if (series) std::printf("   every 0.5 s from 3 s, dB from before:%s\n", trace.c_str());
    }
}

void am_depth() {
    std::printf("\nam depth: carrier 30 dB over the noise in 9 kHz, no fading; THD of the demodulated tone\n"
                "at each depth and pitch, and its level against 80%% at 400 Hz\n");
    struct Case {
        double depth, hz;
    };
    const Case cases[] = {{0.3, 400}, {0.8, 400}, {1.0, 400}, {0.8, 80}, {1.0, 80}};
    std::printf("%-8s", "profile");
    for (const Case& c : cases) std::printf("  %3.0f%% %3.0f Hz: THD level", 100 * c.depth, c.hz);
    std::printf("\n");
    std::vector<std::vector<std::pair<double, double>>> table(kVariants.size());
    std::vector<double> reference(kVariants.size(), 0.0);
    const size_t length = static_cast<size_t>(8 * kRate);
    const double level = amplitude_for(30.0, 9000.0);
    for (const Case& c : cases) {
        std::vector<cfloat> scene(length);
        for (size_t n = 0; n < length; n++) {
            const double t = static_cast<double>(n) / kRate;
            const std::complex<double> v = level * (1.0 + c.depth * std::cos(2.0 * kPi * c.hz * t)) *
                                           std::polar(1.0, 2.0 * kPi * kOffset * t);
            scene[n] = cfloat(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        add_noise(scene, 16);
        double rate = 0;
        const std::vector<Heard> heard = play(scene, Mode::Am, rate);
        const Span window = {static_cast<size_t>(4 * kRate), static_cast<size_t>(7 * kRate)};
        for (size_t p = 0; p < heard.size(); p++) {
            const double fundamental = tone_over(heard[p], rate, window, c.hz, 0);
            double harmonics = 0;
            for (int h = 2; h <= 6; h++) harmonics += tone_over(heard[p], rate, window, c.hz * h, 0);
            if (c.depth == 0.8 && c.hz == 400) reference[p] = db(fundamental);
            table[p].push_back({100.0 * std::sqrt(harmonics / fundamental), db(fundamental)});
        }
    }
    for (size_t p = 0; p < kVariants.size(); p++) {
        std::printf("%-8s", kVariants[p].label.c_str());
        for (const auto& [thd, lvl] : table[p]) std::printf("  %13.2f%% %5.1f", thd, lvl - reference[p]);
        std::printf("\n");
    }
}

void digital() {
    std::printf("\ndigital: a steady tone at 1500 Hz, 15 dB over the noise in 2.4 kHz, as RTTY's mark or one of\n"
                "FT8's tones; the gain may not move under it. Sidebands 5 to 50 Hz either side, and the\n"
                "spread of its level in 20 ms windows\n");
    std::printf("%-8s %13s %10s\n", "profile", "sidebands dB", "spread dB");
    const size_t length = static_cast<size_t>(12 * kRate);
    const double level = amplitude_for(15.0, 2400.0);
    std::vector<cfloat> scene(length);
    for (size_t n = 0; n < length; n++) {
        const double t = static_cast<double>(n) / kRate;
        const std::complex<double> v = level * std::polar(1.0, 2.0 * kPi * (kOffset + 1500.0) * t);
        scene[n] = cfloat(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    add_noise(scene, 18);
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        const Span window = {static_cast<size_t>(4 * kRate), static_cast<size_t>(10 * kRate)};
        const double tone = tone_over(heard, rate, window, 1500.0, 0);
        double sidebands = 0;
        for (int k = 5; k <= 50; k++) {
            sidebands += tone_over(heard, rate, window, 1500.0 + k, 0) + tone_over(heard, rate, window, 1500.0 - k, 0);
        }
        std::vector<double> levels;
        for (double t = 4.0; t < 10.0; t += 0.02) {
            levels.push_back(db(tone_over(heard, rate, {static_cast<size_t>(t * kRate), static_cast<size_t>((t + 0.02) * kRate)},
                                          1500.0, 0)));
        }
        const double mid = median(levels);
        double spread = 0;
        for (const double l : levels) spread += (l - mid) * (l - mid);
        spread = std::sqrt(spread / static_cast<double>(levels.size()));
        std::printf("%-8s %13.1f %10.2f\n", heard.label.c_str(), db(sidebands) - db(tone), spread);
    }
}

void qso(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "qso: cannot open %s\n", path.c_str());
        return;
    }
    // Played at kOffset like every scene: the recording's station is 14.45 kHz
    // under its centre, so it is shifted up by that and the offset.
    std::vector<cfloat> scene;
    int16_t pair[2];
    const double shift = 14450.0 + kOffset;
    for (size_t n = 0; std::fread(pair, sizeof(pair), 1, file) == 1; n++) {
        const cfloat raw(static_cast<float>(pair[1]) / 32768.0f, static_cast<float>(pair[0]) / 32768.0f);
        const double angle = 2.0 * kPi * shift * static_cast<double>(n) / kRate;
        scene.push_back(raw * cfloat(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))));
    }
    std::fclose(file);
    std::printf("\nqso: %s, %.0f s\n%-8s %10s %8s %10s\n", path.c_str(), static_cast<double>(scene.size()) / kRate,
                "profile", "contrast", "wander", "speech dB");
    double rate = 0;
    for (const Heard& heard : play(scene, Mode::Usb, rate)) {
        const size_t frame = static_cast<size_t>(0.032 * rate), start = static_cast<size_t>(3 * rate);
        std::vector<double> levels;
        for (size_t at = start; at + frame <= heard.audio.size(); at += frame) {
            double sum = 0;
            for (size_t i = at; i < at + frame; i++) sum += static_cast<double>(heard.audio[i]) * heard.audio[i];
            levels.push_back(db(sum / static_cast<double>(frame)));
        }
        std::sort(levels.begin(), levels.end());
        const auto at = [&](double p) { return levels[static_cast<size_t>(p / 100.0 * static_cast<double>(levels.size() - 1))]; };
        std::vector<double> quiet(levels.begin(), levels.begin() + static_cast<long>(levels.size() / 4));
        double mean = 0, spread = 0;
        for (const double l : quiet) mean += l;
        mean /= static_cast<double>(quiet.size());
        for (const double l : quiet) spread += (l - mean) * (l - mean);
        spread = std::sqrt(spread / static_cast<double>(quiet.size()));
        std::printf("%-8s %10.1f %8.1f %10.1f\n", heard.label.c_str(), at(95) - at(5), spread, at(90));
    }
}

void delays() {
    std::printf("\nchain delay, ms\n%-8s %8s %8s %8s\n", "profile", "usb", "cw", "am");
    const std::vector<double> usb = chain_delays(Mode::Usb, 1000.0), cw = chain_delays(Mode::Cw, 700.0),
                              am = chain_delays(Mode::Am, 1000.0);
    for (size_t p = 0; p < kVariants.size(); p++) {
        std::printf("%-8s %8.1f %8.1f %8.1f\n", kVariants[p].label.c_str(), 1000 * usb[p], 1000 * cw[p], 1000 * am[p]);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "talkers") talkers();
    if (only.empty() || only == "qsb") qsb();
    if (only.empty() || only == "pauses") pauses();
    if (only.empty() || only == "qrn") qrn();
    if (only.empty() || only == "weak") weak();
    if (only.empty() || only == "cw") cw(25);
    if (only == "cw-speeds") {
        cw(15);
        cw(35);
    }
    if (only.empty() || only == "am") am();
    if (only.empty() || only == "am-depth") am_depth();
    if (only.empty() || only == "digital") digital();
    if (only.empty() || only == "delays") delays();
    if (only == "qso") {
        for (int i = 2; i < argc; i++) qso(argv[i]);
    }
    return 0;
}
