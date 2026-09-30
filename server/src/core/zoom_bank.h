// The zoomed views of one band, and the tiles of bins they read (see
// ZoomSpectrum). Listeners and shared rows ask for a view from their own
// threads; the band runs everything once a block, before any of them reads.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../dsp/zoom_spectrum.h"

namespace fernsdr {

class DspWorkers;

class ZoomBank {
public:
    // `band_bin_hz` is the band's own line's bin in RF Hz, the rest as for
    // ZoomSpectrum; the line rate, averages and smoothing are the band's.
    ZoomBank(const Channelizer& parent, double origin_hz, double rf_scale, double band_bin_hz,
             double lines_per_second, int averages, float smoothing);

    // The zoom for a view, shared with everyone on the same one, or null
    // where the band's own line draws it. Released with its last holder.
    std::shared_ptr<ZoomSpectrum> share(double low_hz, double high_hz, int width);

    // Runs every zoom and tile on one block of the parent's spectrum, on the
    // workers when there are enough of them to be worth handing over.
    void process(const ChannelBlock& block, DspWorkers* workers);

    size_t zoom_count() const;
    size_t tile_count() const;

private:
    const Channelizer& parent_;
    double origin_hz_;
    double rf_scale_;
    double band_bin_hz_;
    double lines_per_second_;
    int averages_;
    float smoothing_;
    // What a full-scale tone sums to through the tiles' kernel, in dB; made
    // with the first tile.
    float tile_gain_db_ = 0.0f;
    bool tile_gain_known_ = false;

    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<ZoomSpectrum>> zooms_;
    std::map<long, std::weak_ptr<BinTile>> tiles_;

    // Band thread only.
    std::vector<std::shared_ptr<ZoomSpectrum>> zoom_snapshot_;
    std::vector<std::shared_ptr<ZoomSpectrum>> channel_snapshot_;
    std::vector<std::shared_ptr<BinTile>> tile_snapshot_;
    ChannelBlock block_;
    // Tiles make their lines together, at the band's line rate.
    double credit_ = 0.0;
};

}  // namespace fernsdr
