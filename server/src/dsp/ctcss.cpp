#include "ctcss.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Two seconds of audio for each measurement: the closest standard tones are
// 2.4 Hz apart, and with a Hann window a block this long keeps them more
// than 30 dB apart. Measured twice a second.
constexpr double kBlockSeconds = 2.0;
constexpr double kHopSeconds = 0.5;
constexpr double kLowestHz = 60.0;
constexpr double kHighestHz = 260.0;
// Share of the energy between 60 and 260 Hz that has to sit at the peak to
// call it a tone. A tone alone gives about 0.9 after the window's spread;
// noise, and voice that leaks below the transmitter's own 300 Hz cut, give
// well under 0.2.
constexpr double kMinimumShare = 0.5;

}  // namespace

const std::vector<double>& CtcssDetector::standard_tones() {
    static const std::vector<double> tones = {
        67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,  94.8,  97.4,  100.0,
        103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7,
        159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
        199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1,
    };
    return tones;
}

// Close enough means nearer to this tone than to either neighbour by a
// clear margin: within 45 % of the gap on each side, and never more than
// 1.5 Hz. An encoder a little off frequency is still named; a measurement
// halfway between two tones is not guessed at.
double CtcssDetector::nearest_standard(double measured_hz) {
    const auto& tones = standard_tones();
    for (size_t i = 0; i < tones.size(); i++) {
        const double below = i > 0 ? tones[i] - tones[i - 1] : 3.0;
        const double above = i + 1 < tones.size() ? tones[i + 1] - tones[i] : 3.0;
        const double reach_down = std::min(1.5, 0.45 * below);
        const double reach_up = std::min(1.5, 0.45 * above);
        if (measured_hz >= tones[i] - reach_down && measured_hz <= tones[i] + reach_up) return tones[i];
    }
    return 0.0;
}

void CtcssDetector::configure(double sample_rate) {
    sample_rate_ = sample_rate;
    decimation_ = std::max<size_t>(1, static_cast<size_t>(std::floor(sample_rate / 1500.0)));
    decimated_rate_ = sample_rate / static_cast<double>(decimation_);

    if (sample_rate > 0.0) {
        // Flat to past the highest tone; down by 60 dB from where anything
        // would fold onto it.
        decimator_.configure(sample_rate, decimation_, kHighestHz + 10.0, decimated_rate_ - (kHighestHz + 10.0), 60.0);
    }

    const size_t block = std::min<size_t>(fft_.size(), static_cast<size_t>(decimated_rate_ * kBlockSeconds));
    ring_.assign(block, 0.0f);
    window_.resize(block);
    for (size_t i = 0; i < block; i++) {
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(block)));
    }
    re_.assign(fft_.size(), 0.0f);
    im_.assign(fft_.size(), 0.0f);
    hop_ = std::max<size_t>(1, static_cast<size_t>(decimated_rate_ * kHopSeconds));
    // The gate looks at the last 0.42 s: a flat window that long has its
    // first null 2.4 Hz from the tone, where the closest standard tones sit,
    // so a neighbour's tone leaves almost nothing in the chosen one's bin.
    gate_samples_ = std::min(ring_.size(), static_cast<size_t>(decimated_rate_ * 0.42));
    gate_hop_ = std::max<size_t>(1, static_cast<size_t>(decimated_rate_ * 0.1));
    since_gate_ = 0;
    gate_misses_ = 0;
    gate_open_ = false;
    written_ = 0;
    since_evaluation_ = 0;
    candidate_hz_ = 0.0;
    agreement_ = 0;
    misses_ = 0;
    reported_hz_ = 0.0;
    measured_precise_ = 0.0;
    candidate_precise_ = 0.0;
}

void CtcssDetector::process(const float* audio, size_t count) {
    if (ring_.empty()) return;
    decimated_.resize(count / decimation_ + 1);
    const size_t kept = decimator_.process(audio, count, decimated_.data());
    for (size_t i = 0; i < kept; i++) {
        ring_[written_ % ring_.size()] = decimated_[i];
        written_++;
        if (++since_evaluation_ >= hop_ && written_ >= ring_.size()) {
            since_evaluation_ = 0;
            evaluate();
        }
        if (squelch_tone_ > 0.0 && ++since_gate_ >= gate_hop_ && written_ >= gate_samples_) {
            since_gate_ = 0;
            evaluate_gate();
        }
    }
}

void CtcssDetector::evaluate() {
    const size_t block = ring_.size();
    const size_t start = written_ % block;  // the oldest sample
    for (size_t i = 0; i < block; i++) re_[i] = ring_[(start + i) % block] * window_[i];
    std::fill(re_.begin() + static_cast<std::ptrdiff_t>(block), re_.end(), 0.0f);
    std::fill(im_.begin(), im_.end(), 0.0f);
    fft_.forward(re_.data(), im_.data());
    const auto power_at = [&](size_t k) {
        return static_cast<double>(re_[k]) * re_[k] + static_cast<double>(im_[k]) * im_[k];
    };

    const double bin_hz = decimated_rate_ / static_cast<double>(fft_.size());
    const size_t first = static_cast<size_t>(std::ceil(kLowestHz / bin_hz));
    const size_t last = std::min(fft_.size() / 2 - 2, static_cast<size_t>(std::floor(kHighestHz / bin_hz)));
    double total = 0.0;
    double peak_power = 0.0;
    size_t peak = first;
    for (size_t k = first; k <= last; k++) {
        const double power = power_at(k);
        total += power;
        if (power > peak_power) {
            peak_power = power;
            peak = k;
        }
    }

    double measured = 0.0;
    double precise = 0.0;
    if (total > 0.0 && peak > first && peak < last) {
        // The zero padding spreads a tone over a few bins; count them all.
        double around = 0.0;
        const size_t spread = static_cast<size_t>(std::ceil(1.5 / bin_hz));
        for (size_t k = peak - std::min(peak - first, spread); k <= std::min(last, peak + spread); k++) {
            around += power_at(k);
        }
        if (around / total >= kMinimumShare) {
            // Where between the bins the peak lies, from the parabola through
            // the logarithms of the three around it.
            const double a = std::log(power_at(peak - 1) + 1e-30);
            const double b = std::log(peak_power + 1e-30);
            const double c = std::log(power_at(peak + 1) + 1e-30);
            const double denominator = a - 2.0 * b + c;
            const double offset = denominator != 0.0 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
            precise = (static_cast<double>(peak) + offset) * bin_hz;
            measured = nearest_standard(precise);
        }
    }

    // Named after two measurements agree, and dropped after two that find
    // nothing: a fade or a word must not make the readout flicker.
    if (measured > 0.0) {
        misses_ = 0;
        agreement_ = measured == candidate_hz_ ? agreement_ + 1 : 1;
        candidate_hz_ = measured;
        // Averaged over the measurements that agree, which steadies it.
        candidate_precise_ = agreement_ == 1 ? precise : 0.7 * candidate_precise_ + 0.3 * precise;
        if (agreement_ >= 2) {
            reported_hz_ = measured;
            measured_precise_ = candidate_precise_;
        }
    } else if (++misses_ >= 2) {
        candidate_hz_ = 0.0;
        agreement_ = 0;
        reported_hz_ = 0.0;
    }
}

void CtcssDetector::set_squelch_tone(double hz) {
    const auto& tones = standard_tones();
    const auto found = std::find(tones.begin(), tones.end(), hz);
    const double tone = found != tones.end() ? hz : 0.0;
    if (tone == squelch_tone_) return;
    squelch_tone_ = tone;
    gate_open_ = false;
    gate_misses_ = 0;
    since_gate_ = 0;
    if (tone <= 0.0) return;
    const size_t index = static_cast<size_t>(found - tones.begin());
    // Beyond the ends, a tone as far away as the nearest neighbour.
    gate_neighbours_[0] = index > 0 ? tones[index - 1] : tone - (tones[1] - tones[0]);
    gate_neighbours_[1] = index + 1 < tones.size() ? tones[index + 1] : tone + (tones[index] - tones[index - 1]);
}

double CtcssDetector::goertzel_power(double hz, size_t samples) const {
    const double coefficient = 2.0 * std::cos(2.0 * kPi * hz / decimated_rate_);
    double s1 = 0.0, s2 = 0.0;
    const size_t block = ring_.size();
    const size_t start = (written_ + block - samples) % block;
    for (size_t i = 0; i < samples; i++) {
        const double s0 = ring_[(start + i) % block] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}

void CtcssDetector::evaluate_gate() {
    const size_t n = gate_samples_;
    const double tone = goertzel_power(squelch_tone_, n);
    const double below = goertzel_power(gate_neighbours_[0], n);
    const double above = goertzel_power(gate_neighbours_[1], n);
    // And against what lies around it, to tell a tone from a loud nothing:
    // noise, or voice leaking under the transmitter's 300 Hz cut, is spread
    // out, a tone is not. Another station's different tone near one of these
    // points raises the reference and keeps the gate shut, as it should.
    double around = 0.0;
    int references = 0;
    for (double offset : {-45.0, -25.0, 25.0, 45.0}) {
        const double hz = squelch_tone_ + offset;
        if (hz < 55.0 || hz > 280.0) continue;
        around += goertzel_power(hz, n);
        references++;
    }
    around = references > 0 ? around / references : 0.0;
    const bool present = tone > 10.0 * around && tone > 4.0 * below && tone > 4.0 * above;
    if (present) {
        gate_open_ = true;
        gate_misses_ = 0;
    } else if (++gate_misses_ >= 2) {
        gate_open_ = false;
    }
}

}  // namespace fernsdr
