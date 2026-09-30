#include "agc.h"

#include "simd.h"
#include "magnitude.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

namespace {

// How long each profile holds, how fast it comes back after, and, for AM and
// SAM, how fast it follows the carrier. Slow's 2.5 s outlasts the pause
// between overs, which runs to a second and a half or two, where 1.1 s did
// not; the others trade some of that for a faster return.
struct Timing {
    double hold_seconds;
    double recovery_db_per_second;
    double follow_seconds;
};

Timing timing(AgcProfile profile) {
    switch (profile) {
        case AgcProfile::Fast: return {0.3, 40.0, 0.1};
        case AgcProfile::Medium: return {1.0, 30.0, 0.2};
        case AgcProfile::Long: return {5.0, 10.0, 0.5};
        default: return {2.5, 20.0, 0.3};
    }
}

// One decision covers at most this much, and a peak is the mean power of the
// loudest 2 ms of it.
constexpr double kStretchSeconds = 0.02;
constexpr double kSliceSeconds = 0.002;
constexpr double kPeakHoldSeconds = 0.08;
// How far that peak may come over the target: speech and CW, and AM, whose
// crests at 80% modulation already stand 3.9 dB over its RMS.
constexpr double kPeakOverTarget = 1.4142135623730951;  // 3 dB
constexpr double kCarrierPeakOverTarget = 2.0;          // 6 dB
// No single sample may leave more than this many times the target: 12 dB over
// it, 3.7 dB under full scale. An onset in the last samples of a block is in
// its 2 ms slice only in part, the rest of it being in the next block, and the
// slice's mean power alone let it out at nearly the old gain, past full scale.
constexpr double kSampleLimit = 4.0;
// A signal, not a pause, while the hold runs: 10 dB over the noise, and more
// than 3 dB under the target at the held gain. See agc.h.
constexpr double kSignalOverNoise = 3.1622776601683795;
constexpr double kSignalUnderTarget = 0.7079457843841379;
constexpr double kSignalRecoveryDbPerSecond = 60.0;
// ...for this much of the last fifth of a second or so. Static crashes are
// short and far apart; taken one at a time, they raised the gain the hold was
// keeping in a pause until the noise sat at the ceiling. Counted without a
// break instead, the 48 ms dots of 25 WPM CW never counted and a station came
// back after a strong neighbour two seconds late.
constexpr double kSignalSeconds = 0.06;
constexpr double kSignalWindowSeconds = 0.2;

}  // namespace

bool agc_profile_from_name(const std::string& name, AgcProfile& out) {
    if (name == "off") { out = AgcProfile::Off; return true; }
    if (name == "fast") { out = AgcProfile::Fast; return true; }
    if (name == "medium" || name == "med") { out = AgcProfile::Medium; return true; }
    if (name == "slow" || name == "steady") { out = AgcProfile::Slow; return true; }
    if (name == "long") { out = AgcProfile::Long; return true; }
    if (name == "auto") { out = AgcProfile::Auto; return true; }
    return false;
}

const char* agc_profile_name(AgcProfile profile) {
    switch (profile) {
        case AgcProfile::Off: return "off";
        case AgcProfile::Fast: return "fast";
        case AgcProfile::Medium: return "medium";
        case AgcProfile::Slow: return "slow";
        case AgcProfile::Long: return "long";
        case AgcProfile::Auto: return "auto";
    }
    return "auto";
}

void Agc::configure(double sample_rate) {
    sample_rate_ = sample_rate;
    reset();
}

void Agc::set_profile(AgcProfile profile) {
    if (profile == AgcProfile::Auto) profile = AgcProfile::Slow;
    // The profiles share one gain, so moving between them keeps it and only
    // the hold is cut to the new length. Off keeps none, and coming back from
    // it the gain starts over from the stretch in front of it.
    if (profile_ == AgcProfile::Off && profile != AgcProfile::Off) primed_ = false;
    profile_ = profile;
    if (profile != AgcProfile::Off) {
        hold_ = std::min(hold_, static_cast<long>(timing(profile).hold_seconds * sample_rate_));
    }
}

void Agc::set_manual_gain_db(float db) { manual_gain_db_ = db; }
void Agc::set_max_gain_db(float db) { max_gain_db_ = db; }

void Agc::reset() {
    gain_db_ = 0.0f;
    gain_ = 1.0;
    level_gain_ = 1.0;
    hold_ = 0;
    peak_hold_ = 0;
    weaker_ = 0.0;
    primed_ = false;
}

void Agc::process(cfloat* samples, size_t count) {
    if (count == 0) return;
    if (profile_ != AgcProfile::Off) {
        level(samples, count);
        return;
    }
    // S-meter: mean power of the input, before any gain. Measuring after the
    // AGC would report the gain control's target, not the antenna.
    // The sum of re^2 + im^2 is the interleaved floats' product with
    // themselves; in vectors, since FM runs here at its whole channel rate.
    const float* floats = reinterpret_cast<const float*>(samples);
    const double power = simd::dot(floats, floats, 2 * count);
    input_dbfs_ = static_cast<float>(10.0 * std::log10(power / count + 1e-30));
    // FM is always at 0 dB, which leaves the samples as they are.
    if (manual_gain_db_ != 0.0f) {
        const float gain = std::pow(10.0f, manual_gain_db_ / 20.0f);
        for (size_t i = 0; i < count; i++) samples[i] *= gain;
    }
    gain_db_ = manual_gain_db_;
}

void Agc::level(cfloat* samples, size_t count) {
    const double rate = sample_rate_;
    const Timing t = timing(profile_);
    const size_t longest = std::max<size_t>(1, static_cast<size_t>(std::lround(kStretchSeconds * rate)));
    const size_t slice = std::max<size_t>(1, static_cast<size_t>(std::lround(kSliceSeconds * rate)));
    const long hold = static_cast<long>(t.hold_seconds * rate);
    const long peak_hold = static_cast<long>(kPeakHoldSeconds * rate);
    const double max_gain = std::pow(10.0, max_gain_db_ / 20.0);
    const double manual = std::pow(10.0, manual_gain_db_ / 20.0);
    const double noise_rms = noise_power_ > 0.0 ? std::sqrt(noise_power_) : 0.0;
    const double recovery = std::pow(10.0, t.recovery_db_per_second / 20.0 / rate);
    const double signal_recovery = std::pow(10.0, kSignalRecoveryDbPerSecond / 20.0 / rate);
    const double peak_over = follow_carrier_ ? kCarrierPeakOverTarget : kPeakOverTarget;
    constexpr double kCeiling = kNoiseUnderTarget * kTarget;
    // No noise estimate yet: no floor, and nothing is taken for a signal.
    const double ceiling_gain = noise_rms > 0.0 ? kCeiling / noise_rms : max_gain;
    const double signal_rms = noise_rms * kSignalOverNoise;
    const double signal_needed = kSignalSeconds * rate;

    // A block is split into equal stretches no longer than 20 ms; a listener's
    // blocks are usually 5 to 20 ms, one stretch each.
    const size_t stretches = (count + longest - 1) / longest;
    double total = 0.0;
    size_t start = 0;
    for (size_t s = 0; s < stretches; s++) {
        const size_t end = count * (s + 1) / stretches;
        const size_t n = end - start;
        cfloat* x = samples + start;

        if (!primed_) {
            double e = 0.0;
            for (size_t k = 0; k < n; k++) e += static_cast<double>(std::norm(x[k]));
            const double settled = kTarget / std::max(std::sqrt(e / static_cast<double>(n)), 1e-12);
            gain_ = std::min({settled, ceiling_gain, max_gain});
            level_gain_ = gain_;
            hold_ = 0;
            peak_hold_ = 0;
            weaker_ = 0.0;
            primed_ = true;
        }

        // The stretch's energy, the mean power of its loudest 2 ms, and where
        // the first 2 ms that is too loud at the present gain begins. The last
        // slice takes whatever is left over, so no sample escapes the check: an
        // onset in an unchecked tail left at nearly the old gain, past full
        // scale.
        const double too_loud = peak_over * kTarget / gain_;
        const double too_loud_power = too_loud * too_loud;
        const double sample_limit = kSampleLimit * kTarget;
        const double sample_over = sample_limit / gain_;
        const double sample_over_power = sample_over * sample_over;
        const size_t slices = std::max<size_t>(1, n / slice);
        double energy = 0.0, loudest = 0.0, loudest_sample = 0.0;
        size_t first_over = n;
        for (size_t j = 0; j < slices; j++) {
            const size_t from = j * slice;
            const size_t to = j + 1 == slices ? n : from + slice;
            double e = 0.0, top = 0.0;
            for (size_t k = from; k < to; k++) {
                const double p = static_cast<double>(std::norm(x[k]));
                e += p;
                top = std::max(top, p);
            }
            energy += e;
            const double power = e / static_cast<double>(to - from);
            loudest = std::max(loudest, power);
            loudest_sample = std::max(loudest_sample, top);
            if (first_over == n && (power > too_loud_power || top > sample_over_power)) first_over = from;
        }
        total += energy;
        const double rms = std::sqrt(energy / static_cast<double>(n));
        const double peak = std::sqrt(loudest);
        const double sample_peak = std::sqrt(loudest_sample);

        const double gain = gain_;
        double next = gain;
        const bool weaker = hold_ > 0 && noise_rms > 0.0 && rms > signal_rms && rms * gain < kSignalUnderTarget * kTarget;
        weaker_ = weaker_ * std::exp(-static_cast<double>(n) / (kSignalWindowSeconds * rate)) +
                  (weaker ? static_cast<double>(n) : 0.0);
        // Where the gain has to have arrived: by the first slice that is too
        // loud when that is what pulls it down, which is at once if that is
        // the first; otherwise by the end of the stretch.
        size_t arrive = n;
        bool peak_set = false, level_set = false;
        if (peak * gain > peak_over * kTarget || sample_peak * gain > sample_limit) {
            next = std::min(peak_over * kTarget / peak, sample_limit / sample_peak);
            arrive = std::min(first_over, n);
            peak_set = true;
        } else if (follow_carrier_) {
            // Towards the stretch's level, both ways, over the profile's time
            // constant, and never past the noise ceiling.
            if (rms > 0.0) {
                const double aim = std::min(kTarget / rms, ceiling_gain);
                next = gain * std::pow(aim / gain, std::min(1.0, static_cast<double>(n) / (t.follow_seconds * rate)));
            }
        } else if (rms * gain > kTarget) {
            next = kTarget / rms;
            level_set = true;
        } else if (gain > ceiling_gain) {
            next = ceiling_gain;
        } else if (peak_hold_ > 0) {
            // Held for 80 ms after a peak.
        } else if (weaker && weaker_ >= signal_needed) {
            // A weaker station, not a pause: up while the hold runs on.
            next = std::min({gain * std::pow(signal_recovery, static_cast<double>(n)),
                             kSignalUnderTarget * kTarget / rms, ceiling_gain});
        } else if (hold_ > 0) {
            // Back up to the level the last loud stretch set, after a peak
            // took it lower, but no further until the hold is over.
            next = std::min(gain * std::pow(recovery, static_cast<double>(n)), std::max(gain, level_gain_));
        } else {
            // The hold is over: back up at the profile's rate, but no further
            // than this stretch's own level allows. Under a steady tone the
            // gain used to rise for a stretch after every hold and be pulled
            // back by the next, 0.2 to 0.4 dB each time; in a pause the level
            // is the noise's, far over the ceiling that stops it anyway.
            next = gain * std::pow(recovery, static_cast<double>(n));
            if (rms > 0.0) next = std::min(next, kTarget / rms);
        }
        next = std::min(next, max_gain);
        const long elapsed = static_cast<long>(n);
        peak_hold_ = peak_set ? peak_hold : std::max(0L, peak_hold_ - elapsed);
        if (level_set) {
            hold_ = hold;
            level_gain_ = next;
        } else {
            hold_ = std::max(0L, hold_ - elapsed);
        }

        // An exponential ramp, a constant factor per sample, and then flat.
        const double step = arrive > 0 ? std::pow(next / gain, 1.0 / static_cast<double>(arrive)) : 1.0;
        double g = gain * manual;
        for (size_t k = 0; k < arrive; k++) {
            g *= step;
            x[k] *= static_cast<float>(g);
        }
        const float held = static_cast<float>(next * manual);
        for (size_t k = arrive; k < n; k++) x[k] *= held;

        gain_ = next;
        start = end;
    }
    input_dbfs_ = static_cast<float>(10.0 * std::log10(total / static_cast<double>(count) + 1e-30));
    gain_db_ = static_cast<float>(20.0 * std::log10(gain_ * manual));
}

// --- NoiseBlanker -----------------------------------------------------------

void NoiseBlanker::configure(double sample_rate) {
    // The reference is an envelope follower with a fast rise and a very slow
    // fall, and it is the part of this that took the most getting right.
    //
    //  - Rising over about 10 ms means a single-sample impulse barely moves
    //    it, so an impulse cannot raise the threshold and hide the ones after
    //    it, while the envelope of real speech is followed closely.
    //  - Falling over a couple of seconds means the gaps between syllables do
    //    not drag it down. A faster fall made the onset of every syllable look
    //    like an impulse.
    //
    // A mean or an RMS cannot do this job: speech and CW have a high crest
    // factor, so ordinary peaks run several times either one. Nor can a
    // median: speech is silent about half the time, so the median lands
    // between the loud and quiet modes and everything above it gets blanked.
    // In double, with the reference: at 64.8 Msps the two-second fall is
    // 0.99999999228 a sample, which a float rounds to 1.0, and a reference
    // that never falls leaves the threshold where an earlier, louder band
    // put it.
    attack_coefficient_ = std::exp(-1.0 / (0.010 * sample_rate));
    decay_coefficient_ = std::exp(-1.0 / (2.0 * sample_rate));
    // How far either side of a detected impulse to blank.
    //
    // An impulse is microseconds long, and after the front end's own bandwidth
    // limiting it smears to about one over the sample rate. A few microseconds
    // covers it. This was 0.2 ms in an earlier version, which at 1.5 Msps is
    // 307 samples either side - so every impulse wiped out 40% of the band and
    // the blanker did far more damage than the noise.
    blank_span_ = std::clamp(static_cast<int>(std::lround(sample_rate * 4e-6)), 1, 16);
    reset();
}

void NoiseBlanker::set_strength(float strength) {
    strength_.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
}

void NoiseBlanker::reset() {
    reference_ = 0.0;
    blanked_ = 0;
    total_ = 0;
    blanked_fraction_ = 0.0f;
}

namespace {

// Shared body: `magnitude_of` reads a sample, `blank_at` clears one.
/**
 * Longer than any impulse, shorter than a stuck blanker.
 *
 * An ignition spark is a handful of samples and the widest mains burst is
 * tens. A blank that has run for hundreds without a break is not noise being
 * removed, it is the detector holding the whole band down.
 */
constexpr size_t kMaxBurstSamples = 256;

/**
 * Fills a blanked run by continuing the signal into it rather than by drawing
 * a line across it.
 *
 * Interpolation was tried first and measured to do nothing at all, and the
 * reason is arithmetic: at 384 kHz a 40 kHz carrier turns over in 9.6 samples,
 * so a 40-sample burst spans four whole cycles, and a straight line across
 * four cycles of a sinusoid is no closer to it than silence. What a gap that
 * long needs is a prediction of what the signal was doing, not a shortcut
 * between its ends.
 *
 * The prediction is the simplest one that fits: the dominant component of a
 * narrow passband is one rotating phasor, so the average rotation per sample
 * over the samples before the gap is estimated and continued through it. The
 * same is done backwards from the far edge, and the two are cross-faded, so
 * the fill meets real signal at both ends instead of stepping at one.
 *
 * It reconstructs the strongest thing in the passband, which is the thing the
 * gate's sidebands are built from. Everything else it treats as noise, which
 * for a few tens of samples it may as well be.
 */
void bridge(cfloat* samples, size_t count, size_t from, size_t to) {
    const size_t span = to - from;
    if (span == 0) return;
    // How many samples the rotation is estimated over. Measured against the
    // sidebands the gate leaves beside the carrier: 4 samples gives -23.7 dBc,
    // 8 gives -24.0, 32 gives -26.1, 64 gives -27.1, 256 gives -28.3, and 512
    // gives -28.4. It saturates at 256, which is 0.7 ms at 384 kHz.
    constexpr size_t kFit = 256;
    if (from < kFit || to + kFit > count) return;

    // Average rotation per sample either side, as a unit phasor.
    auto rotation = [](const cfloat* x, size_t n, int step) {
        cfloat sum(0.0f, 0.0f);
        for (size_t i = 0; i < n; i++) {
            const cfloat a = x[static_cast<long>(i) * step];
            const cfloat b = x[(static_cast<long>(i) + 1) * step];
            sum += b * std::conj(a);
        }
        const float magnitude = magnitude_of(sum);
        return magnitude > 0.0f ? sum / magnitude : cfloat(1.0f, 0.0f);
    };

    const cfloat left_rotation = rotation(samples + from - kFit, kFit - 1, 1);
    // Read backwards from the last sample of the right-hand window, samples
    // [to, to + kFit), which the guard above keeps inside the block.
    const cfloat right_rotation = rotation(samples + to + kFit - 1, kFit - 1, -1);

    cfloat forward = samples[from - 1];
    cfloat backward = samples[to];
    std::vector<cfloat> from_left(span), from_right(span);
    for (size_t i = 0; i < span; i++) {
        forward *= left_rotation;
        from_left[i] = forward;
    }
    for (size_t i = span; i-- > 0;) {
        backward *= right_rotation;
        from_right[i] = backward;
    }
    for (size_t i = 0; i < span; i++) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(span + 1);
        samples[from + i] = from_left[i] * (1.0f - t) + from_right[i] * t;
    }
}

template <typename Read, typename Blank, typename Repair>
size_t blank_impulses(size_t count, double& reference, double attack, double decay, float multiple,
                      int span, Read magnitude_of, Blank blank_at, Repair repair_run) {
    size_t blanked = 0;
    size_t blank_until = 0;
    // How long the current blank has run without a break, and where it began.
    size_t blank_run = 0;
    size_t run_from = 0;

    for (size_t i = 0; i < count; i++) {
        // Read before blanking: the reference must see what actually arrived.
        const float magnitude = magnitude_of(i);
        if (reference <= 0.0) reference = std::max(magnitude, 1e-9f);

        const float threshold = static_cast<float>(reference) * multiple;

        // The reference is updated on EVERY sample, with the magnitude clamped
        // to the threshold. Two traps live here, and both silence the band:
        //
        //  - skipping the update for blanked samples means a reference that
        //    starts or drifts too low blanks everything, which stops it
        //    updating, forever;
        //  - skipping it for a whole blanked span slows recovery by the length
        //    of that span, which is enough to keep a band muted for seconds
        //    after it comes up.
        //
        // Clamping bounds an impulse's influence to one ordinary sample's
        // worth while leaving the tracker free to move.
        const float used = std::min(magnitude, threshold);
        // While a burst is being blanked the reference may still move, but
        // only at the slow rate. Letting it rise quickly is how a wide burst
        // escapes: every sample of it is clamped to the threshold, so the
        // reference climbs toward that, the threshold climbs with it, and the
        // tail of the burst ends up under a threshold its own head lifted.
        // The blanker looked like it worked, because the leading edge went and
        // the rest stayed. Measured on 100 Hz mains bursts, that cost 23 dB at
        // ordinary settings.
        //
        // Slowed only for as long as this could still be a burst. Past that
        // it is not a burst, it is the trap described below - a reference that
        // started too low, blanking everything, with no way up - and the fast
        // rise is exactly what is needed to climb out. A real impulse is a
        // handful of samples and the widest mains burst is tens; a blank that
        // has run for hundreds is a stuck blanker.
        const bool blanking = i < blank_until;
        const bool still_plausibly_a_burst = blanking && blank_run < kMaxBurstSamples;
        const double rising = still_plausibly_a_burst ? decay : attack;
        const double coefficient = used > reference ? rising : decay;
        reference = coefficient * reference + (1.0 - coefficient) * used;

        if (magnitude > threshold) {
            // Zeroing rather than clipping: a clipped impulse still has a
            // broadband spectrum, and keeping it out of the passband is the
            // whole point. The span reaches backwards too, for the leading
            // edge the detector needed a sample or two to notice.
            const size_t from = i > static_cast<size_t>(span) ? i - span : 0;
            for (size_t j = from; j < i; j++) {
                blank_at(j);
                blanked++;
            }
            blank_until = i + span + 1;
        }

        if (i < blank_until) {
            blank_at(i);
            blanked++;
            if (blank_run == 0) run_from = i;
            blank_run++;
        } else {
            // Where a repair would go. Both edges of the run are known here,
            // which is what any fill needs; nothing does it yet.
            if (blank_run > 0) repair_run(run_from, i);
            blank_run = 0;
        }
    }
    // A run still open at the end of the block has no right-hand edge yet.
    if (blank_run > 0) repair_run(run_from, count);
    return blanked;
}

}  // namespace

void NoiseBlanker::process(cfloat* samples, size_t count) {
    if (!active() || count == 0) return;

    // Multiples of the tracked envelope. A Rayleigh-distributed envelope -
    // what noise through a narrow filter looks like - reaches four or five
    // times its mean only rarely, so even the most aggressive setting leaves
    // real headroom above the signal. Impulses sit tens of times above it.
    const float threshold_multiple = 12.0f - 6.0f * strength();

    blanked_ += blank_impulses(
        count, reference_, attack_coefficient_, decay_coefficient_, threshold_multiple, blank_span_,
        [&](size_t i) { return magnitude_of(samples[i]); },
        [&](size_t i) { samples[i] = cfloat(0.0f, 0.0f); },
        [&](size_t from, size_t to) { bridge(samples, count, from, to); });

    account(count);
}

void NoiseBlanker::process_real(float* samples, size_t count) {
    if (!active() || count == 0) return;

    const float threshold_multiple = 12.0f - 6.0f * strength();
    blanked_ += blank_impulses(
        count, reference_, attack_coefficient_, decay_coefficient_, threshold_multiple, blank_span_,
        [&](size_t i) { return std::fabs(samples[i]); }, [&](size_t i) { samples[i] = 0.0f; },
        [&](size_t, size_t) {});

    account(count);
}

void NoiseBlanker::account(size_t count) {
    total_ += count;
    if (total_ >= 65536) {
        blanked_fraction_ = static_cast<float>(blanked_) / static_cast<float>(total_);
        blanked_ = 0;
        total_ = 0;
    }
}

}  // namespace fernsdr
