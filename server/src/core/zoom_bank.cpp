#include "zoom_bank.h"

#include <algorithm>
#include <cmath>

#include "dsp_workers.h"

namespace fernsdr {

namespace {

constexpr double kPi = 3.14159265358979323846;

long floor_div(long a, long b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Fewer than this many of a kind cost less to run here than to hand over.
constexpr size_t kWorthHandingOver = 4;

}  // namespace

ZoomBank::ZoomBank(const Channelizer& parent, double origin_hz, double rf_scale, double band_bin_hz,
                   double lines_per_second, int averages, float smoothing)
    : parent_(parent),
      origin_hz_(origin_hz),
      rf_scale_(rf_scale),
      band_bin_hz_(band_bin_hz),
      lines_per_second_(lines_per_second),
      averages_(averages),
      smoothing_(smoothing) {}

std::shared_ptr<ZoomSpectrum> ZoomBank::share(double low_hz, double high_hz, int width) {
    if (!zoom_wanted(low_hz, high_hz, width, band_bin_hz_)) return nullptr;
    ZoomKey key;
    if (!ZoomSpectrum::plan(parent_, origin_hz_, rf_scale_, low_hz, high_hz, width, key)) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& weak : zooms_) {
        if (auto running = weak.lock(); running && running->key() == key) return running;
    }
    std::shared_ptr<ZoomSpectrum> made;
    if (key.reads_bins()) {
        if (!tile_gain_known_) {
            // A full-scale complex tone sums to the effective window's sum,
            // that of sin^3; a real band's bins are the analytic signal's,
            // which carries a real tone at twice what the band's line reads.
            const size_t size = parent_.fft_size();
            double sum = 0.0;
            for (size_t i = 0; i < size; i++) sum += std::pow(std::sin(kPi * (i + 0.5) / size), 3.0);
            tile_gain_db_ = static_cast<float>(20.0 * std::log10(sum) +
                                               (parent_.kind() == SignalKind::Real ? 20.0 * std::log10(2.0) : 0.0));
            tile_gain_known_ = true;
        }
        const long tile_bins = static_cast<long>(BinTile::kBins);
        const long first = floor_div(key.first_bin, tile_bins);
        const long last = floor_div(key.first_bin + static_cast<long>(key.bins) - 1, tile_bins);
        std::vector<std::shared_ptr<BinTile>> tiles;
        for (long index = first; index <= last; index++) {
            std::shared_ptr<BinTile> tile = tiles_[index].lock();
            if (!tile) {
                tile = std::make_shared<BinTile>(parent_.fft_size(), index * tile_bins, tile_gain_db_, smoothing_);
                tiles_[index] = tile;
            }
            tiles.push_back(std::move(tile));
        }
        made = std::make_shared<ZoomSpectrum>(parent_, origin_hz_, rf_scale_, key, std::move(tiles));
    } else {
        made = std::make_shared<ZoomSpectrum>(parent_, origin_hz_, rf_scale_, key, lines_per_second_, averages_,
                                              smoothing_);
    }
    zooms_.push_back(made);
    return made;
}

void ZoomBank::process(const ChannelBlock& block, DspWorkers* workers) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        zoom_snapshot_.clear();
        for (auto it = zooms_.begin(); it != zooms_.end();) {
            if (auto running = it->lock()) {
                zoom_snapshot_.push_back(std::move(running));
                ++it;
            } else {
                it = zooms_.erase(it);
            }
        }
        tile_snapshot_.clear();
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            if (auto running = it->second.lock()) {
                tile_snapshot_.push_back(std::move(running));
                ++it;
            } else {
                it = tiles_.erase(it);
            }
        }
    }
    block_ = block;
    credit_ += block.block_seconds() * lines_per_second_;
    line_ = credit_ >= 1.0;
    if (line_) credit_ = std::min(credit_ - 1.0, 1.0);
    channel_snapshot_.clear();
    for (const auto& zoom : zoom_snapshot_) {
        if (!zoom->reads_bins()) channel_snapshot_.push_back(zoom);
    }

    // Tiles and channels each read the same finished block and write only
    // their own state, so they go to the workers as one batch: one wake of
    // the workers a block rather than one for each kind.
    const auto work = [](void* context, size_t index) {
        auto& bank = *static_cast<ZoomBank*>(context);
        if (index < bank.tile_snapshot_.size()) {
            BinTile& tile = *bank.tile_snapshot_[index];
            tile.accumulate(bank.block_);
            if (bank.line_) tile.finish_line();
        } else {
            bank.channel_snapshot_[index - bank.tile_snapshot_.size()]->process(bank.block_);
        }
    };
    const size_t count = tile_snapshot_.size() + channel_snapshot_.size();
    if (workers && workers->has_workers() && count >= kWorthHandingOver) workers->run(count, work, this);
    else for (size_t i = 0; i < count; i++) work(this, i);

    // Views of tiles copy the tiles' lines once all of them are made.
    for (const auto& zoom : zoom_snapshot_) {
        if (zoom->reads_bins()) zoom->assemble(line_);
    }
    zoom_snapshot_.clear();
    channel_snapshot_.clear();
    tile_snapshot_.clear();
}

size_t ZoomBank::zoom_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(std::count_if(zooms_.begin(), zooms_.end(),
                                             [](const auto& weak) { return !weak.expired(); }));
}

size_t ZoomBank::tile_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(std::count_if(tiles_.begin(), tiles_.end(),
                                             [](const auto& entry) { return !entry.second.expired(); }));
}

}  // namespace fernsdr
