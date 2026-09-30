#include "shared_waterfall.h"

#include <algorithm>

#include "../codec/waterfall_codec.h"
#include "band.h"

namespace fernsdr {

WaterfallKey WaterfallKey::for_viewport(const ViewportSettings& viewport) {
    WaterfallKey key;
    key.low_hz = viewport.low_hz;
    key.high_hz = viewport.high_hz;
    key.width = viewport.width;
    key.lines_per_second = viewport.lines_per_second;
    key.step_db = viewport.step_db;
    key.native_grid = viewport.native_grid;
    return key;
}

bool render_waterfall_row(const SpectrumPyramid& spectrum, const ViewportSettings& viewport, std::vector<float>& line,
                          double& low_hz, double& high_hz) {
    low_hz = viewport.low_hz;
    high_hz = viewport.high_hz;
    bool native = viewport.native_grid &&
                  spectrum.native_viewport(low_hz, high_hz, static_cast<size_t>(viewport.width), line, low_hz, high_hz);
    if (native && std::any_of(line.begin(), line.end(), [](float level) {
            return !(level >= wfc::kMinLevelDb && level <= wfc::kMaxLevelDb);
        })) {
        // Saturating native endpoints before interpolation changes the
        // visible slope. Render first for an out-of-range source so codec
        // clipping happens at the same place as usual.
        native = false;
        low_hz = viewport.low_hz;
        high_hz = viewport.high_hz;
    }
    if (!native) {
        line.resize(static_cast<size_t>(viewport.width));
        spectrum.render(low_hz, high_hz, line.data(), line.size());
    }
    return native;
}

SharedWaterfall::SharedWaterfall(const Band& band, const WaterfallKey& key) : band_(band), key_(key) {
    viewport_.enabled = true;
    viewport_.range_coded = true;
    viewport_.adaptive_codec = true;
    viewport_.zero_runs = true;
    viewport_.native_grid = key.native_grid;
    viewport_.low_hz = key.low_hz;
    viewport_.high_hz = key.high_hz;
    viewport_.width = key.width;
    viewport_.lines_per_second = key.lines_per_second;
    viewport_.step_db = key.step_db;
    zoom_ = band.share_zoom(viewport_);
}

void SharedWaterfall::process(double block_seconds, const SpectrumPyramid* line, const SpectrumPyramid* paired) {
    has_row_ = false;
    // As a listener's own rows: the zoom's lines once it has one.
    if (zoom_ && zoom_->ready()) {
        const bool fresh = zoom_->has_line();
        line = fresh ? &zoom_->line() : nullptr;
        paired = fresh ? &zoom_->paired() : nullptr;
    }
    // The same pacing as a listener's own rows: credit on every block, a row
    // on a block with a line once there is enough, and at half the band's
    // line rate or less, the mean of two lines.
    credit_ += block_seconds * key_.lines_per_second;
    since_key_seconds_ += block_seconds;
    if (paired && key_.lines_per_second <= 0.55 * band_.spectrum_lines_per_second()) line = paired;
    if (!line || credit_ < 1.0) return;
    credit_ = std::min(credit_ - 1.0, 1.0);
    row_native_ = render_waterfall_row(*line, viewport_, line_, row_low_hz_, row_high_hz_);
    // A key row every two seconds, as each listener's own stream has: it is
    // where listeners join, and what a page that lost a row waits for.
    const bool key = force_key_ || since_key_seconds_ >= 2.0;
    payload_ = &encoder_.encode(line_.data(), line_.size(), key, key_.step_db);
    row_is_key_ = encoder_.last_was_key();
    if (row_is_key_) since_key_seconds_ = 0.0;
    force_key_ = false;
    row_width_ = static_cast<uint16_t>(line_.size());
    has_row_ = !payload_->empty();
}

}  // namespace fernsdr
