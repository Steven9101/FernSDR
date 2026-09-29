#include "cw_filter.h"
#include <algorithm>
#include <cmath>

namespace fernsdr {
namespace { constexpr double kPi = 3.14159265358979323846; }

void CwFilter::Bank::configure(const Target& target) {
    this->target = target;
    enabled = target.enabled;
    phase = {1, 0};
    if (!enabled) return;
    const double midpoint = (target.low + target.high) / 2;
    step = std::polar(1.0, 2 * kPi * midpoint / target.rate);
    const double omega = 2 * kPi * ((target.high - target.low) / 2) / target.rate;
    const double cosine = std::cos(omega), sine = std::sin(omega);
    // Ten poles keep a neighbour 100 Hz beyond a 100 Hz filter well below
    // the wanted level even after the default AGC has recovered 60 dB.
    constexpr double q[] = {0.5062325628940014, 0.5611631188171804, 0.7071067811865476,
                            1.101344632292633, 3.196226610749831};
    for (size_t i = 0; i < sections.size(); i++) {
        const double alpha = sine / (2 * q[i]);
        const double a0 = 1 + alpha;
        sections[i] = {(1 - cosine) / (2 * a0), (1 - cosine) / a0,
                       (1 - cosine) / (2 * a0), -2 * cosine / a0, (1 - alpha) / a0, {}, {}};
    }
}

cfloat CwFilter::Bank::process(cfloat input) {
    if (!enabled) return input;
    std::complex<double> value = std::complex<double>(input) * std::conj(phase);
    for (auto& section : sections) {
        const auto output = section.b0 * value + section.z1;
        section.z1 = section.b1 * value - section.a1 * output + section.z2;
        section.z2 = section.b2 * value - section.a2 * output;
        value = output;
    }
    const cfloat output(value * phase);
    phase *= step;
    return output;
}

// The same arithmetic as process(), in the same order, so the two agree to
// the bit; written out rather than through std::complex, whose product checks
// every result for NaN, and with I and Q in the two lanes of one register
// where the sections treat them alike.
void CwFilter::Bank::run(cfloat* samples, size_t count) {
    if (!enabled) return;
#if defined(__GNUC__) && (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
    typedef double Pair __attribute__((vector_size(16)));
    Pair z1[5], z2[5];
    for (size_t s = 0; s < sections.size(); s++) {
        z1[s] = Pair{sections[s].z1.real(), sections[s].z1.imag()};
        z2[s] = Pair{sections[s].z2.real(), sections[s].z2.imag()};
    }
    double pr = phase.real(), pi = phase.imag();
    const double sr = step.real(), si = step.imag();
    for (size_t i = 0; i < count; i++) {
        const cfloat raw = samples[i];
        const bool finite = std::isfinite(raw.real()) && std::isfinite(raw.imag());
        const double xr = finite ? raw.real() : 0.0, xi = finite ? raw.imag() : 0.0;
        // x times the conjugate of the phase.
        Pair value = {xr * pr - xi * -pi, xr * -pi + xi * pr};
        for (size_t s = 0; s < 5; s++) {
            const Section& section = sections[s];
            const Pair output = section.b0 * value + z1[s];
            z1[s] = section.b1 * value - section.a1 * output + z2[s];
            z2[s] = section.b2 * value - section.a2 * output;
            value = output;
        }
        samples[i] = cfloat(static_cast<float>(value[0] * pr - value[1] * pi),
                            static_cast<float>(value[0] * pi + value[1] * pr));
        const double next_r = pr * sr - pi * si, next_i = pr * si + pi * sr;
        pr = next_r;
        pi = next_i;
    }
    for (size_t s = 0; s < sections.size(); s++) {
        sections[s].z1 = {z1[s][0], z1[s][1]};
        sections[s].z2 = {z2[s][0], z2[s][1]};
    }
    phase = {pr, pi};
#else
    for (size_t i = 0; i < count; i++) {
        const cfloat raw = samples[i];
        samples[i] = process(std::isfinite(raw.real()) && std::isfinite(raw.imag()) ? raw : cfloat{});
    }
#endif
}

void CwFilter::Bank::tidy() {
    if (!enabled) return;
    phase /= std::abs(phase);
    for (auto& section : sections) {
        if (std::norm(section.z1) < 1e-60) section.z1 = {};
        if (std::norm(section.z2) < 1e-60) section.z2 = {};
    }
}

void CwFilter::configure(double rate, double low, double high, bool enabled) {
    if (!std::isfinite(rate) || rate < 100 || !std::isfinite(low) || !std::isfinite(high) ||
        high <= low || high - low >= rate * 0.9) enabled = false;
    if (!std::isfinite(rate) || rate < 100 || rate > 384000) {
        rate = 12000;
        enabled = false;
    }
    Target next{rate, enabled ? low : 0, enabled ? high : 0, enabled};
    if (initialized_ && next == requested_) return;
    requested_ = next;
    if (!initialized_) {
        banks_[active_].configure(next);
        initialized_ = true;
    } else if (fade_) {
        // A fast drag replaces the pending target without repeatedly clearing
        // the filter that is currently settling. At most two banks run.
        pending_ = true;
    } else transition();
}

void CwFilter::transition() {
    banks_[active_ ^ 1].configure(requested_);
    fade_length_ = static_cast<size_t>(std::max(1.0, requested_.rate * 0.010));
    fade_ = 1;
    pending_ = false;
}

void CwFilter::process(cfloat* samples, size_t count) {
    if (!initialized_ || (!fade_ && !banks_[active_].enabled)) return;
    for (size_t i = 0; i < count; i++) {
        if (!fade_) {
            banks_[active_].run(samples + i, count - i);
            break;
        }
        const auto raw = samples[i];
        const cfloat input = std::isfinite(raw.real()) && std::isfinite(raw.imag()) ? raw : cfloat{};
        const cfloat old = banks_[active_].process(input);
        if (!fade_) samples[i] = old;
        else {
            const cfloat next = banks_[active_ ^ 1].process(input);
            const float blend = static_cast<float>(fade_) / fade_length_;
            samples[i] = old + blend * (next - old);
            if (++fade_ > fade_length_) {
                active_ ^= 1;
                fade_ = 0;
                if (pending_ && !(banks_[active_].target == requested_)) transition();
                else pending_ = false;
            }
        }
    }
    for (auto& bank : banks_) bank.tidy();
}

void CwFilter::reset() {
    initialized_ = false;
    pending_ = false;
    fade_ = 0;
    active_ = 0;
}
}
