// Demodulators.
//
// Every mode receives complex baseband from a Channel, already filtered to the
// user's passband and centred so that the tuning point sits at DC.  That makes
// the demodulators themselves small: SSB is a real-part extraction, because
// the channelizer's asymmetric mask has already done the sideband selection.
#pragma once
#include <cstddef>
#include <string>

#include "fft.h"

namespace fernsdr {

enum class Mode {
    Usb,
    Lsb,
    Cw,   // CW on the upper side, offset to the sidetone pitch
    CwL,  // CW on the lower side
    Am,
    Sam,  // synchronous AM with carrier tracking
    Nfm,
    Dsb,
    Wfm,  // broadcast FM, demodulated at the channel's rate; see Listener
};

// Parses "usb", "lsb", "cw", "cwl", "am", "sam", "nfm", "dsb", "wfm"
// (case-insensitive).
bool mode_from_name(const std::string& name, Mode& out);
const char* mode_name(Mode mode);

// Whether the mode's passband is naturally one-sided.
bool mode_is_ssb(Mode mode);

// Default passband edges for a mode, relative to the tuning point, in Hz.
struct Passband {
    double low;
    double high;
};
Passband default_passband(Mode mode, double cw_pitch_hz);

// For CW the channel is tuned to the carrier but the passband is placed around
// the sidetone pitch, so the operator hears a tone at that pitch.
Passband cw_passband(Mode mode, double pitch_hz, double bandwidth_hz);

class Demodulator {
public:
    Demodulator() = default;

    void configure(Mode mode, double sample_rate);
    // The NFM de-emphasis time constant in microseconds; zero is flat. Kept
    // across configure(), and taking effect at once. Broadcast FM's comes
    // out undone: the Listener's decimator applies it.
    void set_deemphasis(double microseconds);
    double deemphasis() const { return deemphasis_us_; }
    Mode mode() const { return mode_; }

    // Demodulates `count` complex samples into `count` real samples.
    void process(const cfloat* in, size_t count, float* out);

    // Mean signal power of the last block, in dB relative to full scale.
    // This is the S-meter reading, taken before AGC so it reflects the
    // antenna, not the gain control.
    float level_dbfs() const { return level_dbfs_; }

    // SAM lock state, for the UI.  Meaningless in other modes.
    bool pll_locked() const { return pll_locked_; }
    double pll_offset_hz() const { return pll_offset_hz_; }

    void reset();

private:
    void demod_ssb(const cfloat* in, size_t count, float* out);
    void demod_am(const cfloat* in, size_t count, float* out);
    void demod_sam(const cfloat* in, size_t count, float* out);
    void demod_fm(const cfloat* in, size_t count, float* out);

    Mode mode_ = Mode::Usb;
    double sample_rate_ = 12000.0;
    double deemphasis_us_ = 300.0;
    float level_dbfs_ = -140.0f;

    // AM/SAM carrier removal, NFM discriminator offset removal
    float dc_state_ = 0.0f;
    float dc_alpha_ = 0.999f;

    // SAM phase-locked loop
    // The tracked carrier as e^(-j phase): turned a little each sample
    // rather than taken from the phase by a sine and a cosine.
    double pll_cos_ = 1.0;
    double pll_sin_ = 0.0;
    double pll_freq_ = 0.0;
    double pll_alpha_ = 0.0;
    double pll_beta_ = 0.0;
    double pll_limit_ = 0.0;
    double pll_lock_ = 0.0;
    double pll_lock_rate_ = 0.001;
    bool pll_locked_ = false;
    double pll_offset_hz_ = 0.0;

    // FM discriminator
    cfloat fm_previous_ = cfloat(0.0f, 0.0f);
    float fm_gain_ = 1.0f;
    // FM de-emphasis
    float deemph_state_ = 0.0f;
    float deemph_alpha_ = 0.0f;
};

}  // namespace fernsdr
