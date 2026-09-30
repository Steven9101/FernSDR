// Automatic gain control, on the complex baseband ahead of demodulation.
//
// It works on the complex signal because real audio crosses zero all the time:
// a detector watching it ripples at audio rate and modulates the gain with the
// signal itself. It also puts the gain where a hardware receiver has it, in
// the IF ahead of the detector, which keeps AM from distorting as the carrier
// fades.
//
// What a listener hears of a gain control is mostly the gaps. A control that
// follows the signal's peak raises its gain between words and between overs,
// and the band noise swells with it until it is as loud as the speech. This
// one knows how much noise the channel carries (read off the band's transform
// around it, see ChannelNoise) and never lets that noise come closer than
// 15 dB under its target. A signal within 15 dB of the noise is therefore left
// at the gain that holds the noise there, which moves only as the noise does,
// and the noise sounds as steady as with no gain control. Stronger signals are
// levelled on their RMS over each stretch of up to 20 ms. The gain is held
// after the last stretch that pulled it down, then released: how long it holds
// and how fast it comes back is what the profiles differ in. 2 ms peaks more
// than 3 dB over the target pull it down further for 80 ms, after which it
// comes back to the level the speech set and no further until the hold is
// over: letting a plosive end the hold, as a plain 80 ms hold does, let the
// noise climb 18 dB in the second after it. The decision is made on the block
// it is applied to, which the listener has whole before any of it leaves, so
// the gain is down before an onset arrives without a delay line.
//
// A hold that only watched for pauses would also hold back a weaker station
// taking over from a stronger one: on a QSO the reply came up two seconds late.
// So stretches at least 10 dB over the noise that the held gain leaves more
// than 3 dB under the target, for 60 ms of the last fifth of a second or so,
// are taken for a signal rather than a pause, and bring the gain up at 60 dB/s,
// as far as 3 dB under the target, while the hold runs on for the pauses. The
// 60 ms leave out static crashes, which are loud and short: crashes of up to
// about 50 ms taking less than about a third of the time. Denser or longer
// static still raises the gain while the hold runs, as far as the noise
// ceiling. The 3 dB keep it off the target itself: on a steady carrier such as
// an RTTY or FT8 tone, a gain rising to the target and pulled back by the next
// stretch's level moved at 25 Hz and put sidebands on the tone.
//
// AM and SAM keep their carrier through every pause, so they have no hold: the
// gain follows the level of carrier and sidebands together, both ways, with a
// time constant set by the profile, and only 2 ms peaks 6 dB over the target
// pull it down at once. At 3 dB, ordinary 80% modulation set off the peak rule
// on every crest, and the gain it moved came out of the 20 Hz DC blocker as
// thumps.
//
// NFM has no gain control; see Listener. Measured on synthetic scenes with a
// known answer, driven through a real band and listener and the codec, in
// `make agc-lab` (tools/agc_lab.cpp).
#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "fft.h"

namespace fernsdr {

enum class AgcProfile {
    Off,     // fixed manual gain
    Fast,    // holds 0.3 s, back at 40 dB/s
    Medium,  // 1 s, 30 dB/s
    Slow,    // 2.5 s, 20 dB/s: outlasts the pause between overs
    Long,    // 5 s, 10 dB/s: nets with long pauses
    Auto,    // what suits the mode: Slow, and nothing for NFM
};

// "steady", the name the first version of this engine had, is taken for Slow.
bool agc_profile_from_name(const std::string& name, AgcProfile& out);
const char* agc_profile_name(AgcProfile profile);

class Agc {
public:
    // A loud enough stretch comes out at this RMS on the complex baseband. A
    // strong SSB station comes out as loud as the peak-following profiles
    // had it (on a 20 m QSO the speech levels agreed within half a dB); a
    // steady tone, CW or a digital mode, 3.7 dB quieter, since its RMS is its
    // peak.
    static constexpr double kTarget = 0.164;
    // ...and holds the channel noise at least this far under it: -15 dB.
    static constexpr double kNoiseUnderTarget = 0.17782794100389228;

    void configure(double sample_rate);
    // Auto is Slow here; what a mode gets is the listener's to decide.
    void set_profile(AgcProfile profile);
    AgcProfile profile() const { return profile_; }
    // For AM and SAM: follow the carrier, with no hold. See above.
    void set_follow_carrier(bool follow) { follow_carrier_ = follow; }

    // Manual gain in dB: the whole gain when the profile is Off, and an offset
    // on top of the automatic gain otherwise.
    void set_manual_gain_db(float db);
    // Ceiling on the gain the AGC will apply. A signal quiet enough that the
    // ceiling binds is simply amplified by it, so a dead band comes up as quiet
    // noise instead of full-scale hiss.
    void set_max_gain_db(float db);

    // The noise power per sample in the samples process() is given, which the
    // automatic profiles hold 15 dB under their target. 0 means not known yet,
    // and they then level the signal without that floor.
    void set_noise_power(double power) { noise_power_ = power; }

    // Applies gain in place to complex baseband, ahead of demodulation.
    void process(cfloat* samples, size_t count);

    // Signal level at the input, before any gain is applied: the honest
    // S-meter reading.
    float input_dbfs() const { return input_dbfs_; }
    float gain_db() const { return gain_db_; }

    void reset();

private:
    void level(cfloat* samples, size_t count);

    AgcProfile profile_ = AgcProfile::Slow;
    bool follow_carrier_ = false;
    double sample_rate_ = 12000.0;
    float manual_gain_db_ = 0.0f;
    float max_gain_db_ = 60.0f;
    float gain_db_ = 0.0f;
    float input_dbfs_ = -140.0f;
    double noise_power_ = 0.0;

    // In double: it moves by tiny factors per sample while recovering.
    double gain_ = 1.0;
    // The gain the last loud stretch set, which the hold protects, and how
    // long that protection and the shorter one after a 2 ms peak have left,
    // in samples.
    double level_gain_ = 1.0;
    long hold_ = 0;
    long peak_hold_ = 0;
    // How much of the last fifth of a second or so has looked like a weaker
    // station, in samples: a sum that decays with that time constant.
    double weaker_ = 0.0;
    // False until the first stretch, which sets the gain outright: a retune
    // should not open with a second of the gain finding its level.
    bool primed_ = false;
};

// Impulse noise blanker, for the WIDEBAND input.
//
// Power-line arcing, ignition noise and switching supplies put short, very
// strong impulses on HF. They are brief enough that removing them costs almost
// none of the wanted signal, and loud enough that leaving them in wrecks both
// listening and the AGC's idea of the signal level.
//
// This runs on the band's raw input, before any channel filtering, and that
// placement is the whole point. An impulse is broadband and lasts microseconds;
// a 2.4 kHz channel filter smears it across milliseconds and buries it in the
// signal, so by the time it reaches a listener's channel it can no longer be
// told apart from an ordinary speech peak. Attempts to blank per listener
// either missed the impulses or chewed the speech. Before the filter it is
// unmistakable - and it is also work done once for the whole band rather than
// once per listener.
class NoiseBlanker {
public:
    void configure(double sample_rate);
    // 0 disables. Higher values blank on smaller excursions.
    void set_strength(float strength);
    float strength() const { return strength_.load(std::memory_order_relaxed); }
    bool active() const { return strength() > 0.0f; }

    // Blanks impulses in place on the band's raw input.
    void process(cfloat* samples, size_t count);
    void process_real(float* samples, size_t count);

    // Fraction of samples blanked recently, for the UI: a blanker that is
    // gating half the signal is misconfigured, and the operator should see it.
    float blanked_fraction() const { return blanked_fraction_.load(std::memory_order_relaxed); }

    void reset();

private:
    void account(size_t count);

    // Written by the admin panel's thread, read by the band's DSP thread on
    // every block. Relaxed because a change taking effect one block later is
    // fine; a torn read would not be.
    std::atomic<float> strength_{0.0f};
    // Envelope follower: fast rise, very slow fall.
    double reference_ = 0.0;
    double attack_coefficient_ = 0.99;
    double decay_coefficient_ = 0.9999;
    int blank_span_ = 2;
    size_t blanked_ = 0;
    size_t total_ = 0;
    // Written by the band's thread, read by whoever asks for the band's
    // information.
    std::atomic<float> blanked_fraction_{0.0f};
};

}  // namespace fernsdr
