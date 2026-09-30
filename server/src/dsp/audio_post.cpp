#include "audio_post.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fernsdr {

namespace {

// Noise floor estimator: a per-bin minimum tracker with a slow upward leak,
// the classic minimum-statistics approach.  A plain average cannot work here,
// because it rises to include whatever signal is present and then gates it
// away; the minimum only sees the gaps between syllables and CW elements.
//
// The leak lets the estimate recover when the real noise floor rises (band
// noise coming up, an antenna switch).  ~0.15% per frame is a few seconds at
// the frame rates we run.
constexpr float kNoiseLeakPerFrame = 1.0015f;
constexpr float kEnergySmoothing = 0.25f;

// The tracked minimum sits below the mean noise power, so it is scaled up
// before being used as the decision threshold.  This is the bias compensation
// that minimum statistics requires; without it nothing is ever gated.
constexpr float kBiasCompensation = 3.0f;

// A consequence worth stating plainly: a carrier that is keyed down
// continuously for longer than the leak window looks exactly like noise to
// this estimator and will be suppressed.  That is what the auto-notch is for,
// and it is why noise reduction defaults to off.

// Never attenuate more than this.  Deeper gating sounds like it is chewing
// the signal, and buries the weak stuff users came here for.
constexpr float kMinGain = 0.06f;  // -24 dB

// Temporal smoothing of the per-bin gain; the main defence against the
// warbling "musical noise" that naive spectral subtraction produces.
constexpr float kGainSmoothing = 0.55f;

// Auto-notch: how far above the local background a bin must sit, and how
// hard it is pulled back down.
constexpr float kAutoNotchThreshold = 5.0f;  // ~14 dB
constexpr float kAutoNotchDepth = 0.12f;
constexpr float kTonalAverageRate = 0.03f;
constexpr int kAutoNotchNeighbourhood = 4;

}  // namespace

AudioPost::AudioPost()
    : mdct_(kPostHop),
      window_(make_sine_window(2 * kPostHop)),
      previous_hop_(kPostHop, 0.0f),
      windowed_(2 * kPostHop, 0.0f),
      coeffs_(kPostHop, 0.0f),
      time_(2 * kPostHop, 0.0f),
      overlap_(kPostHop, 0.0f),
      noise_floor_(kPostHop, 0.0f),
      energy_(kPostHop, 0.0f),
      smoothed_energy_(kPostHop, 0.0f),
      gain_state_(kPostHop, 1.0f),
      tonal_average_(kPostHop, 0.0f),
      notch_mask_(kPostHop, 1.0f) {}

void AudioPost::configure(double sample_rate) {
    sample_rate_ = sample_rate;
    notch_mask_dirty_ = true;
    reset();
}

void AudioPost::set_noise_reduction(float strength) {
    nr_strength_ = std::clamp(strength, 0.0f, 1.0f);
}

void AudioPost::set_auto_notch(bool enabled) { auto_notch_ = enabled; }

void AudioPost::set_notches(const std::vector<Notch>& notches) {
    notches_ = notches;
    notch_mask_dirty_ = true;
}

void AudioPost::reset() {
    std::fill(previous_hop_.begin(), previous_hop_.end(), 0.0f);
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    std::fill(noise_floor_.begin(), noise_floor_.end(), 0.0f);
    std::fill(gain_state_.begin(), gain_state_.end(), 1.0f);
    std::fill(tonal_average_.begin(), tonal_average_.end(), 0.0f);
    primed_ = false;
}

void AudioPost::process(float* samples) {
    if (!active()) return;

    for (size_t i = 0; i < kPostHop; i++) {
        windowed_[i] = previous_hop_[i] * window_[i];
        windowed_[kPostHop + i] = samples[i] * window_[kPostHop + i];
    }
    std::memcpy(previous_hop_.data(), samples, kPostHop * sizeof(float));

    mdct_.forward(windowed_.data(), coeffs_.data());

    if (nr_strength_ > 0.0f) apply_noise_reduction(coeffs_.data());
    if (auto_notch_) apply_auto_notch(coeffs_.data());
    if (!notches_.empty()) apply_manual_notches(coeffs_.data());

    mdct_.inverse(coeffs_.data(), time_.data());
    for (size_t i = 0; i < kPostHop; i++) {
        samples[i] = overlap_[i] + time_[i] * window_[i];
        overlap_[i] = time_[kPostHop + i] * window_[kPostHop + i];
    }
}

void AudioPost::apply_noise_reduction(float* coeffs) {
    // The MDCT is not shift-invariant: a steady tone's coefficient swings
    // frame to frame with the window phase, and single-bin magnitudes are far
    // too jittery to drive a gate.  Local energy across three bins is a stable
    // stand-in for the spectral magnitude and costs almost nothing.
    for (size_t k = 0; k < kPostHop; k++) {
        const float below = k > 0 ? coeffs[k - 1] : 0.0f;
        const float above = k + 1 < kPostHop ? coeffs[k + 1] : 0.0f;
        energy_[k] = (below * below + coeffs[k] * coeffs[k] + above * above) * (1.0f / 3.0f);
    }

    for (size_t k = 0; k < kPostHop; k++) {
        if (!primed_) {
            smoothed_energy_[k] = energy_[k];
            noise_floor_[k] = energy_[k];
        } else {
            smoothed_energy_[k] += kEnergySmoothing * (energy_[k] - smoothed_energy_[k]);
            if (smoothed_energy_[k] < noise_floor_[k]) {
                noise_floor_[k] = smoothed_energy_[k];
            } else {
                noise_floor_[k] = std::min(noise_floor_[k] * kNoiseLeakPerFrame, smoothed_energy_[k]);
            }
        }

        const float threshold = kBiasCompensation * noise_floor_[k] + 1e-20f;
        // Wiener gain from the a-posteriori SNR.
        const float snr = std::max(0.0f, smoothed_energy_[k] / threshold - 1.0f);
        float gain = snr / (snr + 1.0f);
        gain = std::max(gain, kMinGain);

        gain_state_[k] = kGainSmoothing * gain_state_[k] + (1.0f - kGainSmoothing) * gain;
        // Blend toward unity so partial strength means partial reduction.
        const float applied = 1.0f + nr_strength_ * (gain_state_[k] - 1.0f);
        coeffs[k] *= applied;
        // Behind a squelch the energies decay into subnormals and stay there,
        // and the whole stage costs more than twice as much. Far below
        // anything audible, so zero.
        if (smoothed_energy_[k] < 1e-30f) smoothed_energy_[k] = 0.0f;
        if (noise_floor_[k] < 1e-30f) noise_floor_[k] = 0.0f;
    }
    primed_ = true;
}

void AudioPost::apply_auto_notch(float* coeffs) {
    for (size_t k = 0; k < kPostHop; k++) {
        tonal_average_[k] += kTonalAverageRate * (std::fabs(coeffs[k]) - tonal_average_[k]);
        if (tonal_average_[k] < 1e-30f) tonal_average_[k] = 0.0f;
    }

    for (size_t k = 0; k < kPostHop; k++) {
        // Compare each bin against its neighbourhood, excluding itself and its
        // immediate neighbours so a strong carrier does not raise its own
        // reference level.
        float background = 0.0f;
        int counted = 0;
        for (int d = -kAutoNotchNeighbourhood; d <= kAutoNotchNeighbourhood; d++) {
            if (d >= -1 && d <= 1) continue;
            const long idx = static_cast<long>(k) + d;
            if (idx < 0 || idx >= static_cast<long>(kPostHop)) continue;
            background += tonal_average_[idx];
            counted++;
        }
        if (counted == 0) continue;
        background /= counted;

        if (tonal_average_[k] > background * kAutoNotchThreshold) coeffs[k] *= kAutoNotchDepth;
    }
}

void AudioPost::rebuild_notch_mask() {
    std::fill(notch_mask_.begin(), notch_mask_.end(), 1.0f);
    const double bin_hz = sample_rate_ / (2.0 * static_cast<double>(kPostHop));
    for (const Notch& notch : notches_) {
        const double half = std::max(bin_hz, notch.width_hz / 2.0);
        for (size_t k = 0; k < kPostHop; k++) {
            const double f = (static_cast<double>(k) + 0.5) * bin_hz;
            const double distance = std::fabs(f - notch.center_hz);
            if (distance <= half) {
                notch_mask_[k] = 0.0f;
            } else if (distance <= half * 2.0) {
                // Soft shoulder, so a notch does not ring.
                const double t = (distance - half) / half;
                notch_mask_[k] = std::min(notch_mask_[k], static_cast<float>(t));
            }
        }
    }
    notch_mask_dirty_ = false;
}

void AudioPost::apply_manual_notches(float* coeffs) {
    if (notch_mask_dirty_) rebuild_notch_mask();
    for (size_t k = 0; k < kPostHop; k++) coeffs[k] *= notch_mask_[k];
}

}  // namespace fernsdr
