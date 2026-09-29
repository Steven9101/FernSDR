// Waterfall line compressor.
//
// A spectrum line is a row of dB values.  Successive lines are strongly
// correlated (the noise floor barely moves), so the temporal delta is small
// and Rice-codes down to roughly 2-3 bits per bin - about 20-30 kbit/s for a
// 1024-bin display at 10 lines/second, which leaves room inside the 100
// kbit/s per-user budget for the audio.
//
// Two prediction modes:
//   temporal - residual against the same bin of the previous line (default)
//   intra    - residual against the previous bin of the same line, used for
//              the first line and whenever the viewport changes, so a client
//              can join or retune without waiting for a keyframe.
//
// Mirrored by web/src/dsp/waterfall.ts.
#pragma once
#include <cstdint>
#include <vector>

#include "../util/bitio.h"

namespace fernsdr {
namespace wfc {

// The default uses 1 dB steps. A negotiated 2 dB step reduces the random
// residual from FFT-bin noise without removing frequency bins or averaging
// extra rows. It can make slow fades more visibly stepped, so clients retain
// the finer setting. Quantize the source directly, never an already-rounded
// row: two successive roundings would exceed the promised half-step error.
constexpr float kDbStep = 1.0f;

/*
 * The representable range, in dB, and the same range in quantiser units.
 *
 * These were written as bare unit counts with the dB value in a comment, which
 * silently tied them to the step: halving `kDbStep` halved the range in dB, so
 * a 0.5 dB step clipped everything below -100 dB. Measured, that turned a
 * finer quantiser into a worse one - rms error 1.03 dB against 0.29, and a
 * worst case of 8.69 dB against 0.50 - which is not something a quantiser can
 * do and was entirely the clipping.
 */
constexpr float kMinLevelDb = -200.0f;
constexpr float kMaxLevelDb = 100.0f;
constexpr int kMinLevelQ = static_cast<int>(kMinLevelDb / kDbStep);
constexpr int kMaxLevelQ = static_cast<int>(kMaxLevelDb / kDbStep);
// Starting point for the first bin of an intra line.
constexpr int kIntraSeedQ = static_cast<int>(-100.0f / kDbStep);

constexpr uint32_t kModeTemporal = 0;
constexpr uint32_t kModeIntra = 1;
constexpr uint32_t kModeGradient = 2;
constexpr uint32_t kModeLinear = 3;

// Bits used for the mode flag and the Rice parameter in the line header.
constexpr uint32_t kModeBits = 1;
constexpr uint32_t kRiceParamBits = 4;
constexpr uint32_t kMaxRiceParam = (1u << kRiceParamBits) - 1;

inline int quantise_db(float db, int step_db = 1) {
    const int low = kMinLevelQ / step_db, high = kMaxLevelQ / step_db;
    if (!(db > kMinLevelDb)) return low;
    if (db >= kMaxLevelDb) return high;
    int q = static_cast<int>(db < 0.0f ? db / step_db - 0.5f : db / step_db + 0.5f);
    if (q < low) q = low;
    if (q > high) q = high;
    return q;
}

class LineEncoder {
public:
    // Forgets the previous line, so the next encode() emits an intra line.
    void reset();

    // Encodes one line of dB values.  The returned buffer is valid until the
    // next call.  A width change or `force_intra` selects intra mode.
    const std::vector<uint8_t>& encode(const float* db, size_t width, bool force_intra = false,
                                      bool allow_zero_runs = false, bool allow_adaptive = false, int step_db = 1);
    bool used_zero_runs() const { return zero_runs_; }
    bool used_adaptive() const { return adaptive_; }

    size_t last_bits() const { return last_bits_; }

private:
    const std::vector<uint8_t>& encode_adaptive(size_t width, bool force_intra, bool allow_zero_runs);
    std::vector<int> previous_;
    std::vector<int> current_;
    std::vector<int> spatial_;
    std::vector<int> temporal_;
    BitWriter writer_;
    std::vector<uint8_t> frame_;
    size_t last_bits_ = 0;
    bool zero_runs_ = false;
    bool adaptive_ = false;
    int step_db_ = 1;
};

class LineDecoder {
public:
    void reset();

    // Decodes one line into `out`, which must hold `width` floats.  Returns
    // false if the frame is malformed or arrives in temporal mode without a
    // usable previous line.
    bool decode(const uint8_t* data, size_t size, size_t width, float* out, bool zero_runs = false,
                bool adaptive = false, int step_db = 1);

private:
    std::vector<int> previous_;
    int step_db_ = 1;
};

// Chooses the Rice parameter that minimises the coded length of a set of
// residuals.  Exposed for testing.
uint32_t choose_rice_param(const int* residuals, size_t count);

}  // namespace wfc
}  // namespace fernsdr
