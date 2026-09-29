// One second-order filter section, as audio filters are built from.
//
// Coefficients from the Audio EQ Cookbook (Bristow-Johnson). Transposed direct
// form II, in double: at an audio rate a high-pass at 50 Hz puts its poles
// close to 1, where single precision starts to show as a raised noise floor.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fernsdr {

class Biquad {
public:
    // A Butterworth high-pass: flat above `cutoff_hz`, -3 dB at it, falling
    // 12 dB an octave below it. A cutoff of zero or less passes everything.
    //
    // Changed while audio plays, the old response fades into the new one over
    // 20 ms. New coefficients under the old memory describe a filter that
    // never ran, and on a low tone the step that makes is many times what the
    // tone itself moves in a sample: a click, each time a listener picks
    // another low cut.
    void set_highpass(double sample_rate, double cutoff_hz) {
        const bool active = cutoff_hz > 0.0 && sample_rate > 0.0 && cutoff_hz < sample_rate / 2.0;
        if (sample_rate == sample_rate_ && active == now_.active && (!active || cutoff_hz == cutoff_hz_)) return;
        sample_rate_ = sample_rate;
        cutoff_hz_ = active ? cutoff_hz : 0.0;

        Section next;
        if (active) {
            const double w0 = 2.0 * 3.14159265358979323846 * cutoff_hz / sample_rate;
            const double cos_w0 = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440);
            const double a0 = 1.0 + alpha;
            next.active = true;
            next.b0 = (1.0 + cos_w0) / 2.0 / a0;
            next.b1 = -(1.0 + cos_w0) / a0;
            next.b2 = next.b0;
            next.a1 = -2.0 * cos_w0 / a0;
            next.a2 = (1.0 - alpha) / a0;
        }
        install(next, sample_rate);
    }

    // A notch: nothing at `center_hz`, and within `bandwidth_hz` around it a
    // dip, flat elsewhere. A centre of zero or less passes everything. Moves
    // fade the way the high-pass's do.
    void set_notch(double sample_rate, double center_hz, double bandwidth_hz) {
        const bool active = center_hz > 0.0 && bandwidth_hz > 0.0 && sample_rate > 0.0 && center_hz < sample_rate / 2.0;
        if (sample_rate == sample_rate_ && active == now_.active &&
            (!active || (center_hz == cutoff_hz_ && bandwidth_hz == bandwidth_hz_))) {
            return;
        }
        sample_rate_ = sample_rate;
        cutoff_hz_ = active ? center_hz : 0.0;
        bandwidth_hz_ = bandwidth_hz;
        Section next;
        if (active) {
            const double w0 = 2.0 * 3.14159265358979323846 * center_hz / sample_rate;
            const double cos_w0 = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * (center_hz / bandwidth_hz));
            const double a0 = 1.0 + alpha;
            next.active = true;
            next.b0 = 1.0 / a0;
            next.b1 = -2.0 * cos_w0 / a0;
            next.b2 = next.b0;
            next.a1 = next.b1;
            next.a2 = (1.0 - alpha) / a0;
        }
        install(next, sample_rate);
    }

    bool active() const { return now_.active; }

    void process(float* samples, size_t count) {
        playing_ = true;
        size_t i = 0;
        for (; i < count && fade_left_ > 0; i++, fade_left_--) {
            const double x = samples[i];
            const double to = now_.step(x);
            const double from = before_.step(x);
            const double weight = 1.0 - static_cast<double>(fade_left_) / static_cast<double>(fade_length_);
            samples[i] = static_cast<float>(from + weight * (to - from));
        }
        if (now_.active) {
            for (; i < count; i++) samples[i] = static_cast<float>(now_.step(samples[i]));
        }
        // Denormal protection: decays to exactly zero on silence rather than
        // crawling through subnormal numbers, which are slow on some CPUs.
        now_.flush();
        before_.flush();
    }

    // Forgets the audio: what comes next is not a continuation, so a change
    // before it takes effect at once rather than fading.
    void reset() {
        now_.z1 = now_.z2 = 0.0;
        fade_left_ = 0;
        playing_ = false;
    }

    // The gain at `hz`, from the coefficients: what the tests check against.
    double magnitude(double sample_rate, double hz) const {
        if (!now_.active) return 1.0;
        const double w = 2.0 * 3.14159265358979323846 * hz / sample_rate;
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = now_.b0 + now_.b1 * c1 + now_.b2 * c2, ni = -(now_.b1 * s1 + now_.b2 * s2);
        const double dr = 1.0 + now_.a1 * c1 + now_.a2 * c2, di = -(now_.a1 * s1 + now_.a2 * s2);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

private:
    struct Section;
    void install(const Section& next, double sample_rate) {
        fade_left_ = 0;
        if (playing_ && sample_rate > 0.0) {
            before_ = now_;
            fade_length_ = std::max<size_t>(1, static_cast<size_t>(sample_rate * 0.02));
            fade_left_ = fade_length_;
        }
        now_ = next;
    }

    struct Section {
        bool active = false;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double step(double x) {
            if (!active) return x;
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void flush() {
            if (std::fabs(z1) < 1e-30) z1 = 0.0;
            if (std::fabs(z2) < 1e-30) z2 = 0.0;
        }
    };

    Section now_;
    // What was running before the last change, while it fades out.
    Section before_;
    size_t fade_left_ = 0;
    size_t fade_length_ = 1;
    double sample_rate_ = 0.0;
    double cutoff_hz_ = 0.0;
    double bandwidth_hz_ = 0.0;
    // Audio has gone through since the last reset, so a change is heard.
    bool playing_ = false;
};

}  // namespace fernsdr
