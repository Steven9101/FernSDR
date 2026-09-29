// WFC5: waterfall rows through a context-modelled binary range coder.
//
// WFC4 Rice-codes each residual, which costs at least one bit a bin however
// predictable the row is. Measured on rows from the benchmark lab (1536 bins,
// 12 rows a second, 2 dB steps) the residuals carry about 1.0 bit a bin of
// information while WFC4 spent 1.51: most residuals are zero, and whether one
// is, and which way it goes, depends on how the row above moved. WFC5 codes
// each residual as a few binary decisions whose probabilities adapt to that
// context, with LZMA's range coder, and spent 1.01 bits a bin on the same rows
// (1.23 against 1.68 at 1 dB steps).
//
// The model adapts along the stream, so every row depends on the rows before
// it since the last key row. Key rows reset the model and do not look at the
// row above; the encoder sends one every kKeyEvery rows and whenever WFC4
// would send an intra row, so a client that lost a row, joined, or retuned
// decodes again within two seconds. docs/CODEC.md has the layout; the decoder
// in web/src/dsp/waterfall.ts mirrors this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fernsdr {
namespace wfc {

// Rows between key rows: two seconds at the default 12 rows a second. Longer
// spans cost less (the model starts from nothing on each key row) but leave a
// listener who lost a row waiting longer.
constexpr int kKeyEvery = 24;

// Payload header byte: bit 0 marks a key row. Other bits are reserved and
// must be zero.
constexpr uint8_t kRangedKey = 1;

// The adaptive probabilities, 11-bit chances of a zero bit.
struct RangedModel {
    uint16_t zero[5][4][4];
    uint16_t sign[5][4][3];
    uint16_t magnitude[5][5][3];
    void reset();
};

class RangedLineEncoder {
public:
    void reset();

    // Encodes one line of dB values; a width or step change, `force_key`, or
    // kKeyEvery rows since the last key row make it a key row. The buffer is
    // valid until the next call.
    const std::vector<uint8_t>& encode(const float* db, size_t width, bool force_key, int step_db);
    bool last_was_key() const { return last_key_; }

private:
    RangedModel model_{};
    std::vector<int> previous_;
    std::vector<int> current_;
    std::vector<uint8_t> frame_;
    int step_db_ = 0;
    int since_key_ = 0;
    bool last_key_ = false;
};

class RangedLineDecoder {
public:
    void reset();

    // Decodes one line into `out` (dB). Returns false, and forgets its state,
    // for a malformed payload or a row that needs a model it does not have.
    bool decode(const uint8_t* data, size_t size, size_t width, float* out, int step_db);

private:
    RangedModel model_{};
    std::vector<int> previous_;
    std::vector<int> current_;
    int step_db_ = 0;
    bool ready_ = false;
};

}  // namespace wfc
}  // namespace fernsdr
