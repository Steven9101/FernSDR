// Waterfall rows encoded once for everyone with the same view.
//
// Listeners hear different things but mostly look at the same picture: the
// whole band, at the width their screen asks for. Each row's range coding
// is most of what a listener's waterfall costs, and it does not depend on
// who looks, so the band keeps one SharedWaterfall per view (span, width,
// rate, level step) and runs it once a block, before its listeners; each
// listener sends its payload under a header of its own.
//
// Rows are coded against the rows before them since the last key row, so a
// listener can take the shared stream only from a key row on, and only
// while it takes every row. Until the next key row, and whenever it cannot
// take them all (a lost line, a link that cannot carry the full rate), it
// codes its own rows as before; see Listener::process_block. Only WFC5, the
// codec every current page asks for, is shared.
#pragma once

#include "../codec/waterfall_rc.h"
#include "../dsp/spectrum.h"
#include "settings.h"

#include <cstdint>
#include <vector>

namespace fernsdr {

class Band;

// What makes two listeners' waterfall rows the same bytes.
struct WaterfallKey {
    double low_hz = 0.0;
    double high_hz = 0.0;
    int width = 0;
    double lines_per_second = 0.0;
    int step_db = 1;
    bool native_grid = false;

    static WaterfallKey for_viewport(const ViewportSettings& viewport);
    bool operator==(const WaterfallKey& other) const {
        return low_hz == other.low_hz && high_hz == other.high_hz && width == other.width &&
               lines_per_second == other.lines_per_second && step_db == other.step_db &&
               native_grid == other.native_grid;
    }
    bool operator!=(const WaterfallKey& other) const { return !(*this == other); }
};

// One row of a view from a line of the band's spectrum, into `line`: the
// band's native bins when the view oversamples them (and `native_grid`
// allows), otherwise rendered to the view's width. Returns whether the row is
// native, with its edges in `low_hz` and `high_hz`.
bool render_waterfall_row(const SpectrumPyramid& spectrum, const ViewportSettings& viewport, std::vector<float>& line,
                          double& low_hz, double& high_hz);

class SharedWaterfall {
public:
    SharedWaterfall(const Band& band, const WaterfallKey& key);

    const WaterfallKey& key() const { return key_; }

    // Called by the band once a block, after the spectrum and before any
    // listener runs; `line` and `paired` are null on blocks without a new
    // line. Listeners read the row it made until the next call.
    void process(double block_seconds, const SpectrumPyramid* line, const SpectrumPyramid* paired);

    // Whether this block made a row, and that row.
    bool has_row() const { return has_row_; }
    bool row_is_key() const { return row_is_key_; }
    const std::vector<uint8_t>& payload() const { return *payload_; }
    double row_low_hz() const { return row_low_hz_; }
    double row_high_hz() const { return row_high_hz_; }
    uint16_t row_width() const { return row_width_; }
    bool row_native() const { return row_native_; }

private:
    const Band& band_;
    WaterfallKey key_;
    ViewportSettings viewport_;
    wfc::RangedLineEncoder encoder_;
    std::vector<float> line_;
    const std::vector<uint8_t>* payload_ = nullptr;
    // A new view's first row comes with the next line, as a listener's does.
    double credit_ = 1.0;
    double since_key_seconds_ = 0.0;
    bool force_key_ = true;
    bool has_row_ = false;
    bool row_is_key_ = false;
    bool row_native_ = false;
    double row_low_hz_ = 0.0;
    double row_high_hz_ = 0.0;
    uint16_t row_width_ = 0;
};

}  // namespace fernsdr
