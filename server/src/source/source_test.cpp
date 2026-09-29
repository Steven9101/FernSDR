#include "source_test.h"

#include <chrono>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

namespace fernsdr {

namespace {

constexpr double kTwoPi = 6.283185307179586;

struct SyntheticSignal {
    double offset_hz;   // from band centre
    double amplitude;
    enum class Kind { Carrier, Am, Ssb, Cw } kind;
    double tone_hz;     // modulating tone
    double keying_hz;   // for CW: on/off rate
};

struct Oscillator {
    double re = 1, im = 0;
    double step_re = 1, step_im = 0;

    void configure(double frequency, double rate) {
        step_re = std::cos(kTwoPi * frequency / rate);
        step_im = std::sin(kTwoPi * frequency / rate);
    }
    void next() {
        const double previous_re = re;
        re = re * step_re - im * step_im;
        im = previous_re * step_im + im * step_re;
    }
    void normalise() {
        const double scale = 1.0 / std::hypot(re, im);
        re *= scale;
        im *= scale;
    }
};

class TestSource : public Source {
public:
    TestSource(double sample_rate, double center_hz, double noise_amplitude, bool realtime,
               SignalKind kind)
        : sample_rate_(sample_rate),
          center_hz_(center_hz),
          noise_(noise_amplitude),
          realtime_(realtime),
          kind_(kind) {
        // A spread of signal types across the band, so every demodulator and
        // the waterfall all have something recognisable to show.
        signals_ = {
            {-60000.0, 0.30, SyntheticSignal::Kind::Am, 1000.0, 0.0},
            {-25000.0, 0.15, SyntheticSignal::Kind::Ssb, 800.0, 0.0},
            {  -800.0, 0.20, SyntheticSignal::Kind::Carrier, 0.0, 0.0},
            { 15000.0, 0.10, SyntheticSignal::Kind::Cw, 0.0, 4.0},
            { 42000.0, 0.22, SyntheticSignal::Kind::Ssb, 1600.0, 0.0},
            { 71000.0, 0.06, SyntheticSignal::Kind::Am, 400.0, 0.0},
        };
        real_center_ = sample_rate_ * 0.25;
        carriers_.resize(signals_.size());
        tones_.resize(signals_.size());
        key_phases_.resize(signals_.size());
        for (size_t s = 0; s < signals_.size(); s++) {
            const auto& signal = signals_[s];
            const double frequency = signal.offset_hz +
                (signal.kind == SyntheticSignal::Kind::Ssb ? signal.tone_hz : 0) +
                (kind == SignalKind::Real ? real_center_ : 0);
            carriers_[s].configure(frequency, sample_rate_);
            tones_[s].configure(signal.tone_hz, sample_rate_);
        }
    }

    bool start(std::string& error) override {
        (void)error;
        interrupted_.store(false);
        started_ = std::chrono::steady_clock::now();
        total_samples_ = 0;
        rng_.seed(12345);
        for (auto& oscillator : carriers_) { oscillator.re = 1; oscillator.im = 0; }
        for (auto& oscillator : tones_) { oscillator.re = 1; oscillator.im = 0; }
        std::fill(key_phases_.begin(), key_phases_.end(), 0);
        connected_ = true;
        return true;
    }

    void interrupt() override { interrupted_.store(true); }
    void stop() override { interrupt(); connected_ = false; }

    SignalKind kind() const override { return kind_; }

    bool read(cfloat* out, size_t count) override {
        if (kind_ != SignalKind::Iq || interrupted_.load()) return false;
        std::normal_distribution<float> noise(0.0f, static_cast<float>(noise_ > 0 ? noise_ : 1));
        for (size_t i = 0; i < count; i++) {
            cfloat sample = noise_ > 0 ? cfloat(noise(rng_), noise(rng_)) : cfloat{};
            for (size_t s = 0; s < signals_.size(); s++) {
                const double amplitude = advance(s);
                auto& carrier = carriers_[s];
                carrier.next();
                sample += cfloat(static_cast<float>(amplitude * carrier.re),
                                 static_cast<float>(amplitude * carrier.im));
            }
            out[i] = sample;
        }
        finish_block(count);
        if (realtime_) pace();
        return !interrupted_.load();
    }

    bool read_real(float* out, size_t count) override {
        if (kind_ != SignalKind::Real || interrupted_.load()) return false;
        std::normal_distribution<float> noise(0.0f, static_cast<float>(noise_ > 0 ? noise_ : 1));
        for (size_t i = 0; i < count; i++) {
            float sample = noise_ > 0 ? noise(rng_) : 0;
            for (size_t s = 0; s < signals_.size(); s++) {
                const double amplitude = advance(s);
                carriers_[s].next();
                sample += static_cast<float>(amplitude * carriers_[s].re);
            }
            out[i] = sample;
        }
        finish_block(count);
        if (realtime_) pace();
        return !interrupted_.load();
    }

private:
    double advance(size_t s) {
        const SyntheticSignal& signal = signals_[s];
        double amplitude = signal.amplitude;

        switch (signal.kind) {
            case SyntheticSignal::Kind::Carrier:
                break;
            case SyntheticSignal::Kind::Am:
                amplitude *= 1.0 + 0.6 * tones_[s].re;
                tones_[s].next();
                break;
            case SyntheticSignal::Kind::Ssb:
                break;
            case SyntheticSignal::Kind::Cw: {
                if (key_phases_[s] >= 0.5) amplitude = 0.0;
                key_phases_[s] += signal.keying_hz / sample_rate_;
                if (key_phases_[s] >= 1) key_phases_[s] -= 1;
                break;
            }
        }

        return amplitude;
    }

    void finish_block(size_t count) {
        // Recursive oscillators replace millions of sin/cos evaluations per
        // second. Renormalise once per block so roundoff cannot change signal
        // levels during a long-running demo. The phase stays continuous.
        for (auto& oscillator : carriers_) oscillator.normalise();
        for (auto& oscillator : tones_) oscillator.normalise();
        total_samples_.fetch_add(count, std::memory_order_relaxed);
    }

public:
    double sample_rate() const override { return sample_rate_; }
    double center_hz() const override { return center_hz_; }
    SourceStats stats() const override { return {total_samples_.load(std::memory_order_relaxed), 0, connected_.load()}; }
    const char* kind_name() const override { return "test"; }

private:
    // Holds the generator to real time so the rest of the system sees the
    // same timing it would from live hardware.
    void pace() {
        const auto target = started_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(total_samples_.load(std::memory_order_relaxed) / sample_rate_));
        while (!interrupted_.load()) {
            const auto remaining = target - std::chrono::steady_clock::now();
            if (remaining <= decltype(remaining)::zero()) break;
            std::this_thread::sleep_for(std::min(remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(100))));
        }
    }

    double sample_rate_;
    double center_hz_;
    double noise_;
    bool realtime_;
    SignalKind kind_;
    // Where the synthetic signals sit for a real source: a quarter of the way
    // up the band, well clear of DC and Nyquist.
    double real_center_ = 0.0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> interrupted_{false};
    std::atomic<uint64_t> total_samples_{0};

    std::vector<SyntheticSignal> signals_;
    std::vector<Oscillator> carriers_, tones_;
    std::vector<double> key_phases_;
    std::mt19937 rng_{12345};
    std::chrono::steady_clock::time_point started_;
};

}  // namespace

std::unique_ptr<Source> make_test_source(const ConfigSection& section, std::string& error) {
    const double sample_rate = section.get_double("sample_rate", 192000.0);
    const double noise = section.get_double("noise", 0.003);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || !std::isfinite(noise) || noise < 0) {
        error = "[" + section.name() + "] sample_rate must be positive and noise must be nonnegative, both finite";
        return nullptr;
    }

    SourceFormat format;
    if (!parse_source_format(section, format, error)) return nullptr;

    return std::make_unique<TestSource>(sample_rate, section.get_double("center", 7100000.0),
                                        noise,
                                        section.get_bool("realtime", true), format.kind);
}

}  // namespace fernsdr
