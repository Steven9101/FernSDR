// Post-demodulation audio processing: noise reduction, auto-notch and manual
// notches.
//
// All three live in one MDCT, aligned to the codec's 128-sample hop, so
// enabling them costs a single extra transform pair rather than one per
// feature.  When nothing is enabled the whole stage is bypassed and adds no
// latency at all - the common case stays exactly as low-latency as it would
// be without this file existing.
#pragma once
#include <cstddef>
#include <vector>

#include "mdct.h"

namespace fernsdr {

// Hop size; must match the codec's frame hop so the two transforms line up.
constexpr size_t kPostHop = 128;

struct Notch {
    double center_hz = 0.0;
    double width_hz = 200.0;
};

class AudioPost {
public:
    AudioPost();

    void configure(double sample_rate);

    // 0 disables noise reduction; 1 applies the full Wiener gain.
    void set_noise_reduction(float strength);
    float noise_reduction() const { return nr_strength_; }

    void set_auto_notch(bool enabled);
    bool auto_notch() const { return auto_notch_; }

    void set_notches(const std::vector<Notch>& notches);
    const std::vector<Notch>& notches() const { return notches_; }

    // True when any stage is enabled.  While false, process() is a no-op and
    // the audio path has zero added delay.
    bool active() const { return nr_strength_ > 0.0f || auto_notch_ || !notches_.empty(); }

    // Adds this many samples of delay while active.
    static constexpr size_t latency_samples() { return kPostHop; }

    // Processes exactly kPostHop samples in place.
    void process(float* samples);

    void reset();

private:
    void apply_noise_reduction(float* coeffs);
    void apply_auto_notch(float* coeffs);
    void apply_manual_notches(float* coeffs);
    void rebuild_notch_mask();

    double sample_rate_ = 12000.0;
    float nr_strength_ = 0.0f;
    bool auto_notch_ = false;
    std::vector<Notch> notches_;

    Mdct mdct_;
    std::vector<float> window_;
    std::vector<float> previous_hop_;
    std::vector<float> windowed_;
    std::vector<float> coeffs_;
    std::vector<float> time_;
    std::vector<float> overlap_;

    std::vector<float> noise_floor_;   // per-bin noise estimate
    std::vector<float> energy_;        // three-bin local energy, the magnitude proxy
    std::vector<float> smoothed_energy_;
    std::vector<float> gain_state_;    // smoothed NR gain, limits musical noise
    std::vector<float> tonal_average_; // slow per-bin average, for auto-notch
    std::vector<float> notch_mask_;
    bool notch_mask_dirty_ = true;
    bool primed_ = false;
};

}  // namespace fernsdr
