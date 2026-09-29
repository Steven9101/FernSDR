#include "demod.h"

#include "simd.h"

#include "magnitude.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

std::string lowercase(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

bool mode_from_name(const std::string& name, Mode& out) {
    const std::string n = lowercase(name);
    if (n == "usb") { out = Mode::Usb; return true; }
    if (n == "lsb") { out = Mode::Lsb; return true; }
    if (n == "cw" || n == "cwu") { out = Mode::Cw; return true; }
    if (n == "cwl") { out = Mode::CwL; return true; }
    if (n == "am") { out = Mode::Am; return true; }
    if (n == "sam") { out = Mode::Sam; return true; }
    if (n == "nfm" || n == "fm") { out = Mode::Nfm; return true; }
    if (n == "dsb") { out = Mode::Dsb; return true; }
    if (n == "wfm") { out = Mode::Wfm; return true; }
    return false;
}

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Usb: return "usb";
        case Mode::Lsb: return "lsb";
        case Mode::Cw: return "cw";
        case Mode::CwL: return "cwl";
        case Mode::Am: return "am";
        case Mode::Sam: return "sam";
        case Mode::Nfm: return "nfm";
        case Mode::Dsb: return "dsb";
        case Mode::Wfm: return "wfm";
    }
    return "usb";
}

bool mode_is_ssb(Mode mode) {
    return mode == Mode::Usb || mode == Mode::Lsb || mode == Mode::Cw || mode == Mode::CwL;
}

Passband default_passband(Mode mode, double cw_pitch_hz) {
    switch (mode) {
        // The classic 2.4 kHz filter, half gain at 300 and 2700 Hz. The
        // 2.7 kHz preset passes that range flat, but as the default its
        // extra 0.3 kHz took 14 % more audio bits, and on 24 and 32 kbit/s
        // links more dropouts, for 0.6 dB on a tone measurement.
        case Mode::Usb: return {300.0, 2700.0};
        case Mode::Lsb: return {-2700.0, -300.0};
        case Mode::Cw: return cw_passband(mode, cw_pitch_hz, 500.0);
        case Mode::CwL: return cw_passband(mode, cw_pitch_hz, 500.0);
        case Mode::Am: return {-4500.0, 4500.0};
        case Mode::Sam: return {-4500.0, 4500.0};
        case Mode::Nfm: return {-6000.0, 6000.0};
        case Mode::Dsb: return {-2700.0, 2700.0};
        // A broadcast channel: 75 kHz of deviation, the stereo and RDS
        // subcarriers up to 60 kHz, and stations 200 kHz apart.
        case Mode::Wfm: return {-100000.0, 100000.0};
    }
    return {-2700.0, 2700.0};
}

Passband cw_passband(Mode mode, double pitch_hz, double bandwidth_hz) {
    const double half = bandwidth_hz / 2.0;
    if (mode == Mode::CwL) return {-pitch_hz - half, -pitch_hz + half};
    return {pitch_hz - half, pitch_hz + half};
}

void Demodulator::configure(Mode mode, double sample_rate) {
    mode_ = mode;
    sample_rate_ = sample_rate;

    // ~20 Hz DC blocker: low enough not to touch speech, high enough to track
    // fading carriers in AM.
    dc_alpha_ = static_cast<float>(std::exp(-kTwoPi * 20.0 / sample_rate));

    // SAM loop: ~15 Hz natural frequency, critically damped-ish.  Wide enough
    // to follow a drifting or fading carrier, narrow enough to ignore
    // modulation sidebands.
    const double loop_bw = 15.0;
    const double wn = kTwoPi * loop_bw / sample_rate;
    pll_alpha_ = 2.0 * 0.707 * wn;
    pll_beta_ = wn * wn;
    pll_limit_ = kTwoPi * 250.0 / sample_rate;  // +/- 250 Hz capture range
    // ~50 ms smoothing on the lock indicator.
    pll_lock_rate_ = 1.0 - std::exp(-1.0 / (0.05 * sample_rate));

    // Phase discrimination ignores the gain applied to the complex input, so
    // the level is set here: 3 kHz of deviation comes out at 0.25, where the
    // peak-following AGC before the present one put a steady tone. SSB speech
    // comes out as loud under the present one, so the modes still match.
    // Scaling full deviation to 1.0 made a mode switch to NFM about 12 dB
    // louder before de-emphasis.
    fm_gain_ = static_cast<float>(0.25 * sample_rate / (kTwoPi * 3000.0));
    // Broadcast FM is as loud at 25 kHz of its 75: programme audio peaks
    // near full deviation, and pre-emphasis lifts its treble further.
    if (mode == Mode::Wfm) fm_gain_ = static_cast<float>(0.25 * sample_rate / (kTwoPi * 25000.0));
    set_deemphasis(deemphasis_us_);

    reset();
}

void Demodulator::set_deemphasis(double microseconds) {
    deemphasis_us_ = std::isfinite(microseconds) ? std::clamp(microseconds, 0.0, 2000.0) : 300.0;
    // One pole at 1 / (2 pi tau). A coefficient of one follows the input
    // exactly, which is what switched off means.
    deemph_alpha_ = deemphasis_us_ > 0.0
                        ? static_cast<float>(1.0 - std::exp(-1.0 / (sample_rate_ * deemphasis_us_ * 1e-6)))
                        : 1.0f;
}

void Demodulator::reset() {
    dc_state_ = 0.0f;
    pll_cos_ = 1.0;
    pll_sin_ = 0.0;
    pll_freq_ = 0.0;
    pll_lock_ = 0.0;
    pll_locked_ = false;
    pll_offset_hz_ = 0.0;
    fm_previous_ = cfloat(0.0f, 0.0f);
    deemph_state_ = 0.0f;
}

void Demodulator::process(const cfloat* in, size_t count, float* out) {
    if (count == 0) return;

    // S-meter from the complex channel power, before any demodulation or gain.
    double power = 0.0;
    for (size_t i = 0; i < count; i++) {
        power += static_cast<double>(in[i].real()) * in[i].real() +
                 static_cast<double>(in[i].imag()) * in[i].imag();
    }
    level_dbfs_ = static_cast<float>(10.0 * std::log10(power / count + 1e-30));

    switch (mode_) {
        case Mode::Usb:
        case Mode::Lsb:
        case Mode::Cw:
        case Mode::CwL:
        case Mode::Dsb:
            demod_ssb(in, count, out);
            break;
        case Mode::Am:
            demod_am(in, count, out);
            break;
        case Mode::Sam:
            demod_sam(in, count, out);
            break;
        case Mode::Nfm:
            demod_fm(in, count, out);
            break;
        case Mode::Wfm:
            // The phase steps alone: de-emphasis and the DC blocker follow
            // the decimation (see Listener), at a seventh of the rate.
            simd::phase_steps(in, count, fm_previous_, out);
            fm_previous_ = in[count - 1];
            for (size_t i = 0; i < count; i++) out[i] *= fm_gain_;
            break;
    }
}

// The channelizer's mask already selected the sideband, so the audio is
// simply the real part: the suppressed sideband contributes nothing to fold
// back over it.
void Demodulator::demod_ssb(const cfloat* in, size_t count, float* out) {
    for (size_t i = 0; i < count; i++) out[i] = in[i].real();
}

void Demodulator::demod_am(const cfloat* in, size_t count, float* out) {
    for (size_t i = 0; i < count; i++) {
        const float envelope = magnitude_of(in[i]);
        dc_state_ = dc_alpha_ * dc_state_ + (1.0f - dc_alpha_) * envelope;
        out[i] = envelope - dc_state_;
    }
}

// Synchronous AM takes the component in phase with the tracked carrier.
// While locked, it rejects quadrature noise that envelope detection folds
// into the audio. A deep carrier fade can still make the loop lose lock.
void Demodulator::demod_sam(const cfloat* in, size_t count, float* out) {
    // The loop is steered once a run of samples rather than every sample:
    // within a run the carrier turns by a fixed step, so no sample waits on
    // the square root and the division of the one before, a chain that was
    // most of what SAM cost. A run is as long as a carrier at the edge of
    // the capture range takes to turn an eighth of a cycle against the loop,
    // 2000 runs a second; longer, a carrier far off averaged its own error
    // away across the run and was never caught.
    const size_t run = std::max<size_t>(1, static_cast<size_t>(sample_rate_ / 2000.0));
    for (size_t start = 0; start < count; start += run) {
        const size_t end = std::min(count, start + run);
        const double f = pll_freq_, f2 = f * f;
        // The run's step, e^(-j freq): under a quarter radian (250 Hz of
        // capture at 8 kHz), where the series to the ninth power are exact
        // to 1e-9.
        const double step_sin = f * (1.0 - f2 / 6.0 * (1.0 - f2 / 20.0 * (1.0 - f2 / 42.0 * (1.0 - f2 / 72.0))));
        const double step_cos = 1.0 - f2 / 2.0 * (1.0 - f2 / 12.0 * (1.0 - f2 / 30.0 * (1.0 - f2 / 56.0)));
        double pc = pll_cos_, ps = pll_sin_;
        double errors = 0.0;
        for (size_t i = start; i < end; i++) {
            const float c = static_cast<float>(pc);
            const float s = static_cast<float>(ps);
            const cfloat mixed(in[i].real() * c - in[i].imag() * s, in[i].real() * s + in[i].imag() * c);

            // Phase error, normalised so loop gain does not depend on signal level.
            const float magnitude = magnitude_of(mixed) + 1e-12f;
            errors += static_cast<double>(mixed.imag()) / magnitude;

            // In-phase component is the audio; DC-block removes the carrier.
            const float inphase = mixed.real();
            dc_state_ = dc_alpha_ * dc_state_ + (1.0f - dc_alpha_) * inphase;
            out[i] = inphase - dc_state_;

            // Locked when the energy sits predominantly on the in-phase axis.
            // Tracked per sample rather than per call so the indicator does not
            // depend on how the caller happens to block the audio up.
            const double instantaneous = std::fabs(static_cast<double>(mixed.real())) / magnitude;
            pll_lock_ += pll_lock_rate_ * (instantaneous - pll_lock_);

            const double next = pc * step_cos + ps * step_sin;
            ps = ps * step_cos - pc * step_sin;
            pc = next;
        }
        // The run's errors steer the frequency and, as one small turn, the
        // phase; the phasor's length is set back to one.
        pll_freq_ = std::clamp(pll_freq_ + pll_beta_ * errors, -pll_limit_, pll_limit_);
        const double p = pll_alpha_ * errors, p2 = p * p;
        const double turn_sin = p * (1.0 - p2 / 6.0 * (1.0 - p2 / 20.0));
        const double turn_cos = 1.0 - p2 / 2.0 * (1.0 - p2 / 12.0 * (1.0 - p2 / 30.0));
        const double turned_cos = pc * turn_cos + ps * turn_sin;
        const double turned_sin = ps * turn_cos - pc * turn_sin;
        const double length_fix = 1.5 - 0.5 * (turned_cos * turned_cos + turned_sin * turned_sin);
        pll_cos_ = turned_cos * length_fix;
        pll_sin_ = turned_sin * length_fix;
    }

    pll_locked_ = pll_lock_ > 0.85;
    pll_offset_hz_ = pll_freq_ * sample_rate_ / kTwoPi;
}

void Demodulator::demod_fm(const cfloat* in, size_t count, float* out) {
    // Phase difference between consecutive samples is the instantaneous
    // frequency; the angle of the product with the conjugate avoids
    // unwrapping. Taken for the whole block in vectors first: at broadcast
    // FM's channel rate it is most of the work.
    simd::phase_steps(in, count, fm_previous_, out);
    if (count > 0) fm_previous_ = in[count - 1];
    for (size_t i = 0; i < count; i++) {
        const float discriminated = out[i] * fm_gain_;
        // A tuning error becomes DC after the discriminator. Remove it before
        // encoding so it cannot consume codec bits and output headroom, or
        // leave a large step when the listener switches modes.
        dc_state_ = dc_alpha_ * dc_state_ + (1.0f - dc_alpha_) * discriminated;
        deemph_state_ += deemph_alpha_ * (discriminated - dc_state_ - deemph_state_);
        out[i] = deemph_state_;
    }
}

}  // namespace fernsdr
