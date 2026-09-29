#include "fir_decimator.h"

#include <algorithm>
#include <cmath>

#include "simd.h"

namespace fernsdr {

namespace {

// The zeroth-order modified Bessel function, by its series; converges fast
// for the arguments a Kaiser window uses.
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

}  // namespace

std::vector<float> design_lowpass(double input_rate, double pass_hz, double stop_hz, double stop_db,
                                  double deemphasis_us) {
    // Kaiser's estimates for the window's shape and length (Oppenheim and
    // Schafer, 7.6): beta from the attenuation, length from it and the
    // transition width in radians.
    const double attenuation = std::max(21.0, stop_db);
    const double beta = attenuation > 50.0 ? 0.1102 * (attenuation - 8.7)
                                           : 0.5842 * std::pow(attenuation - 21.0, 0.4) + 0.07886 * (attenuation - 21.0);
    const double transition = 2.0 * M_PI * std::max(1.0, stop_hz - pass_hz) / input_rate;
    size_t length = static_cast<size_t>(std::ceil((attenuation - 8.0) / (2.285 * transition))) + 1;
    length |= 1;  // odd, so the delay is a whole number of samples
    const double cutoff = (pass_hz + stop_hz) / 2.0 / input_rate;  // cycles per sample
    const double middle = (static_cast<double>(length) - 1.0) / 2.0;
    const double norm = bessel_i0(beta);
    std::vector<double> taps(length);
    for (size_t i = 0; i < length; i++) {
        const double t = static_cast<double>(i) - middle;
        const double sinc = t == 0.0 ? 2.0 * cutoff : std::sin(2.0 * M_PI * cutoff * t) / (M_PI * t);
        const double r = t / middle;
        const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
        taps[i] = sinc * window;
    }
    if (deemphasis_us > 0.0) {
        // The one-pole response sampled at this rate, e^(-m / (rate tau)),
        // to eight time constants, convolved with the low pass.
        const double samples_per_tau = input_rate * deemphasis_us * 1e-6;
        const size_t tail = static_cast<size_t>(std::ceil(8.0 * samples_per_tau)) + 1;
        std::vector<double> combined(length + tail - 1, 0.0);
        for (size_t m = 0; m < tail; m++) {
            const double weight = std::exp(-static_cast<double>(m) / samples_per_tau);
            for (size_t i = 0; i < length; i++) combined[i + m] += weight * taps[i];
        }
        taps.swap(combined);
    }
    // Unity gain at DC, whatever the window did to the sum.
    double sum = 0.0;
    for (const double tap : taps) sum += tap;
    // Stored backwards: process() multiplies the oldest input of a window
    // by the first tap, and the taps are no longer symmetric once the
    // de-emphasis is in them.
    std::vector<float> reversed(taps.size());
    for (size_t i = 0; i < taps.size(); i++) reversed[taps.size() - 1 - i] = static_cast<float>(taps[i] / sum);
    return reversed;
}

void FirDecimator::configure(double input_rate, size_t factor, double pass_hz, double stop_hz, double stop_db,
                             double deemphasis_us) {
    factor_ = std::max<size_t>(1, factor);
    output_rate_ = input_rate / static_cast<double>(factor_);
    taps_ = design_lowpass(input_rate, pass_hz, stop_hz, stop_db, deemphasis_us);
    reset();
}

void FirDecimator::reset() {
    history_.assign(taps_.empty() ? 0 : taps_.size() - 1, 0.0f);
    phase_ = 0;
}

size_t FirDecimator::process(const float* in, size_t count, float* out) {
    if (taps_.empty()) return 0;
    const size_t kept = taps_.size() - 1;
    history_.resize(kept);
    history_.insert(history_.end(), in, in + count);
    const float* taps = taps_.data();
    const size_t length = taps_.size();
    size_t produced = 0;
    // Output n uses the `length` inputs ending at input position `end`,
    // oldest first against the reversed taps.
    size_t end = phase_;
    for (; end < count; end += factor_) {
        out[produced++] = simd::dot(history_.data() + end, taps, length);
    }
    phase_ = end - count;
    history_.erase(history_.begin(), history_.begin() + static_cast<long>(count));
    return produced;
}

void BandDecimator::configure(double input_rate, size_t factor, double centre_hz, double pass_hz, double stop_hz,
                              double stop_db) {
    factor_ = std::max<size_t>(1, factor);
    output_rate_ = input_rate / static_cast<double>(factor_);
    const std::vector<float> taps = design_lowpass(input_rate, pass_hz, stop_hz, stop_db);
    const size_t length = taps.size();
    const double w = 2.0 * M_PI * centre_hz / input_rate;
    // taps[i] is h[length - 1 - i]; it meets the input length - 1 - i
    // samples before the window's newest, which is where h's shift is taken.
    taps_i_.resize(length);
    taps_q_.resize(length);
    for (size_t i = 0; i < length; i++) {
        const double k = static_cast<double>(length - 1 - i);
        taps_i_[i] = static_cast<float>(taps[i] * std::cos(w * k));
        taps_q_[i] = static_cast<float>(taps[i] * std::sin(w * k));
    }
    step_ = std::polar(1.0, -w * static_cast<double>(factor_));
    turn_ = 1.0;
    history_.assign(length - 1, 0.0f);
    phase_ = 0;
}

size_t BandDecimator::process(const float* in, size_t count, std::complex<float>* out) {
    if (taps_i_.empty()) return 0;
    const size_t length = taps_i_.size();
    history_.resize(length - 1);
    history_.insert(history_.end(), in, in + count);
    size_t produced = 0;
    size_t end = phase_;
    for (; end < count; end += factor_) {
        const float* window = history_.data() + end;
        float a = 0.0f, b = 0.0f;
        simd::dot2(window, taps_i_.data(), taps_q_.data(), length, &a, &b);
        // Written out: std::complex's operator* checks every product for
        // NaNs, which costs more here than the product.
        const double c = turn_.real(), s = turn_.imag();
        out[produced++] = std::complex<float>(static_cast<float>(a * c - b * s), static_cast<float>(a * s + b * c));
        turn_ = std::complex<double>(c * step_.real() - s * step_.imag(), c * step_.imag() + s * step_.real());
    }
    // The phasor's length drifts by rounding, a little each step: set back
    // to one once a block, where a single correction is exact enough.
    turn_ /= std::abs(turn_);
    phase_ = end - count;
    history_.erase(history_.begin(), history_.begin() + static_cast<long>(count));
    return produced;
}

}  // namespace fernsdr
