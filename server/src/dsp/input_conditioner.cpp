#include "input_conditioner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fernsdr {

namespace {

// How fast each estimate follows the band. The DC offset moves when the
// tuner's gain changes and should be gone well within a second of it. The
// balance is a property of the hardware that drifts with temperature;
// following it slowly is what keeps one strong signal's own modulation out
// of it.
constexpr double kDcSeconds = 0.25;
constexpr double kBalanceSeconds = 2.0;
// The DC correction moves from one block's estimate to the next in steps
// this long rather than jumping at the block boundary.
constexpr size_t kDcStep = 64;
// Floats are summed in runs this long before joining a double, so a long
// block does not lose its small terms against a large total.
constexpr size_t kRun = 1024;
// An imbalance larger than this is not a front end to be corrected but a
// broken one, or a band holding one signal whose I and Q are not independent;
// the estimate is held inside it rather than followed.
constexpr double kMaxSinPhase = 0.5;
constexpr double kMaxAmplitudeRatio = 2.0;

// How much of a new block goes into an estimate that has seen `seen` samples
// before it. At first every sample counts equally, a plain mean, so the start
// does not hang on whatever one short first block happened to hold; after one
// time constant's worth the exponential forgetting takes over, and the two
// weights meet there without a step.
double weight(double time_constant, double sample_rate, double count, double seen) {
    const double exponential = 1.0 - std::exp(-count / (time_constant * sample_rate));
    return std::max(exponential, count / (seen + count));
}

float power_db(double power) { return static_cast<float>(10.0 * std::log10(power + 1e-16)); }

#if defined(__GNUC__) && (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
#define FERNSDR_CONDITIONER_LANES 1
// Four float lanes over interleaved samples: I, Q, I, Q. memcpy gives
// unaligned loads and stores without promising an alignment the buffers do
// not have.
typedef float Lanes __attribute__((vector_size(16)));
inline Lanes load(const float* p) {
    Lanes v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store(float* p, Lanes v) { std::memcpy(p, &v, sizeof v); }

// Lanes rearranged in the register. Taking the neighbours from memory one
// float over instead reads what the previous store has only just written,
// and the stall on every iteration made the vector loop slower than the
// scalar one.
#if defined(__clang__)
inline Lanes swap_pairs(Lanes x) { return __builtin_shufflevector(x, x, 1, 0, 3, 2); }
inline Lanes spread_i(Lanes x) { return __builtin_shufflevector(x, x, 0, 0, 2, 2); }
#else
typedef int LaneOrder __attribute__((vector_size(16)));
inline Lanes swap_pairs(Lanes x) { return __builtin_shuffle(x, LaneOrder{1, 0, 3, 2}); }
inline Lanes spread_i(Lanes x) { return __builtin_shuffle(x, LaneOrder{0, 0, 2, 2}); }
#endif
#endif

// The sums of I and of Q over `count` interleaved samples.
void sum_arms(const float* f, size_t count, double& sum_i, double& sum_q) {
    sum_i = 0.0;
    sum_q = 0.0;
    const size_t floats = count * 2;
    size_t j = 0;
#ifdef FERNSDR_CONDITIONER_LANES
    while (j + 4 <= floats) {
        Lanes run = {0.0f, 0.0f, 0.0f, 0.0f};
        const size_t end = std::min(floats - floats % 4, j + kRun * 2);
        for (; j < end; j += 4) run += load(f + j);
        sum_i += static_cast<double>(run[0]) + run[2];
        sum_q += static_cast<double>(run[1]) + run[3];
    }
#endif
    for (; j + 1 < floats; j += 2) {
        sum_i += f[j];
        sum_q += f[j + 1];
    }
}

// The sums of I*I, Q*Q and I*Q over `count` interleaved samples.
void arm_products(const float* f, size_t count, double& ii, double& qq, double& iq) {
    ii = 0.0;
    qq = 0.0;
    iq = 0.0;
    const size_t floats = count * 2;
    size_t j = 0;
#ifdef FERNSDR_CONDITIONER_LANES
    // Each sample times itself with I and Q exchanged holds I*Q twice.
    while (j + 4 <= floats) {
        Lanes squares = {0.0f, 0.0f, 0.0f, 0.0f};
        Lanes cross = {0.0f, 0.0f, 0.0f, 0.0f};
        const size_t end = std::min(floats - floats % 4, j + kRun * 2);
        for (; j < end; j += 4) {
            const Lanes x = load(f + j);
            squares += x * x;
            cross += x * swap_pairs(x);
        }
        ii += static_cast<double>(squares[0]) + squares[2];
        qq += static_cast<double>(squares[1]) + squares[3];
        iq += (static_cast<double>(cross[0]) + cross[1] + cross[2] + cross[3]) / 2.0;
    }
#endif
    for (; j + 1 < floats; j += 2) {
        const double i = f[j], q = f[j + 1];
        ii += i * i;
        qq += q * q;
        iq += i * q;
    }
}

// Subtracts (di, dq) from each interleaved sample.
void subtract_arms(float* f, size_t count, float di, float dq) {
    const size_t floats = count * 2;
    size_t j = 0;
#ifdef FERNSDR_CONDITIONER_LANES
    const Lanes offset = {di, dq, di, dq};
    for (; j + 4 <= floats; j += 4) store(f + j, load(f + j) - offset);
#endif
    for (; j + 1 < floats; j += 2) {
        f[j] -= di;
        f[j + 1] -= dq;
    }
}

// Q' = q_gain * Q + i_to_q * I, in place.
void mix_q(float* f, size_t count, float q_gain, float i_to_q) {
    const size_t floats = count * 2;
    size_t j = 0;
#ifdef FERNSDR_CONDITIONER_LANES
    // Each I copied over its own Q, so one multiply-add updates both Q lanes
    // and leaves the I lanes as they were.
    const Lanes keep = {1.0f, q_gain, 1.0f, q_gain};
    const Lanes take = {0.0f, i_to_q, 0.0f, i_to_q};
    for (; j + 4 <= floats; j += 4) {
        const Lanes x = load(f + j);
        store(f + j, x * keep + spread_i(x) * take);
    }
#endif
    for (; j + 1 < floats; j += 2) f[j + 1] = q_gain * f[j + 1] + i_to_q * f[j];
}

}  // namespace

void InputConditioner::configure(double sample_rate) {
    sample_rate_ = sample_rate;
    reset();
}

void InputConditioner::reset() {
    dc_primed_ = false;
    dc_seen_ = 0.0;
    dc_i_ = dc_q_ = 0.0;
    balance_primed_ = false;
    balance_seen_ = 0.0;
    ii_ = qq_ = iq_ = 0.0;
    q_gain_ = 1.0f;
    i_to_q_ = 0.0f;
    dc_offset_dbfs_.store(-160.0f, std::memory_order_relaxed);
    gain_error_db_.store(0.0f, std::memory_order_relaxed);
    phase_error_degrees_.store(0.0f, std::memory_order_relaxed);
    image_rejection_db_.store(0.0f, std::memory_order_relaxed);
}

void InputConditioner::process(cfloat* samples, size_t count) {
    if (count == 0) return;
    // Read once, so one block sees one decision even if the panel changes it
    // halfway through.
    const bool swap = this->swap();
    const bool dc = dc_remove();
    const bool balance = this->balance();
    if (swap != swapped_) {
        // Each estimate was taken on the other arm; kept, the balance would
        // start by making the mirror images worse than no correction at all.
        swapped_ = swap;
        dc_primed_ = false;
        dc_seen_ = 0.0;
        balance_primed_ = false;
        balance_seen_ = 0.0;
        q_gain_ = 1.0f;
        i_to_q_ = 0.0f;
    }

    if (swap) {
        for (size_t i = 0; i < count; i++) samples[i] = cfloat(samples[i].imag(), samples[i].real());
    }
    if (dc) {
        remove_dc(samples, count);
    } else if (dc_primed_) {
        dc_primed_ = false;
        dc_seen_ = 0.0;
        dc_offset_dbfs_.store(-160.0f, std::memory_order_relaxed);
    }
    if (balance) {
        balance_block(samples, count);
    } else if (balance_primed_) {
        balance_primed_ = false;
        balance_seen_ = 0.0;
        q_gain_ = 1.0f;
        i_to_q_ = 0.0f;
    }
}

void InputConditioner::process_real(float* samples, size_t count) {
    if (count == 0) return;
    if (dc_remove()) {
        remove_dc_real(samples, count);
    } else if (dc_primed_) {
        dc_primed_ = false;
        dc_seen_ = 0.0;
        dc_offset_dbfs_.store(-160.0f, std::memory_order_relaxed);
    }
}

void InputConditioner::remove_dc(cfloat* samples, size_t count) {
    float* f = reinterpret_cast<float*>(samples);
    double sum_i = 0.0, sum_q = 0.0;
    sum_arms(f, count, sum_i, sum_q);
    const double mean_i = sum_i / static_cast<double>(count);
    const double mean_q = sum_q / static_cast<double>(count);

    // The first block is taken at its word, so the spike is gone at once
    // rather than fading out over the first second.
    const double start_i = dc_primed_ ? dc_i_ : mean_i;
    const double start_q = dc_primed_ ? dc_q_ : mean_q;
    const double n = static_cast<double>(count);
    const double a = weight(kDcSeconds, sample_rate_, n, dc_seen_);
    const double next_i = start_i + a * (mean_i - start_i);
    const double next_q = start_q + a * (mean_q - start_q);
    dc_primed_ = true;
    dc_seen_ += n;

    const size_t steps = (count + kDcStep - 1) / kDcStep;
    for (size_t step = 0; step < steps; step++) {
        const double t = (static_cast<double>(step) + 0.5) / static_cast<double>(steps);
        const size_t first = step * kDcStep;
        subtract_arms(f + first * 2, std::min(kDcStep, count - first),
                      static_cast<float>(start_i + (next_i - start_i) * t),
                      static_cast<float>(start_q + (next_q - start_q) * t));
    }
    dc_i_ = next_i;
    dc_q_ = next_q;
    dc_offset_dbfs_.store(power_db(next_i * next_i + next_q * next_q), std::memory_order_relaxed);
}

void InputConditioner::remove_dc_real(float* samples, size_t count) {
    double sum = 0.0;
    size_t j = 0;
#ifdef FERNSDR_CONDITIONER_LANES
    while (j + 4 <= count) {
        Lanes run = {0.0f, 0.0f, 0.0f, 0.0f};
        const size_t end = std::min(count - count % 4, j + kRun * 2);
        for (; j < end; j += 4) run += load(samples + j);
        sum += static_cast<double>(run[0]) + run[1] + run[2] + run[3];
    }
#endif
    for (; j < count; j++) sum += samples[j];
    const double mean = sum / static_cast<double>(count);

    const double start = dc_primed_ ? dc_i_ : mean;
    const double n = static_cast<double>(count);
    const double next = start + weight(kDcSeconds, sample_rate_, n, dc_seen_) * (mean - start);
    dc_primed_ = true;
    dc_seen_ += n;

    const size_t steps = (count + kDcStep - 1) / kDcStep;
    for (size_t step = 0; step < steps; step++) {
        const double t = (static_cast<double>(step) + 0.5) / static_cast<double>(steps);
        const float offset = static_cast<float>(start + (next - start) * t);
        const size_t first = step * kDcStep;
        const size_t end = std::min(count, first + kDcStep);
        for (size_t k = first; k < end; k++) samples[k] -= offset;
    }
    dc_i_ = next;
    dc_offset_dbfs_.store(power_db(next * next), std::memory_order_relaxed);
}

void InputConditioner::balance_block(cfloat* samples, size_t count) {
    float* f = reinterpret_cast<float*>(samples);
    double ii = 0.0, qq = 0.0, iq = 0.0;
    arm_products(f, count, ii, qq, iq);
    const double n = static_cast<double>(count);
    ii /= n;
    qq /= n;
    iq /= n;

    // Silence, or an input that has stopped, says nothing about the balance.
    if (ii > 1e-20 && qq > 1e-20) {
        const double a = weight(kBalanceSeconds, sample_rate_, n, balance_primed_ ? balance_seen_ : 0.0);
        ii_ += a * (ii - ii_);
        qq_ += a * (qq - qq_);
        iq_ += a * (iq - iq_);
        balance_primed_ = true;
        balance_seen_ += n;
        // Q = A (Q0 cos(phi) + I0 sin(phi)), with I0 and Q0 what the band
        // would have been. A is the ratio of the arms' amplitudes; once Q is
        // brought back to I's amplitude, its correlation with I is sin(phi);
        // taking that part out and scaling what is left by 1 / cos(phi)
        // gives Q0 back.
        const double amplitude = std::clamp(std::sqrt(qq_ / ii_), 1.0 / kMaxAmplitudeRatio, kMaxAmplitudeRatio);
        const double sin_phase = std::clamp(iq_ / (ii_ * amplitude), -kMaxSinPhase, kMaxSinPhase);
        const double cos_phase = std::sqrt(1.0 - sin_phase * sin_phase);
        q_gain_ = static_cast<float>(1.0 / (amplitude * cos_phase));
        i_to_q_ = static_cast<float>(-sin_phase / cos_phase);

        const double folded = 2.0 * amplitude * cos_phase;
        const double wanted = 1.0 + folded + amplitude * amplitude;
        const double image = std::max(1e-12, 1.0 - folded + amplitude * amplitude);
        gain_error_db_.store(static_cast<float>(20.0 * std::log10(amplitude)), std::memory_order_relaxed);
        phase_error_degrees_.store(static_cast<float>(std::asin(sin_phase) * 180.0 / 3.14159265358979323846),
                                   std::memory_order_relaxed);
        image_rejection_db_.store(static_cast<float>(10.0 * std::log10(wanted / image)), std::memory_order_relaxed);
    }
    if (balance_primed_) mix_q(f, count, q_gain_, i_to_q_);
}

}  // namespace fernsdr
