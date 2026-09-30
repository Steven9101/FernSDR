// A waterfall for one view that the band's own spectrum is too coarse for.
//
// The band's SpectrumAnalyzer draws the whole band once for everyone, so
// its bins are as wide as a shared transform can afford: about 30 Hz on a
// 2 Msps band, but a kilohertz on a 64.8 Msps one, where an SSB signal on
// 40 m would be three bins wide however far a listener zooms in. Making the
// shared transform finer does not scale: a 1M-point one on that band took
// most of a core and delayed everyone's audio.
//
// A view finer than the band's bins gets one of these instead, drawn from
// the channelizer's transform, which the band computes anyway for every
// listener's audio, in one of two ways:
//
// - While a pixel is still wider than the channelizer's bins (on 64.8 Msps,
//   views from about 115 kHz up), the power of those bins is enough. The
//   channelizer's sine window leaks too far for a waterfall (-23 dB
//   sidelobes, a skirt around every strong carrier), but multiplying a
//   windowed block by sin^2 is, in frequency, a three-point kernel over the
//   bins, which turns the window into sin^3: -39 dB sidelobes falling by 24
//   dB an octave. So each line is the power of that kernel over the view's
//   bins, averaged over the blocks in it: about ten operations a bin. The
//   bins are summed in tiles of 1024 (BinTile), each once for every view
//   that shows it, so ten listeners zoomed about 40 m pay for 40 m once;
//   a view only copies its tiles' lines into its own.
// - Finer than that, time is needed to resolve frequency: a channel cut from
//   the transform, just wide enough for the view, and a Blackman-Harris
//   transform of it with bins no wider than a pixel, down to 2 Hz. Such a
//   view is narrow, and so is its channel.
//
// A channel is keyed by where it lies on a grid, not by the exact view: its
// centre sits on steps of a sixteenth of its width, and it is wide enough
// that the view stays inside it wherever between two steps the view's
// centre falls. Panning, and zooming in steps that keep its sizes, go on
// reading the channel already running instead of starting a new one, and
// listeners looking at nearly the same place share one (ZoomBank). A view
// read from tiles needs no grid: the tiles it pans onto are mostly running.
//
// The lines are in dBFS like the band's, so a carrier reads the same level
// in both; the noise in each bin is lower by as much as the bins are
// narrower, as on any receiver whose resolution changes with the zoom.
#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "channelizer.h"
#include "spectrum.h"

namespace fernsdr {

struct ZoomKey {
    // A view read from the parent's bins: the first (from the origin,
    // negative below it on an IQ band) and how many.
    long first_bin = 0;
    size_t bins = 0;
    // Or a channel of channel_size of the parent's bins centred at
    // centre_hz (in the parent's samples from its origin) and a transform
    // of transform_size points of it.
    double centre_hz = 0.0;
    size_t channel_size = 0;
    size_t transform_size = 0;
    bool reads_bins() const { return transform_size == 0; }
    bool operator==(const ZoomKey& other) const {
        return first_bin == other.first_bin && bins == other.bins && centre_hz == other.centre_hz &&
               channel_size == other.channel_size && transform_size == other.transform_size;
    }
    bool operator!=(const ZoomKey& other) const { return !(*this == other); }
};

// Whether a view of `width` pixels over [low_hz, high_hz] needs a zoom
// spectrum on a band whose own bins are `band_bin_hz` wide (all in RF Hz).
// A little oversampling is left to the band's line: interpolating a bin
// over one and a quarter pixels is not visible, and it spares a zoom.
bool zoom_wanted(double low_hz, double high_hz, int width, double band_bin_hz);

// A run of the parent's bins read through the sin^3 kernel, summed over the
// blocks of each line and turned into a line of dBFS, smoothed as the band's
// line is. Shared by every view it lies under.
class BinTile {
public:
    static constexpr size_t kBins = 1024;

    // `gain_db` makes a full-scale tone read 0 dBFS (see ZoomBank).
    BinTile(size_t parent_size, long first_bin, float gain_db, float smoothing);

    long first_bin() const { return first_bin_; }
    // Adds one block of the parent's spectrum.
    void accumulate(const ChannelBlock& block);
    // Turns the blocks since the last line into a line; nothing if none.
    void finish_line();
    bool has_line() const { return has_line_; }
    const float* line() const { return line_.data(); }

private:
    size_t parent_size_;
    long first_bin_;
    float gain_db_;
    float smoothing_;
    std::vector<float> power_;
    std::vector<float> line_;
    int blocks_ = 0;
    bool has_line_ = false;
};

class ZoomSpectrum {
public:
    // Finest bin a zoom transform makes: 2 Hz is half a second of samples,
    // about as long as a waterfall line can gather before it smears in time
    // what it resolves in frequency. The page's narrowest view, 2 kHz, is
    // then a thousand bins, which it draws at any width.
    static constexpr double kFinestBinHz = 2.0;

    // The zoom for a view of `width` pixels over [low_hz, high_hz] (RF Hz).
    // `origin_hz` and `rf_scale` place RF frequencies in the parent's
    // samples as Band::spectrum_origin_hz() and Band::rf_scale() do for a
    // listener's channel. False when no channel the parent can cut holds the
    // view in its flat part: a view nearly as wide as a small band, whose
    // own line then draws it.
    static bool plan(const Channelizer& parent, double origin_hz, double rf_scale, double low_hz, double high_hz,
                     int width, ZoomKey& key);

    // A channel and its transform; the line rate, averages and smoothing
    // are the band's.
    ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                 double lines_per_second, int averages, float smoothing);
    // A view of the parent's bins, from the tiles under it, in order.
    ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                 std::vector<std::shared_ptr<BinTile>> tiles);

    const ZoomKey& key() const { return key_; }
    bool reads_bins() const { return key_.reads_bins(); }

    // A channel's zoom takes one block of the parent's spectrum; a view of
    // tiles takes its tiles' lines when they have made one (`line`). Called
    // once per block, by ZoomBank, before anything reads line() or paired().
    void process(const ChannelBlock& block);
    void assemble(bool line);

    // Whether a line exists yet: until then, the band's line stands in.
    bool ready() const { return ready_; }
    // Whether the last process() or assemble() made a line.
    bool has_line() const { return has_line_; }
    const SpectrumPyramid& line() const { return line_; }
    // Each line averaged with the one before, for views shown at half the
    // line rate or less, as the band keeps for its own.
    const SpectrumPyramid& paired() const { return paired_; }
    // Whether [low_hz, high_hz] (RF) lies in what this zoom draws well, so
    // that this zoom can draw it: a listener keeps drawing from the zoom it
    // had while the one for its new view is still gathering its first line.
    bool covers(double low_hz, double high_hz) const { return low_hz >= flat_low_hz_ && high_hz <= flat_high_hz_; }

    // The width of a line's bins, in the parent's samples.
    double bin_hz() const { return bin_hz_; }

private:
    // Places the line's `bins` bins, the first centred at `first_hz`, and
    // the part drawn well, [usable_low_hz, usable_high_hz], all in the
    // parent's samples from its origin.
    void place(const Channelizer& parent, double origin_hz, double rf_scale, double first_hz, size_t bins,
               double usable_low_hz, double usable_high_hz);
    void publish();

    ZoomKey key_;
    // A channel and its transform, for a zoom finer than the parent's bins.
    std::unique_ptr<Channel> channel_;
    std::unique_ptr<SpectrumAnalyzer> analyzer_;
    // Or the tiles under a view of the parent's bins.
    std::vector<std::shared_ptr<BinTile>> tiles_;
    double bin_hz_ = 0.0;
    // RF edges of the line's cells, as SpectrumPyramid takes them.
    double low_hz_ = 0.0;
    double high_hz_ = 0.0;
    double flat_low_hz_ = 0.0;
    double flat_high_hz_ = 0.0;
    // The bins inside what the parent sampled; see publish().
    size_t sampled_first_ = 0;
    size_t sampled_end_ = 0;
    std::vector<cfloat> baseband_;
    std::vector<float> spectrum_line_;
    std::vector<float> previous_line_;
    std::vector<float> paired_line_;
    SpectrumPyramid line_;
    SpectrumPyramid paired_;
    bool real_parent_ = false;
    bool ready_ = false;
    bool has_line_ = false;
};

}  // namespace fernsdr
