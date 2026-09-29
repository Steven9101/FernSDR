// Low-pass filtering and decimation by a whole number, for audio that is
// demodulated at a rate far above what it needs: broadcast FM is taken apart
// at the channel's 200 kHz and more, and heard at a tenth of that.
//
// A polyphase FIR: only every `factor`-th output is computed, so the cost is
// taps / factor multiplications per input sample. The taps are a windowed
// sinc (Kaiser), with the window sized from the attenuation asked for and the
// width of the transition; see configure().
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace fernsdr {

class FirDecimator {
public:
    // Passes up to `pass_hz` and attenuates by `stop_db` from `stop_hz` on, at
    // `input_rate`, keeping every `factor`-th sample. The stop edge must lie
    // below half the output rate plus what the pass band leaves, or the
    // output would fold back into what is kept; configure() does not check
    // this, the caller chooses the rates.
    //
    // With `deemphasis_us`, an FM receiver's de-emphasis, a one-pole low pass
    // with that time constant, is folded into the taps: its response has
    // fallen by e^-8 within eight time constants, a few dozen samples at a
    // broadcast channel's rate, so a filter that runs anyway takes it over
    // from a recursive loop that would visit every sample one after another.
    void configure(double input_rate, size_t factor, double pass_hz, double stop_hz, double stop_db,
                   double deemphasis_us = 0.0);
    // Filters and decimates `count` samples of `in` into `out`, which may be
    // `in` itself, and returns how many came out. Keeps its history between
    // calls, so blocks of any size join without a seam.
    size_t process(const float* in, size_t count, float* out);
    void reset();

    size_t factor() const { return factor_; }
    size_t taps() const { return taps_.size(); }
    double output_rate() const { return output_rate_; }

private:
    std::vector<float> taps_;
    // The last taps - 1 inputs, then the block being filtered.
    std::vector<float> history_;
    size_t factor_ = 1;
    // Inputs still to skip before the next output, carried across blocks.
    size_t phase_ = 0;
    double output_rate_ = 0.0;
};

// A band of a real signal around `centre_hz`, brought down to complex
// baseband and decimated, as a receiver takes a subcarrier out of the FM
// multiplex. The mixing is folded into the taps: output m, ending at input
// n, is e^(-j w n) times the window filtered by h[k] e^(j w k), so no input
// sample is ever multiplied by an oscillator; only the outputs are turned,
// by a phasor that steps the same angle each time.
class BandDecimator {
public:
    // As FirDecimator::configure, with pass and stop measured from the centre.
    void configure(double input_rate, size_t factor, double centre_hz, double pass_hz, double stop_hz, double stop_db);
    size_t process(const float* in, size_t count, std::complex<float>* out);

    size_t factor() const { return factor_; }
    size_t taps() const { return taps_i_.size(); }
    double output_rate() const { return output_rate_; }

private:
    // Oldest-first, like FirDecimator's; the real and imaginary parts of
    // the shifted taps apart, so each is one dot product.
    std::vector<float> taps_i_, taps_q_;
    std::vector<float> history_;
    size_t factor_ = 1;
    size_t phase_ = 0;
    double output_rate_ = 0.0;
    // e^(-j w n) at the next output, and its step between outputs.
    std::complex<double> turn_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
};

// The low-pass taps FirDecimator and BandDecimator use, oldest-first
// (reversed) and with unity gain at DC; for `deemphasis_us` see
// FirDecimator::configure.
std::vector<float> design_lowpass(double input_rate, double pass_hz, double stop_hz, double stop_db,
                                  double deemphasis_us = 0.0);

}  // namespace fernsdr
