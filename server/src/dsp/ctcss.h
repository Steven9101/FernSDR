// Which CTCSS tone an NFM signal carries, measured on the demodulated audio.
//
// Repeaters and most FM users send a sub-audible tone (67 to 254.1 Hz) under
// the voice, and knowing it is what a listener needs to use the repeater or
// to tell two users of one channel apart. Radios show it as "88.5"; so can a
// receiver, if it measures rather than guesses.
//
// Measured on the discriminator's output before the audio low cut, which is
// there precisely to take these tones out of what the listener hears.
#pragma once

#include "fir_decimator.h"
#include "fft_split.h"

#include <cstddef>
#include <vector>

namespace fernsdr {

class CtcssDetector {
public:
    // The 50 tones of EIA/TIA-603, in Hz.
    static const std::vector<double>& standard_tones();

    // Starts over at a new audio rate: nothing measured, nothing reported.
    void configure(double sample_rate);

    void process(const float* audio, size_t count);

    // The tone being received, in Hz, or 0 when there is none.
    double tone_hz() const { return reported_hz_; }
    // Where that tone actually is, to a tenth of a hertz or so: an encoder
    // can sit a hertz off the standard, and a notch aimed at the standard
    // would then miss it. 0 when there is no tone.
    double measured_hz() const { return reported_hz_ > 0.0 ? measured_precise_ : 0.0; }

    // The nearest standard tone to a measured frequency, or 0 when it is not
    // close enough to one to say which (see the .cpp for "close enough").
    static double nearest_standard(double measured_hz);

    // Tone squelch: open only while `hz` (a standard tone) is being received;
    // 0 switches it off. Unlike naming a tone, which takes a couple of
    // seconds, this answers within about half a second, since it only has to
    // tell one known tone from its neighbours.
    void set_squelch_tone(double hz);
    double squelch_tone() const { return squelch_tone_; }
    bool squelch_open() const { return squelch_tone_ <= 0.0 || gate_open_; }

private:
    void evaluate();
    void evaluate_gate();
    double goertzel_power(double hz, size_t samples) const;

    double sample_rate_ = 0.0;
    size_t decimation_ = 1;
    double decimated_rate_ = 0.0;
    // The tones' band kept and everything that would fold onto it after the
    // decimation 60 dB down, the voice above it included. A polyphase FIR
    // computes only the samples it keeps, a fifteenth or so of them; the two
    // IIR sections it replaced ran on every sample and were a quarter of an
    // NFM listener's cost.
    FirDecimator decimator_;
    std::vector<float> decimated_;

    std::vector<float> ring_;
    size_t written_ = 0;
    size_t since_evaluation_ = 0;
    size_t hop_ = 0;
    std::vector<float> window_;
    // The measurement's transform, the vector one the band uses.
    std::vector<float> re_, im_;
    FftSplit fft_{4096};

    double candidate_hz_ = 0.0;
    int agreement_ = 0;
    int misses_ = 0;
    double reported_hz_ = 0.0;
    double squelch_tone_ = 0.0;
    double gate_neighbours_[2] = {0.0, 0.0};
    size_t gate_samples_ = 0;
    size_t gate_hop_ = 1;
    size_t since_gate_ = 0;
    int gate_misses_ = 0;
    bool gate_open_ = false;
    double measured_precise_ = 0.0;
    double candidate_precise_ = 0.0;
};

}  // namespace fernsdr
