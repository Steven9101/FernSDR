// A waterfall for one view that the band's own spectrum is too coarse for.
//
// The band's SpectrumAnalyzer draws the whole band once for everyone, so
// its bins are as wide as a shared transform can afford: about 30 Hz on a
// 2 Msps band, but a kilohertz on a 64.8 Msps one, where an SSB signal on
// 40 m would be three bins wide however far a listener zooms in. Making the
// shared transform finer does not scale: a 1M-point one on that band took
// most of a core and delayed everyone's audio.
//
// A view that is finer than the band's bins gets this instead: a channel
// cut from the channelizer's spectrum, which the band computes anyway and
// which every listener's audio already comes from, just wide enough for the
// view, and a transform of that channel sized so that one bin is no wider
// than one pixel. What it costs follows the width of the view, not of the
// band: a 300 kHz view on the 64.8 Msps band is an 8192-point inverse
// transform a block and a few small windowed ones a line.
//
// A zoom is keyed by its channel, not by the exact view: the channel's
// centre sits on a grid of a sixteenth of its width, and it is wide enough
// that the view stays in its flat part wherever between two grid points
// the view's centre falls. Panning, and zooming in steps that keep the
// channel's and transform's sizes, go on reading the zoom already running
// instead of starting a new one, and listeners looking at nearly the same
// place share one (Band::share_zoom).
//
// The lines are in dBFS like the band's, so a carrier reads the same level
// in both; the noise in each bin is lower by as much as the bins are
// narrower, as on any receiver whose resolution changes with the zoom.
#pragma once
#include <cstddef>
#include <vector>

#include "channelizer.h"
#include "spectrum.h"

namespace fernsdr {

struct ZoomKey {
    double centre_hz = 0.0;  // the channel's centre, in the parent's samples from its origin
    size_t channel_size = 0;
    size_t transform_size = 0;
    bool operator==(const ZoomKey& other) const {
        return centre_hz == other.centre_hz && channel_size == other.channel_size &&
               transform_size == other.transform_size;
    }
    bool operator!=(const ZoomKey& other) const { return !(*this == other); }
};

// Whether a view of `width` pixels over [low_hz, high_hz] needs a zoom
// spectrum on a band whose own bins are `band_bin_hz` wide (all in RF Hz).
// A little oversampling is left to the band's line: interpolating a bin
// over one and a quarter pixels is not visible, and it spares a channel.
bool zoom_wanted(double low_hz, double high_hz, int width, double band_bin_hz);

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

    // The line rate, averages and smoothing are the band's.
    ZoomSpectrum(const Channelizer& parent, double origin_hz, double rf_scale, const ZoomKey& key,
                 double lines_per_second, int averages, float smoothing);

    const ZoomKey& key() const { return key_; }

    // Takes one block of the parent's spectrum. Called once per block, by
    // the band, before anything reads line() or paired().
    void process(const ChannelBlock& block);

    // Whether a line exists yet: until then, the band's line stands in.
    bool ready() const { return ready_; }
    // Whether the last process() made a line.
    bool has_line() const { return has_line_; }
    const SpectrumPyramid& line() const { return line_; }
    // Each line averaged with the one before, for views shown at half the
    // line rate or less, as the band keeps for its own.
    const SpectrumPyramid& paired() const { return paired_; }
    // Whether [low_hz, high_hz] (RF) lies in the channel's flat part, so
    // that this zoom can draw it: a listener keeps drawing from the zoom it
    // had while the one for its new view is still gathering its first line.
    bool covers(double low_hz, double high_hz) const { return low_hz >= flat_low_hz_ && high_hz <= flat_high_hz_; }

    // The channel's rate and the transform's bin, in the parent's samples.
    double channel_rate() const { return channel_.output_rate(); }
    double bin_hz() const { return analyzer_.bin_hz(); }

private:
    ZoomKey key_;
    Channel channel_;
    SpectrumAnalyzer analyzer_;
    // RF edges of the transform's cells, as SpectrumPyramid takes them.
    double low_hz_ = 0.0;
    double high_hz_ = 0.0;
    double flat_low_hz_ = 0.0;
    double flat_high_hz_ = 0.0;
    // The bins inside what the parent sampled; see process().
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
