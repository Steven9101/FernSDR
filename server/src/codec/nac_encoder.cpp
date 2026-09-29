#include "nac.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>

namespace fernsdr {
namespace nac {

const uint8_t kBandWidths[kNumBands] = {
    4, 4, 4, 4, 4, 4, 4, 4,   // 0 .. 31   - narrowest, where tuning errors bite
    8, 8, 8, 8, 8, 8,         // 32 .. 79
    16, 16, 16,               // 80 .. 127
};

constexpr uint8_t kBandStarts[kNumBands + 1] = {
    0,  4,  8,  12, 16, 20, 24, 28, 32,
    40, 48, 56, 64, 72, 80,
    96, 112, 128,
};

// The band layout is written out by hand and its last entry is the coefficient
// count, so it is silently tied to the frame length. Halving kFrameHop leaves
// this table indexing past the end of every frame, which corrupts the heap and
// surfaces as a free() abort a long way from the cause. Found the hard way
// while measuring whether a shorter frame would be worth its lower latency.
static_assert(kBandStarts[kNumBands] == kNumCoeffs,
              "the band table must end at the coefficient count: change the frame length and "
              "this table has to be rewritten with it");

namespace {

// What one frame works on and no frame keeps: the windowed samples and their
// coefficients. One set per thread rather than per encoder: a thousand
// listeners' encoders taking turns on a worker each found their own copy cold
// at every frame. Plain arrays, so the thread's copy needs no construction
// and costs nothing to reach.
struct alignas(64) FrameScratch {
    float windowed[2 * kFrameHop];
    float coeffs[kNumCoeffs];
};

FrameScratch& frame_scratch() {
    thread_local FrameScratch scratch;
    return scratch;
}

const float* shared_sine_window() {
    static const std::vector<float> window = make_sine_window(2 * kFrameHop);
    return window.data();
}

inline int32_t quantise(float normalised, float gain) {
    return static_cast<int32_t>(std::lrintf(normalised * gain));
}

int exponent_cost(int delta) {
    uint32_t value = zigzag_encode(delta) + 1;
    int bits = 1;
    while (value >>= 1) bits += 2;
    return bits;
}

}  // namespace

Encoder::Encoder(int sample_rate)
    : sample_rate_(sample_rate),
      mdct_(kFrameHop),
      window_(shared_sine_window()),
      overlap_(kFrameHop, 0.0f),
      normalised_(kNumCoeffs, 0.0f),
      band_exponent_(kNumBands, 0),
      band_active_(kNumBands, 0) {
    set_bitrate(bitrate_);
    frame_.reserve(256);
    active_coeffs_.reserve(kNumCoeffs);
    noise_.configure(sample_rate_, target_.passband_low_hz, target_.passband_high_hz);
    set_target(target_);
}

void Encoder::set_bitrate(int bits_per_second) {
    // At high sample rates even an all-silent frame needs more than 8 kbit/s:
    // the quality and band mask occupy three whole bytes. Report that floor
    // instead of promising a ceiling the format cannot represent.
    constexpr int minimum_frame_bits = (kQualityBits + kNumBands + 7) / 8 * 8;
    const int header_bitrate = static_cast<int>(std::ceil(minimum_frame_bits * frame_rate()));
    bitrate_ = std::max({8000, header_bitrate, bits_per_second});
    target_bits_per_frame_ = static_cast<int>(bitrate_ / frame_rate()) / 8 * 8;
}

void Encoder::set_max_quality_index(int index) {
    max_quality_index_ = std::clamp(index, 0, kMaxQualityIndex);
}

void Encoder::reset() {
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    noise_.reset();
}

void Encoder::set_target(const Nac3Target& target) {
    target_ = target;
    noise_.set_passband(target.passband_low_hz, target.passband_high_hz);
    const auto power = [](float db) { return std::pow(10.0f, db / 10.0f); };
    margin_ = power(-target.noise_margin_db);
    finest_ = power(-target.max_snr_db);
    coarsest_ = power(-target.min_snr_db);
    relevance_inside_ = power(-(target.noise_margin_db + 24.0f));
    relevance_outside_ = power(-24.0f);
}

void Encoder::analyse(const float* samples) {
    FrameScratch& frame = frame_scratch();
    for (size_t i = 0; i < kFrameHop; i++) {
        // A broken source must not poison the overlap of every later frame
        // or reach a float-to-integer conversion as NaN. This bound is far
        // beyond normalised receiver audio, including brief AGC overshoot,
        // and keeps every coefficient inside the range the decoder accepts
        // as genuine rather than corrupt.
        const float sample = std::isfinite(samples[i]) ? std::clamp(samples[i], -1e4f, 1e4f) : 0.0f;
        frame.windowed[i] = overlap_[i] * window_[i];
        frame.windowed[kFrameHop + i] = sample * window_[kFrameHop + i];
        overlap_[i] = sample;
    }
    mdct_.forward(frame.windowed, frame.coeffs);
}

const std::vector<uint8_t>& Encoder::encode(const float* samples, bool allow_compact) {
    return encode(samples, allow_compact ? Layout::Compact : Layout::Original);
}

const std::vector<uint8_t>& Encoder::encode(const float* samples, Layout layout) {
    if (layout == Layout::PerBand) {
        begin_packet();
        add_frame(samples);
        return finish_packet();
    }
    analyse(samples);
    const float* coeffs = frame_scratch().coeffs;
    const bool allow_compact = layout == Layout::Compact;

    // Per-band scale factors.  RMS rather than peak: the Rice coder handles
    // the occasional outlier more cheaply than raising the whole band's step.
    float band_rms[kNumBands];
    float peak_rms = 0.0f;
    for (int b = 0; b < kNumBands; b++) {
        const int start = kBandStarts[b];
        const int width = kBandWidths[b];
        double energy = 0.0;
        for (int i = 0; i < width; i++) {
            const double v = coeffs[start + i];
            energy += v * v;
        }
        band_rms[b] = static_cast<float>(std::sqrt(energy / width));
        peak_rms = std::max(peak_rms, band_rms[b]);
    }
    const float floor_rms = std::max(kSilenceThreshold,
                                     peak_rms * std::exp2(-0.25f * kDynamicRangeQuarterSteps));

    int active = 0;
    for (int b = 0; b < kNumBands; b++) {
        const int start = kBandStarts[b];
        const int width = kBandWidths[b];
        const float rms = band_rms[b];
        if (rms < floor_rms) {
            band_active_[b] = 0;
            band_exponent_[b] = 0;
            for (int i = 0; i < width; i++) normalised_[start + i] = 0.0f;
            continue;
        }
        band_active_[b] = 1;
        active++;
        int exponent = static_cast<int>(std::lrintf(4.0f * std::log2(rms)));
        exponent = std::clamp(exponent, -200, 200);
        band_exponent_[b] = exponent;
        // Normalise against the *quantised* exponent, not the true RMS, so the
        // rate estimate matches what emit() will actually produce.
        const float inv = std::exp2(static_cast<float>(-exponent) * 0.25f);
        for (int i = 0; i < width; i++) normalised_[start + i] = coeffs[start + i] * inv;
    }

    prepare_costs();
    // Binary search for the highest quality that fits the frame budget.  The
    // cost is monotone in quality, so this converges in six probes.
    int lo = 0;
    int hi = max_quality_index_;
    int chosen = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (estimate_bits(mid) <= target_bits_per_frame_) {
            chosen = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    // Even the coarsest quality can overflow on a dense frame at a very low
    // target.  Drop the quietest bands until it fits rather than emitting an
    // oversized frame.
    while (estimate_bits(chosen) > target_bits_per_frame_ && active > 0) {
        int weakest = -1;
        for (int b = 0; b < kNumBands; b++) {
            if (!band_active_[b]) continue;
            if (weakest < 0 || band_exponent_[b] < band_exponent_[weakest]) weakest = b;
        }
        if (weakest < 0) break;
        band_active_[weakest] = 0;
        active--;
        prepare_costs();
    }

    int scale_bias = 0;
    const int original_bits = estimate_bits(chosen, &scale_bias);
    emit(chosen, scale_bias, original_bits, allow_compact);

    stats_.quality_index = chosen;
    stats_.payload_bits = static_cast<int>(frame_.size() * 8);
    stats_.active_bands = active;
    return frame_;
}

void Encoder::prepare_costs() {
    active_coeffs_.clear();
    int previous[3] = {kExponentReference, kExponentReference, kExponentReference};
    for (int& bits : side_bits_) bits = kQualityBits + kNumBands;
    for (int b = 0; b < kNumBands; b++) {
        if (!band_active_[b]) continue;
        for (int c = 0; c < 3; c++) {
            const int exponent = band_exponent_[b] + 4 * c;
            if (exponent > 200) { side_bits_[c] = 1000000000; continue; }
            const uint32_t delta = zigzag_encode(exponent - previous[c]);
            uint32_t nbits = 0;
            for (uint32_t v = delta + 1; (v >> nbits) > 1u;) nbits++;
            side_bits_[c] += 2 * static_cast<int>(nbits) + 1;
            previous[c] = exponent;
        }
        active_coeffs_.insert(active_coeffs_.end(), normalised_.begin() + kBandStarts[b],
                              normalised_.begin() + kBandStarts[b] + kBandWidths[b]);
    }
}

int Encoder::estimate_bits(int quality_index, int* scale_bias) const {
    // Raising every exponent and the transmitted quality by the same amount
    // leaves the reconstruction step unchanged. It does change Rice's k,
    // which otherwise encodes large coefficients with expensive unary tails.
    // Compare three equivalent representations, including the original, so a
    // sparse or low-rate frame can keep the cheaper original representation.
    // Existing decoders already understand all three; there is no new header
    // or prediction state to recover after a lost frame.
    int costs[3] = {side_bits_[0], side_bits_[1], side_bits_[2]};
    const int candidates = std::min(3, (kMaxQualityIndex - quality_index) / 4 + 1);
    const float gain = std::exp2(static_cast<float>(quality_index) * 0.25f);

    const uint32_t k0 = rice_k_for_quality(quality_index);
    const uint32_t k1 = rice_k_for_quality(quality_index + 4);
    const uint32_t k2 = rice_k_for_quality(quality_index + 8);
    for (float normalised : active_coeffs_) {
        const uint32_t q = zigzag_encode(quantise(normalised, gain));
        costs[0] += static_cast<int>(rice_cost(q, k0));
        costs[1] += static_cast<int>(rice_cost(q, k1));
        costs[2] += static_cast<int>(rice_cost(q, k2));
    }
    int best = 0;
    for (int c = 1; c < candidates; c++) if (costs[c] < costs[best]) best = c;
    if (scale_bias) *scale_bias = best * 4;
    return costs[best];
}

void Encoder::emit(int quality_index, int scale_bias, int original_bits, bool allow_compact) {
    int first = kNumBands, last = -1, active = 0;
    for (int b = 0; b < kNumBands; b++) {
        if (!band_active_[b]) continue;
        first = std::min(first, b);
        last = b;
        active++;
    }
    const bool contiguous = active == last - first + 1;
    const int mask_mode = active == kNumBands ? 1 :
        (active == 0 || (first == 0 && contiguous)) ? 2 : contiguous ? 3 : 0;
    const int mask_bits = mask_mode == 0 ? kNumBands : mask_mode == 1 ? 0 : mask_mode == 2 ? 5 : 10;
    int scale_mode = 0;
    bool compact = false;
    if (allow_compact) {
        int costs[6]{};
        int previous = first < kNumBands ? band_exponent_[first] : 0;
        for (int b = first + 1; b < kNumBands; b++) {
            if (!band_active_[b]) continue;
            const int delta = band_exponent_[b] - previous;
            costs[0] += exponent_cost(delta);
            for (uint32_t mode = 1; mode < 6; mode++) costs[mode] += rice_cost(zigzag_encode(delta), mode - 1);
            previous = band_exponent_[b];
        }
        for (int mode = 1; mode < 6; mode++) if (costs[mode] < costs[scale_mode]) scale_mode = mode;
        const int compact_side = kQualityBits + 2 + mask_bits + (active ? 12 + costs[scale_mode] : 0);
        const int compact_bits = original_bits - side_bits_[scale_bias / 4] + compact_side;
        // Choose the quantizer with the original budget, then remove redundant
        // metadata. Including these savings in the quality search would spend
        // them again and leave the listener's transfer rate unchanged.
        compact = (compact_bits + 7) / 8 < (original_bits + 7) / 8;
    }
    layout_ = compact ? Layout::Compact : Layout::Original;

    writer_.reset();
    writer_.put_bits(static_cast<uint32_t>(quality_index + scale_bias), kQualityBits);
    if (compact) writer_.put_bits(mask_mode, 2);
    if (!compact || mask_mode == 0) {
        for (int b = 0; b < kNumBands; b++) writer_.put_bit(band_active_[b]);
    } else if (mask_mode == 2) {
        writer_.put_bits(active, 5);
    } else if (mask_mode == 3) {
        writer_.put_bits(first, 5);
        writer_.put_bits(active, 5);
    }

    const uint32_t k = rice_k_for_quality(quality_index + scale_bias);
    const float gain = std::exp2(static_cast<float>(quality_index) * 0.25f);

    int prev_exponent = kExponentReference;
    for (int b = 0; b < kNumBands; b++) {
        if (!band_active_[b]) continue;
        const int exponent = band_exponent_[b] + scale_bias;
        if (compact && b == first) {
            writer_.put_bits(exponent + 200, 9);
            writer_.put_bits(scale_mode, 3);
        } else if (compact && scale_mode != 0) {
            writer_.put_signed_rice(exponent - prev_exponent, scale_mode - 1);
        } else {
            writer_.put_signed_exp_golomb(exponent - prev_exponent);
        }
        prev_exponent = band_exponent_[b] + scale_bias;
    }
    for (int b = 0; b < kNumBands; b++) {
        if (!band_active_[b]) continue;
        const int start = kBandStarts[b];
        const int width = kBandWidths[b];
        for (int i = 0; i < width; i++) {
            writer_.put_signed_rice(quantise(normalised_[start + i], gain), k);
        }
    }
    frame_ = writer_.finish();
}

}  // namespace nac
}  // namespace fernsdr

// --- NAC3 -------------------------------------------------------------------

namespace fernsdr {
namespace nac {

namespace {

// 2^(-s/4) for a quarter-octave step index s, from a table of the same exp2
// the encoder would otherwise call per band per frame: identical values, so
// the frames stay bit for bit what they were. Built on first use rather than
// as a global, whose construction other files' globals could run before.
float inverse_step(int step) {
    struct Table {
        float value[kMaxStep - kMinStep + 1];
        Table() {
            for (int s = kMinStep; s <= kMaxStep; s++) value[s - kMinStep] = std::exp2(-0.25f * static_cast<float>(s));
        }
    };
    static const Table table;
    return table.value[step - kMinStep];
}

#if defined(__GNUC__) && (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
#define FERNSDR_NAC_LANES 1
// Four coefficients at a time: every band is a whole number of fours and
// starts on one; see kBandStarts.
typedef float Floats __attribute__((vector_size(16)));
typedef int32_t Ints __attribute__((vector_size(16)));
typedef uint32_t Codes __attribute__((vector_size(16)));
constexpr bool whole_vectors() {
    for (int b = 0; b <= kNumBands; b++) {
        if (kBandStarts[b] % 4 != 0) return false;
    }
    return true;
}
static_assert(whole_vectors(), "every band must start and end on a multiple of four coefficients");

template <typename Vector, typename Scalar>
inline Vector load(const Scalar* p) {
    Vector v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename Vector, typename Scalar>
inline void store(Scalar* p, Vector v) { std::memcpy(p, &v, sizeof v); }
template <typename Vector>
inline uint32_t lane_sum(Vector v) {
    return static_cast<uint32_t>(v[0]) + static_cast<uint32_t>(v[1]) + static_cast<uint32_t>(v[2]) +
           static_cast<uint32_t>(v[3]);
}
// A cast between vector types of one size keeps the bits, as a bit_cast would.
inline Codes zigzag(Ints q) { return ((Codes)q << 1) ^ (Codes)(q >> 31); }

// rice_cost() of four codes: the escape costs the same whatever k is.
inline Codes rice_costs(Codes codes, uint32_t k) {
    const Codes q = codes >> k;
    const Codes within = (Codes)(q < Codes{} + kRiceEscape);
    return (within & (q + (1 + k))) | (~within & (kRiceEscape + 1 + 32));
}
#endif

// One band's coefficients over their step, scaled by `factor` and rounded
// half away from zero into `out`. Returns the sum of their zigzag codes, and
// in `nonzero` how many did not round to zero.
uint64_t quantise_band(const float* scaled, float factor, int width, int32_t* out, int& nonzero) {
#ifdef FERNSDR_NAC_LANES
    // The codes are summed a nibble apart: a band's sixteen codes can pass
    // 2^32, and their high parts cannot.
    Codes high{}, low{};
    Ints zeros{};  // a comparison is -1 where true, so subtracting counts
    for (int i = 0; i < width; i += 4) {
        const Floats value = load<Floats>(scaled + i) * factor;
        // A half with the sign bit set where the value is below zero: -0.0
        // is not, as it is not in the scalar comparison.
        const Ints negative = value < Floats{};
        const Floats half = (Floats)((Ints)(Floats{} + 0.5f) | (negative & (Ints{} + INT32_MIN)));
        const Ints q = __builtin_convertvector(value + half, Ints);
        store(out + i, q);
        const Codes codes = zigzag(q);
        high += codes >> 4;
        low += codes & 15u;
        zeros -= q == Ints{};
    }
    nonzero = width - static_cast<int>(lane_sum(zeros));
    return (static_cast<uint64_t>(lane_sum(high)) << 4) + lane_sum(low);
#else
    uint64_t sum = 0;
    nonzero = 0;
    for (int i = 0; i < width; i++) {
        const float value = scaled[i] * factor;
        // Round half away from zero. The decoder only multiplies back, so the
        // encoder may round however it likes, and this stays inline where
        // lrintf is a library call per coefficient.
        const int32_t q = static_cast<int32_t>(value + (value < 0.0f ? -0.5f : 0.5f));
        out[i] = q;
        sum += zigzag_encode(q);
        nonzero += q != 0;
    }
    return sum;
#endif
}

// What `width` quantised coefficients cost as Rice codes with parameters
// lowest, lowest + 1, ... for `candidates` of them, each priced as rice_cost().
void rice_band_costs(const int32_t* quantised, int width, int lowest, int candidates, int* costs) {
#ifdef FERNSDR_NAC_LANES
    Codes totals[3]{};
    for (int i = 0; i < width; i += 4) {
        const Codes codes = zigzag(load<Ints>(quantised + i));
        for (int c = 0; c < candidates; c++) totals[c] += rice_costs(codes, static_cast<uint32_t>(lowest + c));
    }
    for (int c = 0; c < candidates; c++) costs[c] = static_cast<int>(lane_sum(totals[c]));
#else
    for (int c = 0; c < candidates; c++) costs[c] = 0;
    for (int i = 0; i < width; i++) {
        const uint32_t mapped = zigzag_encode(quantised[i]);
        for (int c = 0; c < candidates; c++) costs[c] += static_cast<int>(rice_cost(mapped, static_cast<uint32_t>(lowest + c)));
    }
#endif
}

// The quarter-octave histogram bin of one coefficient's energy, read from the
// float's exponent and top two mantissa bits rather than with a logarithm.
// The four bins in an octave are therefore equal in linear width, not in log
// width; that only has to be monotonic and the same everywhere.
inline int energy_bin(float energy, int lowest_octave, int bins) {
    if (!(energy > 0.0f)) return 0;
    uint32_t bits;
    std::memcpy(&bits, &energy, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127;
    const int quarter = static_cast<int>((bits >> 21) & 3);
    return std::clamp((exponent - lowest_octave) * 4 + quarter, 0, bins - 1);
}

// Mask selector and its field width, shared by NAC2 and NAC3 frames.
struct MaskShape {
    int mode = 0;
    int bits = 0;
    int first = 0;
    int count = 0;
};

MaskShape mask_shape(const uint8_t* active) {
    int first = kNumBands, last = -1, count = 0;
    for (int b = 0; b < kNumBands; b++) {
        if (!active[b]) continue;
        first = std::min(first, b);
        last = b;
        count++;
    }
    MaskShape shape;
    shape.count = count;
    shape.first = count ? first : 0;
    const bool contiguous = count == last - first + 1;
    shape.mode = count == kNumBands ? 1 : (count == 0 || (first == 0 && contiguous)) ? 2 : contiguous ? 3 : 0;
    shape.bits = 2 + (shape.mode == 0 ? kNumBands : shape.mode == 1 ? 0 : shape.mode == 2 ? 5 : 10);
    return shape;
}

// The smoothed energy of a band that holds only noise touches its minimum
// well below its mean. Measured for this smoothing and one-second window on
// white noise; see tests/test_nac.cpp.
constexpr float kMinimumBias = 2.2f;
// The 20th percentile of a chi-square distribution with one degree of
// freedom, relative to its mean.
constexpr float kPercentile20 = 0.0642f;

}  // namespace

void NoiseEstimate::configure(int sample_rate, float passband_low_hz, float passband_high_hz) {
    sample_rate_ = sample_rate;
    set_passband(passband_low_hz, passband_high_hz);
    const float frame_rate = static_cast<float>(sample_rate) / static_cast<float>(kFrameHop);
    // About a second of memory. Long enough to see the gaps between CW
    // elements and syllables; short enough to follow the AGC after a strong
    // station leaves the passband.
    decay_ = std::exp(-1.0f / frame_rate);
    subwindow_frames_ = std::max(1, static_cast<int>(std::lround(frame_rate / kSubwindows)));
    history_frames_ = std::max(1, static_cast<int>(std::lround(frame_rate * 0.5f)));
    reset();
}

void NoiseEstimate::set_passband(float passband_low_hz, float passband_high_hz) {
    const float nyquist = 0.5f * static_cast<float>(sample_rate_);
    float low = std::isfinite(passband_low_hz) ? std::clamp(passband_low_hz, 0.0f, nyquist) : 0.0f;
    float high = std::isfinite(passband_high_hz) && passband_high_hz > 0.0f ? std::min(passband_high_hz, nyquist)
                                                                             : nyquist;
    if (high <= low) {
        low = 0.0f;
        high = nyquist;
    }
    const int coefficients = static_cast<int>(kNumCoeffs);
    const float hz = nyquist / static_cast<float>(coefficients);
    // Coefficient c is centred on (c + 0.5) * hz.
    first_coefficient_ = std::clamp(static_cast<int>(std::ceil(low / hz - 0.5f)), 0, coefficients - 1);
    last_coefficient_ = std::clamp(static_cast<int>(std::floor(high / hz - 0.5f)) + 1, first_coefficient_ + 1,
                                   coefficients);
    bool any = false;
    for (int b = 0; b < kNumBands; b++) {
        const float centre = (static_cast<float>(kBandStarts[b]) + 0.5f * kBandWidths[b]) * hz;
        band_in_passband_[b] = centre >= low && centre <= high;
        any = any || band_in_passband_[b];
    }
    if (!any) {
        // A passband narrower than any band still has a band around it.
        const float middle = 0.5f * (low + high) / hz;
        for (int b = 0; b < kNumBands; b++) {
            if (middle >= kBandStarts[b] && middle < kBandStarts[b + 1]) band_in_passband_[b] = 1;
        }
    }
}

void NoiseEstimate::reset() {
    std::fill(std::begin(histogram_), std::end(histogram_), 0.0f);
    lowest_bin_ = kBins;
    weight_ = 1.0;
    total_ = 0.0;
    frames_ = 0;
    std::fill(std::begin(smoothed_), std::end(smoothed_), 0.0f);
    for (auto& row : window_minimum_) std::fill(std::begin(row), std::end(row), FLT_MAX);
    std::fill(std::begin(current_minimum_), std::end(current_minimum_), FLT_MAX);
    subwindow_position_ = 0;
    subwindow_count_ = 0;
    std::fill(std::begin(band_noise_), std::end(band_noise_), 0.0f);
    passband_noise_ = 0.0f;
    short_term_noise_ = 0.0f;
    gain_ = 1.0f;
    std::fill(std::begin(history_), std::end(history_), FLT_MAX);
    history_position_ = 0;
    history_count_ = 0;
}

void NoiseEstimate::update(const float* coefficients, const float* band_energy, float power_gain) {
    // An AGC gain of 60 dB is 1e6; anything outside a generous range is a
    // caller's mistake, and must not poison the estimate.
    gain_ = std::isfinite(power_gain) && power_gain > 1e-12f && power_gain < 1e12f ? power_gain : 1.0f;
    const float unscale = 1.0f / gain_;
    // Digital silence says nothing about the channel. A closed squelch hands
    // the encoder zeros, and letting them in would pin the long memory near
    // zero for sixteen seconds after it opens, coding the returning noise
    // far more finely than anyone can hear.
    double passband_energy = 0.0;
    for (int c = first_coefficient_; c < last_coefficient_; c++) {
        passband_energy += static_cast<double>(coefficients[c]) * coefficients[c];
    }
    if (!(passband_energy * unscale > 1e-14 * (last_coefficient_ - first_coefficient_))) return;
    // New observations weigh 1/decay more than the previous frame's, which is
    // the same as decaying every bin without touching them. Rescale before
    // the weights leave float range.
    weight_ /= decay_;
    if (weight_ > 1e30) {
        const float scale = static_cast<float>(1.0 / weight_);
        for (float& bin : histogram_) bin *= scale;
        total_ /= weight_;
        weight_ = 1.0;
    }
    const float weight = static_cast<float>(weight_);
    for (int c = first_coefficient_; c < last_coefficient_; c++) {
        const int bin = energy_bin(coefficients[c] * coefficients[c] * unscale, kLowestOctave, kBins);
        histogram_[bin] += weight;
        lowest_bin_ = std::min(lowest_bin_, bin);
    }
    total_ += weight_ * (last_coefficient_ - first_coefficient_);

    for (int b = 0; b < kNumBands; b++) {
        const float energy = std::isfinite(band_energy[b]) ? band_energy[b] * unscale : 0.0f;
        smoothed_[b] = frames_ ? 0.8f * smoothed_[b] + 0.2f * energy : energy;
        current_minimum_[b] = std::min(current_minimum_[b], smoothed_[b]);
    }
    if (++subwindow_position_ >= subwindow_frames_) {
        subwindow_position_ = 0;
        for (int b = 0; b < kNumBands; b++) {
            window_minimum_[b][subwindow_count_ % kSubwindows] = current_minimum_[b];
            current_minimum_[b] = FLT_MAX;
        }
        subwindow_count_++;
    }
    frames_++;
    // The estimate moves over seconds. Refresh every frame while it forms,
    // then every fourth.
    if (frames_ <= 8 || frames_ % 4 == 0) refresh();
    if (frames_ % history_frames_ == 0) {
        history_[history_position_] = short_term_noise_;
        history_position_ = (history_position_ + 1) % kHistory;
        history_count_ = std::min(kHistory, history_count_ + 1);
    }
}

void NoiseEstimate::refresh() {
    const double wanted = 0.2 * total_;
    double cumulative = 0.0;
    // Everything below the lowest bin ever added to is zero, and adds nothing
    // to the sum. With nothing added the start does not matter: the result
    // is zero whatever bin the scan stops on.
    int bin = std::min(lowest_bin_, kBins - 1);
    for (; bin < kBins - 1; bin++) {
        cumulative += histogram_[bin];
        if (cumulative >= wanted) break;
    }
    const float representative = std::ldexp(1.0f + (static_cast<float>(bin % 4) + 0.5f) * 0.25f,
                                            bin / 4 + kLowestOctave);
    short_term_noise_ = total_ > 0.0 ? representative / kPercentile20 : 0.0f;
    float pooled = short_term_noise_;
    // A one-second estimate that has not seen a quiet moment lands on the
    // signals. The longer memory holds the lowest of the recent ones; each
    // is already an average over a second of the whole passband, so their
    // minimum needs no bias correction worth the name.
    for (int h = 0; h < history_count_; h++) pooled = std::min(pooled, history_[h]);
    passband_noise_ = pooled;

    for (int b = 0; b < kNumBands; b++) {
        float minimum = current_minimum_[b];
        for (int w = 0; w < kSubwindows; w++) minimum = std::min(minimum, window_minimum_[b][w]);
        const float local = minimum < FLT_MAX ? minimum * kMinimumBias : passband_noise_;
        // The pooled estimate caps a band's floor: a carrier or a long
        // transmission holding one band up for seconds cannot pass for noise.
        // The band's own minimum may only lower it, as in the filter's
        // transition bands or FM's de-emphasised top octave.
        band_noise_[b] = band_in_passband_[b] ? std::min(local, passband_noise_) : local;
    }
}

void Encoder::begin_packet() {
    writer_.reset();
    // The frame count goes first; its value is known only when the packet is
    // finished, so two zero bits hold its place.
    writer_.put_bits(0, 2);
    packet_frames_ = 0;
}

const std::vector<uint8_t>& Encoder::finish_packet() {
    frame_ = writer_.finish();
    if (!frame_.empty() && packet_frames_ > 0) {
        frame_[0] = static_cast<uint8_t>(frame_[0] | ((packet_frames_ - 1) << 6));
    }
    writer_.reset();
    packet_frames_ = 0;
    layout_ = Layout::PerBand;
    return frame_;
}

void Encoder::add_frame(const float* samples) {
    if (packet_frames_ == 0) begin_packet();
    analyse(samples);
    const float* coeffs = frame_scratch().coeffs;
    const bool predicted = packet_frames_ > 0;

    for (int b = 0; b < kNumBands; b++) {
        double energy = 0.0;
        for (int i = kBandStarts[b]; i < kBandStarts[b + 1]; i++) {
            energy += static_cast<double>(coeffs[i]) * coeffs[i];
        }
        band_energy_[b] = static_cast<float>(energy / kBandWidths[b]);
    }
    noise_.update(coeffs, band_energy_, signal_gain_);

    // Inside the passband nothing needs coding far below the noise there.
    // Outside it the channel filter has already removed the signal; what is
    // left is its stopband and transition, and content 24 dB below the
    // passband noise is neither audible nor decodable. Coding it anyway costs
    // a band's side information for nothing.
    const float passband = noise_.passband_noise();
    const float relevance_inside = passband * relevance_inside_;
    const float relevance_outside = passband * relevance_outside_;
    for (int b = 0; b < kNumBands; b++) {
        const bool inside = noise_.in_passband(b);
        const float noise = target_.band_minimum || !inside ? noise_.band_noise(b) : passband;
        float allowed = noise * margin_;
        allowed = std::max(allowed, band_energy_[b] * finest_);
        allowed = std::min(allowed, band_energy_[b] * coarsest_);
        allowed = std::max(allowed, inside ? relevance_inside : relevance_outside);
        // A uniform quantiser with step s adds s^2/12 of noise per coefficient;
        // step indices are quarter octaves, so index = 4*log2(s) = 2*log2(12*noise).
        const float step_energy = 12.0f * allowed;
        band_step_[b] = step_energy > 0.0f && std::isfinite(step_energy)
                            ? std::clamp(static_cast<int>(std::lrintf(2.0f * std::log2(step_energy))), kMinStep, kMaxStep)
                            : kMinStep;
        // Below the relevance floor a band is silent outright. Quantising it
        // at that floor instead still lets its occasional largest coefficient
        // round to one, which buys the band's side information for nothing.
        band_dropped_[b] = band_energy_[b] < (inside ? relevance_inside : relevance_outside);
        const float inverse = inverse_step(band_step_[b]);
        for (int i = kBandStarts[b]; i < kBandStarts[b + 1]; i++) {
            // The target never asks for a step this far below a coefficient,
            // but a float-to-integer conversion must not see an unbounded value.
            scaled_[i] = std::clamp(coeffs[i] * inverse, -1.0e9f, 1.0e9f);
        }
    }

    // A frame costs what its targets need. Only when that exceeds the
    // ceiling does every band give way together, a quarter octave at a time,
    // which keeps the noise rise even across the passband. The packet's
    // frame count is charged to its first frame.
    const int budget = target_bits_per_frame_ - (predicted ? 0 : 2);
    int coarser = 0;
    std::fill(std::begin(silent_from_), std::end(silent_from_), INT_MAX);
    int bits = per_band_cost(0, predicted);
    if (bits > budget) {
        // The frame size falls roughly linearly with each quarter octave of
        // coarsening: about a quarter of a bit per coefficient still worth
        // coding. Bracket the smallest coarsening that fits, starting from
        // that estimate and narrowing by secants: usually three costings
        // instead of a six-probe bisection on every frame the ceiling binds.
        int fits = 64, fits_bits = -1;           // known to fit (64 if nothing tried)
        int over = 0, over_bits = bits;          // known not to fit
        int guess = previous_coarser_ > 0 ? previous_coarser_ : std::clamp(static_cast<int>(std::ceil(
            (bits - budget) / std::max(1.0f, 0.25f * static_cast<float>(coded_coefficients_)))), 1, 64);
        while (fits - over > 1) {
            guess = std::clamp(guess, over + 1, fits - 1);
            const int cost = per_band_cost(guess, predicted);
            if (cost <= budget) {
                fits = guess;
                fits_bits = cost;
            } else {
                over = guess;
                over_bits = cost;
            }
            if (fits - over <= 1) break;
            // Interpolate between the bracket's ends when both are known,
            // otherwise extrapolate from the overshoot's slope.
            if (fits_bits >= 0 && over_bits > fits_bits) {
                guess = over + static_cast<int>(std::ceil(static_cast<double>(over_bits - budget) * (fits - over) /
                                                          (over_bits - fits_bits)));
            } else {
                guess = over + std::max(1, static_cast<int>(std::ceil(
                    (over_bits - budget) / std::max(1.0f, 0.25f * static_cast<float>(coded_coefficients_)))));
            }
        }
        coarser = fits;
        // The search usually ends on the costing it keeps; only redo it when
        // the last one was a coarsening that did not fit.
        bits = costed_coarser_ == coarser ? fits_bits : per_band_cost(coarser, predicted);
        // Only an absurdly small ceiling reaches this: drop the quietest bands.
        while (bits > budget) {
            int weakest = -1;
            for (int b = 0; b < kNumBands; b++) {
                if (coded_active_[b] && (weakest < 0 || band_energy_[b] < band_energy_[weakest])) weakest = b;
            }
            if (weakest < 0) break;
            band_dropped_[weakest] = 1;
            bits = per_band_cost(coarser, predicted);
        }
    }
    previous_coarser_ = coarser;
    const size_t before = writer_.bits_written();
    emit_frame(predicted);
    update_references(!predicted);
    packet_frames_++;
    stats_.quality_index = coarser;
    stats_.payload_bits = static_cast<int>(writer_.bits_written() - before) + (predicted ? 0 : 2);
    stats_.side_bits = per_band_side_bits_;
    int active = 0;
    for (int b = 0; b < kNumBands; b++) active += coded_active_[b];
    stats_.active_bands = active;
}

int Encoder::per_band_cost(int coarser, bool predicted) {
    int bits = 0;
    int side = 0;
    coded_coefficients_ = 0;
    const float coarsening = inverse_step(coarser);
    int previous_step = 0, previous_rice = 0;
    bool first = true;
    int step_costs[6]{};
    for (int b = 0; b < kNumBands; b++) {
        coded_active_[b] = 0;
        const int start = kBandStarts[b];
        const int width = kBandWidths[b];
        if (band_dropped_[b] || coarser >= silent_from_[b]) {
            for (int i = 0; i < width; i++) quantised_[start + i] = 0;
            continue;
        }
        const int step = std::min(kMaxStep, band_step_[b] + coarser);
        // Coarsening every band alike scales every coefficient alike.
        const float factor = step == band_step_[b] + coarser ? coarsening : inverse_step(step - band_step_[b]);
        int nonzero = 0;
        const uint64_t sum = quantise_band(scaled_ + start, factor, width, quantised_ + start, nonzero);
        if (sum == 0) {
            silent_from_[b] = std::min(silent_from_[b], coarser);
            continue;
        }
        coded_coefficients_ += nonzero;
        coded_active_[b] = 1;
        coded_step_[b] = step;

        // The mean mapped value puts the best Rice parameter within one of
        // floor(log2(mean)); the exact cost decides between the neighbours,
        // priced in one pass over the band.
        const uint64_t mean = sum / static_cast<uint64_t>(width);
        int guess = 0;
        while (guess < kMaxRiceParameter && (uint64_t{1} << (guess + 1)) <= mean) guess++;
        const int lowest = std::max(0, guess - 1);
        const int candidates = std::min(kMaxRiceParameter, guess + 1) - lowest + 1;
        int costs[3]{};
        rice_band_costs(quantised_ + start, width, lowest, candidates, costs);
        int best_rice = lowest, best_cost = costs[0];
        for (int c = 1; c < candidates; c++) {
            if (costs[c] < best_cost) {
                best_cost = costs[c];
                best_rice = lowest + c;
            }
        }
        band_rice_[b] = best_rice;
        bits += best_cost;

        // Step and Rice parameter residuals: against the previous frame's
        // values for this band, or along the bands of an independent frame.
        int step_residual, rice_residual;
        if (predicted) {
            step_residual = step - reference_step_[b];
            rice_residual = best_rice - reference_rice_[b];
        } else if (first) {
            side += 9 + 3 + 4;
            first = false;
            previous_step = step;
            previous_rice = best_rice;
            continue;
        } else {
            step_residual = step - previous_step;
            rice_residual = best_rice - previous_rice;
        }
        step_costs[0] += exponent_cost(step_residual);
        for (uint32_t mode = 1; mode < 6; mode++) step_costs[mode] += rice_cost(zigzag_encode(step_residual), mode - 1);
        side += static_cast<int>(rice_cost(zigzag_encode(rice_residual), 0));
        previous_step = step;
        previous_rice = best_rice;
    }
    int selector = 0;
    for (int mode = 1; mode < 6; mode++) {
        if (step_costs[mode] < step_costs[selector]) selector = mode;
    }
    step_selector_ = selector;
    const MaskShape shape = mask_shape(coded_active_);
    if (predicted) {
        const bool same = std::equal(std::begin(coded_active_), std::end(coded_active_), std::begin(reference_active_));
        side += 1 + (same ? 0 : shape.bits) + (shape.count ? 3 : 0);
    } else {
        side += shape.bits;
    }
    side += step_costs[selector];
    per_band_side_bits_ = side;
    costed_coarser_ = coarser;
    return bits + side;
}

void Encoder::emit_frame(bool predicted) {
    const MaskShape shape = mask_shape(coded_active_);
    bool write_mask = true;
    if (predicted) {
        const bool same = std::equal(std::begin(coded_active_), std::end(coded_active_), std::begin(reference_active_));
        writer_.put_bit(same ? 1 : 0);
        write_mask = !same;
    }
    if (write_mask) {
        writer_.put_bits(static_cast<uint32_t>(shape.mode), 2);
        if (shape.mode == 0) {
            for (int b = 0; b < kNumBands; b++) writer_.put_bit(coded_active_[b]);
        } else if (shape.mode == 2) {
            writer_.put_bits(static_cast<uint32_t>(shape.count), 5);
        } else if (shape.mode == 3) {
            writer_.put_bits(static_cast<uint32_t>(shape.first), 5);
            writer_.put_bits(static_cast<uint32_t>(shape.count), 5);
        }
    }
    if (shape.count == 0) return;
    const auto put_step = [&](int residual) {
        if (step_selector_ == 0) writer_.put_signed_exp_golomb(residual);
        else writer_.put_signed_rice(residual, static_cast<uint32_t>(step_selector_ - 1));
    };
    if (predicted) {
        writer_.put_bits(static_cast<uint32_t>(step_selector_), 3);
        for (int b = 0; b < kNumBands; b++) {
            if (coded_active_[b]) put_step(coded_step_[b] - reference_step_[b]);
        }
        for (int b = 0; b < kNumBands; b++) {
            if (coded_active_[b]) writer_.put_signed_rice(band_rice_[b] - reference_rice_[b], 0);
        }
    } else {
        bool first = true;
        int previous = 0;
        for (int b = 0; b < kNumBands; b++) {
            if (!coded_active_[b]) continue;
            if (first) {
                writer_.put_bits(static_cast<uint32_t>(coded_step_[b] - kMinStep), 9);
                writer_.put_bits(static_cast<uint32_t>(step_selector_), 3);
                first = false;
            } else {
                put_step(coded_step_[b] - previous);
            }
            previous = coded_step_[b];
        }
        first = true;
        for (int b = 0; b < kNumBands; b++) {
            if (!coded_active_[b]) continue;
            if (first) writer_.put_bits(static_cast<uint32_t>(band_rice_[b]), 4);
            else writer_.put_signed_rice(band_rice_[b] - previous, 0);
            previous = band_rice_[b];
            first = false;
        }
    }
    for (int b = 0; b < kNumBands; b++) {
        if (!coded_active_[b]) continue;
        writer_.put_signed_rice_run(quantised_ + kBandStarts[b], kBandWidths[b], static_cast<uint32_t>(band_rice_[b]));
    }
}

// What the next frame of the packet predicts from. After the first frame a
// band it left silent borrows the nearest active band's values, looking left
// first, so a band that wakes up in a later frame starts close to its
// neighbours. Later frames only refresh the bands they carried.
void Encoder::update_references(bool first) {
    for (int b = 0; b < kNumBands; b++) {
        if (coded_active_[b]) {
            reference_step_[b] = coded_step_[b];
            reference_rice_[b] = band_rice_[b];
        }
        reference_active_[b] = coded_active_[b];
    }
    if (!first) return;
    fill_references(coded_active_, reference_step_, reference_rice_);
}

}  // namespace nac
}  // namespace fernsdr
