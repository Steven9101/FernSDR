#include "waterfall_codec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fernsdr {
namespace wfc {

uint32_t choose_rice_param(const int* residuals, size_t count) {
    if (count == 0) return 0;
    // Mean magnitude gives a near-optimal k for a Laplacian; probing the two
    // neighbours costs almost nothing and covers the rounding cases.
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) sum += zigzag_encode(residuals[i]);
    const uint64_t mean = sum / count;

    uint32_t guess = 0;
    while ((1ull << (guess + 1)) < mean + 1 && guess < kMaxRiceParam) guess++;

    uint32_t best_k = guess;
    uint64_t best_cost = UINT64_MAX;
    const uint32_t lo = guess > 0 ? guess - 1 : 0;
    const uint32_t hi = std::min(guess + 1, kMaxRiceParam);
    for (uint32_t k = lo; k <= hi; k++) {
        uint64_t cost = 0;
        for (size_t i = 0; i < count; i++) cost += rice_cost(zigzag_encode(residuals[i]), k);
        if (cost < best_cost) {
            best_cost = cost;
            best_k = k;
        }
    }
    return best_k;
}

void LineEncoder::reset() { previous_.clear(); }

namespace {
struct Quantizer {
    int low, high, seed;
};
constexpr Quantizer kQuantizers[] = {{-200, 100, -100}, {-100, 50, -50}};

template<uint32_t Mode>
int predict(size_t i, const int* current, const int* previous, const Quantizer& q) {
    const int left = i ? current[i - 1] : q.seed;
    if constexpr (Mode == kModeIntra) return left;
    if constexpr (Mode == kModeTemporal) return previous[i];
    if constexpr (Mode == kModeLinear) return i > 1 ? std::clamp(2 * left - current[i - 2], q.low, q.high) : left;
    if (i == 0) return previous[0];
    const int above = previous[i], diagonal = previous[i - 1];
    return std::clamp(left + above - diagonal, std::min(left, above), std::max(left, above));
}

int predict(uint32_t mode, size_t i, const std::vector<int>& current, const std::vector<int>& previous, const Quantizer& q) {
    switch (mode) {
        case kModeTemporal: return predict<kModeTemporal>(i, current.data(), previous.data(), q);
        case kModeIntra: return predict<kModeIntra>(i, current.data(), previous.data(), q);
        case kModeGradient: return predict<kModeGradient>(i, current.data(), previous.data(), q);
        default: return predict<kModeLinear>(i, current.data(), previous.data(), q);
    }
}

size_t run_bits(size_t count) {
    size_t bits = 1;
    while (count >>= 1) bits += 2;
    return bits;
}

struct PredictionCosts {
    std::array<uint32_t, 2 * (kMaxLevelQ - kMinLevelQ) + 1> counts{};
    size_t zeros = 0, run_cost = 0;
    uint32_t maximum = 0;
};

// Zeros and what their runs cost as zero runs, from a bit per pixel set where
// the residual is zero: each maximal run costs 1 + run_bits(its length).
// Walking the runs a word at a time replaces a branch per pixel on whether
// it was zero, which noisy rows mispredicted about as often as not.
void zero_runs(const uint64_t* mask, size_t width, size_t& zeros, size_t& run_cost) {
    zeros = 0;
    run_cost = 0;
    size_t run = 0;
    const size_t words = (width + 63) / 64;
    for (size_t w = 0; w < words; w++) {
        uint64_t bits = mask[w];
        const size_t valid = w + 1 == words && width % 64 ? width % 64 : 64;
        zeros += static_cast<size_t>(__builtin_popcountll(bits));
        size_t position = 0;
        while (position < valid) {
            if (bits & 1) {
                const size_t ones = std::min<size_t>(valid - position, ~bits ? __builtin_ctzll(~bits) : 64);
                run += ones;
                position += ones;
                bits = ones >= 64 ? 0 : bits >> ones;
            } else {
                if (run) {
                    run_cost += 1 + run_bits(run);
                    run = 0;
                }
                const size_t gap = std::min<size_t>(valid - position, bits ? __builtin_ctzll(bits) : 64);
                position += gap;
                bits = gap >= 64 ? 0 : bits >> gap;
            }
        }
    }
    if (run) run_cost += 1 + run_bits(run);
}

// One row's residuals under each prediction mode, kept from costing to
// writing so the chosen mode's are written without predicting again. Per
// thread, like the other codecs' scratch.
struct ModeResiduals {
    std::vector<int32_t> values[4];
};

ModeResiduals& mode_residuals(size_t width) {
    thread_local ModeResiduals scratch;
    for (auto& values : scratch.values) {
        if (values.size() < width) values.resize(width);
    }
    return scratch;
}

template<uint32_t Mode>
void predict_row(const int* current, const int* previous, size_t width, const Quantizer& q, int32_t* out) {
    for (size_t i = 0; i < width; i++) out[i] = current[i] - predict<Mode>(i, current, previous, q);
}

// Histograms, maxima and zero masks for several modes in one pass. A
// histogram whose neighbouring pixels land in the same bin waits for each
// increment's store before the next; with every mode's pixel in one
// iteration those waits overlap instead of adding up. Rows are at most 4,096
// pixels wide, so a mask is at most 64 words.
template<size_t Modes>
void collect_costs(const int32_t* const (&residuals)[Modes], size_t width, PredictionCosts* const (&costs)[Modes]) {
    uint64_t masks[Modes][64];
    uint32_t maxima[Modes] = {};
    for (size_t start = 0; start < width; start += 64) {
        uint64_t words[Modes] = {};
        const size_t end = std::min(width, start + 64);
        for (size_t i = start; i < end; i++) {
            for (size_t m = 0; m < Modes; m++) {
                const uint32_t residual = zigzag_encode(residuals[m][i]);
                costs[m]->counts[residual]++;
                maxima[m] = std::max(maxima[m], residual);
                words[m] |= static_cast<uint64_t>(residual == 0) << (i - start);
            }
        }
        for (size_t m = 0; m < Modes; m++) masks[m][start / 64] = words[m];
    }
    for (size_t m = 0; m < Modes; m++) {
        costs[m]->maximum = maxima[m];
        zero_runs(masks[m], width, costs[m]->zeros, costs[m]->run_cost);
    }
}

size_t residual_cost(const std::vector<int>& values, uint32_t k, bool zero_runs) {
    size_t bits = 0;
    for (size_t i = 0; i < values.size();) {
        if (zero_runs && values[i] == 0) {
            const size_t start = i++;
            while (i < values.size() && values[i] == 0) i++;
            bits += 1 + run_bits(i - start);
        } else {
            bits += (zero_runs ? 1 : 0) + rice_cost(zigzag_encode(values[i++]), k);
        }
    }
    return bits;
}
}  // namespace

const std::vector<uint8_t>& LineEncoder::encode(const float* db, size_t width, bool force_intra,
                                               bool allow_zero_runs, bool allow_adaptive, int step_db) {
    if (width < 1 || width > 4096 || (step_db != 1 && step_db != 2)) { reset(); frame_.clear(); return frame_; }
    if (step_db != step_db_) reset();
    step_db_ = step_db;
    const auto& q = kQuantizers[step_db - 1];
    current_.resize(width);
    for (size_t i = 0; i < width; i++) current_[i] = quantise_db(db[i], step_db);

    adaptive_ = false;
    if (allow_adaptive) return encode_adaptive(width, force_intra, allow_zero_runs);
    bool intra = force_intra || previous_.size() != width;
    spatial_.resize(width);
    int predictor = q.seed;
    for (size_t i = 0; i < width; i++) {
        spatial_[i] = current_[i] - predictor;
        predictor = current_[i];
    }

    uint32_t k = choose_rice_param(spatial_.data(), width);
    size_t best_bits = residual_cost(spatial_, k, false);
    zero_runs_ = false;
    if (allow_zero_runs) {
        const size_t bits = residual_cost(spatial_, k, true);
        if (bits < best_bits) { best_bits = bits; zero_runs_ = true; }
    }
    if (!intra) {
        temporal_.resize(width);
        for (size_t i = 0; i < width; i++) temporal_[i] = current_[i] - previous_[i];
        const uint32_t temporal_k = choose_rice_param(temporal_.data(), width);
        size_t temporal_bits = residual_cost(temporal_, temporal_k, false);
        bool temporal_runs = false;
        if (allow_zero_runs) {
            const size_t bits = residual_cost(temporal_, temporal_k, true);
            if (bits < temporal_bits) { temporal_bits = bits; temporal_runs = true; }
        }
        // A changing noise floor can be cheaper to describe across this row
        // than against the last one. Both modes use the existing wire format
        // and preserve the same 1 dB quantisation.
        intra = best_bits <= temporal_bits;
        if (!intra) { k = temporal_k; zero_runs_ = temporal_runs; }
    }
    const auto& residuals = intra ? spatial_ : temporal_;

    writer_.reset();
    writer_.put_bits(intra ? kModeIntra : kModeTemporal, kModeBits);
    writer_.put_bits(k, kRiceParamBits);
    for (size_t i = 0; i < width;) {
        if (zero_runs_ && residuals[i] == 0) {
            const size_t start = i++;
            while (i < width && residuals[i] == 0) i++;
            writer_.put_bit(0);
            writer_.put_exp_golomb(static_cast<uint32_t>(i - start - 1));
        } else {
            if (zero_runs_) writer_.put_bit(1);
            writer_.put_signed_rice(residuals[i++], k);
        }
    }
    frame_ = writer_.finish();
    last_bits_ = writer_.bits_written();

    previous_.swap(current_);
    return frame_;
}

const std::vector<uint8_t>& LineEncoder::encode_adaptive(size_t width, bool force_intra, bool allow_zero_runs) {
    // Prediction never leaves the representable range, so residuals have only
    // 601 possible zigzag values. A histogram lets us price every Rice code
    // without rescanning a wide row for each parameter and prediction mode.
    const auto& q = kQuantizers[step_db_ - 1];
    PredictionCosts costs[4];
    const bool independent = force_intra || previous_.size() != width;
    ModeResiduals& modes = mode_residuals(width);
    int32_t* const intra = modes.values[kModeIntra].data();
    int32_t* const linear = modes.values[kModeLinear].data();
    predict_row<kModeIntra>(current_.data(), previous_.data(), width, q, intra);
    predict_row<kModeLinear>(current_.data(), previous_.data(), width, q, linear);
    if (independent) {
        const int32_t* const rows[2] = {intra, linear};
        PredictionCosts* const tallies[2] = {&costs[kModeIntra], &costs[kModeLinear]};
        collect_costs(rows, width, tallies);
    } else {
        int32_t* const temporal = modes.values[kModeTemporal].data();
        int32_t* const gradient = modes.values[kModeGradient].data();
        predict_row<kModeTemporal>(current_.data(), previous_.data(), width, q, temporal);
        predict_row<kModeGradient>(current_.data(), previous_.data(), width, q, gradient);
        const int32_t* const rows[4] = {intra, linear, temporal, gradient};
        PredictionCosts* const tallies[4] = {&costs[kModeIntra], &costs[kModeLinear], &costs[kModeTemporal],
                                             &costs[kModeGradient]};
        collect_costs(rows, width, tallies);
    }

    size_t best_bits = SIZE_MAX;
    uint32_t chosen = kModeIntra, chosen_k = 0;
    zero_runs_ = false;
    for (uint32_t mode = 0; mode < 4; mode++) {
        if (independent && (mode == kModeTemporal || mode == kModeGradient)) continue;
        auto& c = costs[mode];
        size_t raw[10]{};
        // At k+1, Rice's quotient is the quotient at k divided by two.
        // Fold adjacent histogram entries after pricing each k. Even the
        // full 601-symbol range takes fewer than 610 pair visits across all
        // parameters, while preserving the exact 24-bit escape cost.
        uint32_t maximum = c.maximum;
        for (uint32_t k = 0; k < 10; k++) {
            for (uint32_t q = 0; q <= maximum; q += 2) {
                const uint32_t a = c.counts[q];
                const uint32_t b = q < maximum ? c.counts[q + 1] : 0;
                raw[k] += a * (q >= kRiceEscape ? kRiceEscape + 33 : q + 1 + k);
                raw[k] += b * (q + 1 >= kRiceEscape ? kRiceEscape + 33 : q + 2 + k);
                c.counts[q / 2] = a + b;
            }
            maximum /= 2;
        }
        for (uint32_t k = 0; k < 10; k++) {
            const size_t runs = raw[k] - c.zeros * (k + 1) + width - c.zeros + c.run_cost;
            const bool use_runs = allow_zero_runs && runs < raw[k];
            const size_t bits = (use_runs ? runs : raw[k]) + (mode < 2 ? 5 : 6);
            // A payload flag distinguishes the extended predictor header.
            // Retain the original representation on equal padded sizes, so
            // adding a predictor can never make a transmitted row larger.
            if (best_bits == SIZE_MAX || (bits + 7) / 8 < (best_bits + 7) / 8) {
                best_bits = bits;
                chosen = mode;
                chosen_k = k;
                zero_runs_ = use_runs;
            }
        }
    }
    adaptive_ = chosen >= 2;
    writer_.reset();
    writer_.put_bits(chosen, adaptive_ ? 2 : 1);
    writer_.put_bits(chosen_k, kRiceParamBits);
    if (zero_runs_) writer_.put_signed_rice_with_zero_runs(modes.values[chosen].data(), width, chosen_k);
    else writer_.put_signed_rice_run(modes.values[chosen].data(), width, chosen_k);
    frame_ = writer_.finish();
    last_bits_ = writer_.bits_written();
    previous_.swap(current_);
    return frame_;
}

void LineDecoder::reset() { previous_.clear(); }

bool LineDecoder::decode(const uint8_t* data, size_t size, size_t width, float* out, bool zero_runs, bool adaptive, int step_db) {
    if (size == 0 || width < 1 || width > 4096 || (step_db != 1 && step_db != 2)) { reset(); return false; }
    BitReader reader(data, size);
    const uint32_t mode = reader.get_bits(adaptive ? 2 : kModeBits);
    const uint32_t k = reader.get_bits(kRiceParamBits);

    const bool dependent = mode == kModeTemporal || mode == kModeGradient;
    if (dependent && (previous_.size() != width || step_db != step_db_)) { reset(); return false; }
    const auto& q = kQuantizers[step_db - 1];

    std::vector<int> line(width);
    for (size_t i = 0; i < width;) {
        const bool run = zero_runs && reader.get_bit() == 0;
        const uint64_t count = run ? static_cast<uint64_t>(reader.get_exp_golomb()) + 1 : 1;
        if (count > width - i || reader.overrun()) { reset(); return false; }
        const int residual = run ? 0 : reader.get_signed_rice(k);
        for (size_t end = i + count; i < end; i++) {
            const int64_t value = static_cast<int64_t>(predict(mode, i, line, previous_, q)) + residual;
            if (value < q.low || value > q.high || reader.overrun()) { reset(); return false; }
            line[i] = static_cast<int>(value);
        }
    }

    if (reader.overrun()) return false;

    for (size_t i = 0; i < width; i++) out[i] = static_cast<float>(line[i]) * step_db;
    previous_.swap(line);
    step_db_ = step_db;
    return true;
}

}  // namespace wfc
}  // namespace fernsdr
