#include "nac.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fernsdr {
namespace nac {

Decoder::Decoder(int sample_rate)
    : sample_rate_(sample_rate),
      mdct_(kFrameHop),
      window_(make_sine_window(2 * kFrameHop)),
      coeffs_(kNumCoeffs, 0.0f),
      prev_coeffs_(kNumCoeffs, 0.0f),
      time_(2 * kFrameHop, 0.0f),
      overlap_(kFrameHop, 0.0f) {}

void Decoder::reset() {
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    std::fill(prev_coeffs_.begin(), prev_coeffs_.end(), 0.0f);
    conceal_gain_ = 1.0f;
}

bool Decoder::decode(const uint8_t* data, size_t size, float* out, bool compact) {
    if (size == 0) {
        conceal(out);
        return false;
    }

    BitReader reader(data, size);
    const int quality_index = static_cast<int>(reader.get_bits(kQualityBits));

    uint8_t active[kNumBands]{};
    const uint32_t mask_mode = compact ? reader.get_bits(2) : 0;
    if (mask_mode == 0) {
        for (int b = 0; b < kNumBands; b++) active[b] = static_cast<uint8_t>(reader.get_bit());
    } else if (mask_mode == 1) {
        std::fill(active, active + kNumBands, 1);
    } else {
        const uint32_t first = mask_mode == 3 ? reader.get_bits(5) : 0;
        const uint32_t count = reader.get_bits(5);
        if (first >= kNumBands || count > kNumBands - first || (mask_mode == 3 && count == 0)) {
            conceal(out);
            return false;
        }
        std::fill(active + first, active + first + count, 1);
    }

    int exponents[kNumBands] = {0};
    int prev_exponent = kExponentReference;
    bool first = true;
    uint32_t scale_mode = 0;
    for (int b = 0; b < kNumBands; b++) {
        if (!active[b]) continue;
        int64_t exponent;
        if (compact && first) {
            exponent = static_cast<int>(reader.get_bits(9)) - 200;
            scale_mode = reader.get_bits(3);
        } else {
            exponent = static_cast<int64_t>(prev_exponent) +
                (scale_mode ? reader.get_signed_rice(scale_mode - 1) : reader.get_signed_exp_golomb());
        }
        if (scale_mode > 5 || exponent < -200 || exponent > 200 || reader.overrun()) {
            conceal(out);
            return false;
        }
        first = false;
        prev_exponent = static_cast<int>(exponent);
        exponents[b] = prev_exponent;
    }

    const uint32_t k = rice_k_for_quality(quality_index);
    std::fill(coeffs_.begin(), coeffs_.end(), 0.0f);
    for (int b = 0; b < kNumBands; b++) {
        if (!active[b]) continue;
        const int start = kBandStarts[b];
        const int width = kBandWidths[b];
        const float step = std::exp2(static_cast<float>(exponents[b] - quality_index) * 0.25f);
        for (int i = 0; i < width; i++) {
            coeffs_[start + i] = static_cast<float>(reader.get_signed_rice(k)) * step;
        }
    }

    if (reader.overrun()) {
        // Truncated or corrupt: conceal rather than emit whatever partial
        // spectrum we managed to parse.
        conceal(out);
        return false;
    }

    std::memcpy(prev_coeffs_.data(), coeffs_.data(), kNumCoeffs * sizeof(float));
    conceal_gain_ = 1.0f;
    synthesise(out);
    return true;
}

bool Decoder::decode(const uint8_t* data, size_t size, float* out, Layout layout) {
    if (layout != Layout::PerBand) return decode(data, size, out, layout == Layout::Compact);
    bool ok = false;
    if (size == 0 || (data[0] >> 6) != 0) {
        conceal(out);
        return false;
    }
    return decode_packet(data, size, out, 1, ok) == 1 && ok;
}

int Decoder::decode_packet(const uint8_t* data, size_t size, float* out, int capacity, bool& ok) {
    ok = false;
    if (size == 0) return 0;
    BitReader reader(data, size);
    const int frames = static_cast<int>(reader.get_bits(2)) + 1;
    if (frames > capacity) return 0;
    PacketReferences references;
    int produced = 0;
    for (; produced < frames; produced++) {
        if (!decode_per_band(reader, produced > 0, references)) break;
        std::memcpy(prev_coeffs_.data(), coeffs_.data(), kNumCoeffs * sizeof(float));
        conceal_gain_ = 1.0f;
        synthesise(out + static_cast<size_t>(produced) * kFrameHop);
    }
    ok = produced == frames;
    // A corrupt frame takes the rest of its packet with it: every later frame
    // predicts from it. Conceal them so the sample clock keeps its count.
    for (int f = produced; f < frames; f++) conceal(out + static_cast<size_t>(f) * kFrameHop);
    return frames;
}

// NAC3 frame: activity mask, one step index and one Rice parameter per active
// band, then the coefficients. A later frame of a packet may repeat the
// previous frame's mask with one bit and codes steps and Rice parameters as
// residuals against the previous frame's. Everything is validated before the
// frame is allowed to touch the overlap.
bool Decoder::decode_per_band(BitReader& reader, bool predicted, PacketReferences& references) {
    uint8_t active[kNumBands]{};
    const bool repeat_mask = predicted && reader.get_bit() == 1;
    if (repeat_mask) {
        std::copy(std::begin(references.active), std::end(references.active), active);
    } else {
        const uint32_t mask_mode = reader.get_bits(2);
        if (mask_mode == 0) {
            for (int b = 0; b < kNumBands; b++) active[b] = static_cast<uint8_t>(reader.get_bit());
        } else if (mask_mode == 1) {
            std::fill(active, active + kNumBands, 1);
        } else {
            const uint32_t first = mask_mode == 3 ? reader.get_bits(5) : 0;
            const uint32_t count = reader.get_bits(5);
            if (first >= kNumBands || count > kNumBands - first || (mask_mode == 3 && count == 0)) return false;
            std::fill(active + first, active + first + count, 1);
        }
    }

    int steps[kNumBands]{};
    int rice[kNumBands]{};
    int active_count = 0;
    for (int b = 0; b < kNumBands; b++) active_count += active[b];
    if (active_count > 0) {
        uint32_t selector = predicted ? reader.get_bits(3) : 0;
        const auto residual = [&](uint32_t mode) -> int64_t {
            return mode ? reader.get_signed_rice(mode - 1) : reader.get_signed_exp_golomb();
        };
        bool first = true;
        int64_t step = 0;
        for (int b = 0; b < kNumBands; b++) {
            if (!active[b]) continue;
            if (predicted) {
                if (selector > 5) return false;
                step = static_cast<int64_t>(references.step[b]) + residual(selector);
            } else if (first) {
                step = static_cast<int64_t>(reader.get_bits(9)) + kMinStep;
                selector = reader.get_bits(3);
                if (selector > 5) return false;
            } else {
                step += residual(selector);
            }
            first = false;
            if (step < kMinStep || step > kMaxStep || reader.overrun()) return false;
            steps[b] = static_cast<int>(step);
        }
        first = true;
        int64_t parameter = 0;
        for (int b = 0; b < kNumBands; b++) {
            if (!active[b]) continue;
            if (predicted) parameter = static_cast<int64_t>(references.rice[b]) + reader.get_signed_rice(0);
            else parameter = first ? static_cast<int64_t>(reader.get_bits(4)) : parameter + reader.get_signed_rice(0);
            first = false;
            if (parameter < 0 || parameter > kMaxRiceParameter || reader.overrun()) return false;
            rice[b] = static_cast<int>(parameter);
        }
    }

    std::fill(coeffs_.begin(), coeffs_.end(), 0.0f);
    for (int b = 0; b < kNumBands; b++) {
        if (!active[b]) continue;
        const float step_size = std::exp2(0.25f * static_cast<float>(steps[b]));
        const uint32_t k = static_cast<uint32_t>(rice[b]);
        for (int i = kBandStarts[b]; i < kBandStarts[b + 1]; i++) {
            const float value = static_cast<float>(reader.get_signed_rice(k)) * step_size;
            // Receiver audio is normalised near unity. A coefficient this far
            // outside it is a corrupt or hostile frame, not a loud signal, and
            // would otherwise reach the speaker as a full-scale burst.
            if (!(std::fabs(value) <= 1.0e7f)) return false;
            coeffs_[i] = value;
        }
    }
    if (reader.overrun()) return false;

    for (int b = 0; b < kNumBands; b++) {
        if (active[b]) {
            references.step[b] = steps[b];
            references.rice[b] = rice[b];
        }
        references.active[b] = active[b];
    }
    if (!predicted) fill_references(active, references.step, references.rice);
    return true;
}

void Decoder::conceal(float* out) {
    // Repeat the last spectrum, fading out.  No time stretching and no sample
    // insertion: the output clock stays exactly one hop per frame so decoders
    // riding on the audio do not lose symbol timing across a dropout.
    conceal_gain_ *= 0.5f;
    for (size_t i = 0; i < kNumCoeffs; i++) coeffs_[i] = prev_coeffs_[i] * conceal_gain_;
    synthesise(out);
}

void Decoder::synthesise(float* out) {
    mdct_.inverse(coeffs_.data(), time_.data());
    for (size_t i = 0; i < kFrameHop; i++) {
        out[i] = overlap_[i] + time_[i] * window_[i];
        overlap_[i] = time_[kFrameHop + i] * window_[kFrameHop + i];
    }
}

}  // namespace nac
}  // namespace fernsdr
