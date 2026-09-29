// Waterfall / spectrum producer.
//
// Deliberately separate from the Channelizer's transform. Fast convolution
// needs a rectangular window, but a rectangular window has -13 dB sidelobes,
// which on a waterfall smears every strong carrier into a visible skirt tens
// of kilohertz wide. A second, windowed FFT costs a few MFLOP/s per band -
// shared across every listener - and is worth it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "channelizer.h"
#include "fft.h"
#include "fft_split.h"

namespace fernsdr {

DspVector<float> make_blackman_harris_window(size_t length);

class SpectrumAnalyzer {
public:
    // Produces `lines_per_second` spectrum lines, each the power average of
    // `averages` transforms. Averaging both steadies the display and reduces
    // the line-to-line variance the waterfall codec has to encode.
    SpectrumAnalyzer(double sample_rate, size_t fft_size, double lines_per_second, int averages,
                     float smoothing = 0.5f, SignalKind kind = SignalKind::Iq);

    // Bins in a published line: the whole transform for IQ, half of it for a
    // real input, which only occupies 0 .. rate/2.
    size_t bins() const { return kind_ == SignalKind::Iq ? fft_size_ : fft_size_ / 2; }
    double sample_rate() const { return sample_rate_; }
    SignalKind kind() const { return kind_; }
    double bin_hz() const { return sample_rate_ / static_cast<double>(fft_size_); }
    // Transforms averaged into each line: what was asked for, less any that
    // would overlap their neighbour by more than three quarters.
    int averages() const { return averages_; }

    // Feeds samples in. Use the one matching kind().
    void push(const cfloat* input, size_t count);
    void push_real(const float* input, size_t count);

    // Returns true and fills `out` (bins() entries, dBFS, index 0 = lowest
    // frequency) when a line is ready. Call until it returns false.
    bool take_line(std::vector<float>& out);

    void set_smoothing(float smoothing);

    // Transforms only the averages of one line in `stride`, for a band nobody
    // is watching: its lines still feed the noise floor and the admin panel,
    // which need two a second rather than twenty-five, and the transforms of
    // the rest were most of an idle band's CPU. Back to 1, every hop is
    // transformed again at once, so the first listener's first line is
    // `averages()` hops away rather than the rest of an idle stretch; a line
    // under way when the stride goes up is finished first, so no line mixes
    // samples from either side of a gap.
    void set_stride(int stride) { stride_ = stride < 1 ? 1 : stride; }
    int stride() const { return stride_; }
    // Seconds of input between the last two lines, which the stride stretches.
    double last_line_seconds() const {
        return static_cast<double>(last_line_hops_) * static_cast<double>(hop_) / sample_rate_;
    }
    // Transforms run so far, for tests and measurement.
    uint64_t transforms() const { return transforms_; }

private:
    void finish_span(size_t count);
    // How many of the next `count` samples fall where no transform reads them.
    size_t unused_ahead(size_t count) const;
    void transform();

    double sample_rate_;
    size_t fft_size_;
    size_t hop_;
    int averages_;
    SignalKind kind_;
    FftSplit fft_;
    RealFft real_fft_;
    DspVector<float> window_;
    float window_gain_db_ = 0.0f;

    DspVector<float> history_re_;  // a real input's fft_size_ most recent samples, as a ring
    // An IQ input's most recent samples as (re, im) pairs, the ring written
    // twice, at write_pos_ and fft_size_ further on: any window of fft_size_
    // samples then lies in one run, which the windowed transform reads as it
    // is, with no unwrapping, splitting or windowing pass of its own.
    DspVector<float> ring_;
    size_t write_pos_ = 0;
    size_t since_hop_ = 0;
    size_t primed_ = 0;

    DspVector<float> work_re_;
    DspVector<float> work_im_;
    DspVector<float> accumulator_;
    int accumulated_ = 0;

    int stride_ = 1;
    uint64_t hops_ = 0;
    uint64_t hops_since_line_ = 0;
    uint64_t last_line_hops_ = 0;
    uint64_t transforms_ = 0;

    std::vector<float> ready_;
    std::vector<float> smoothed_;
    // The accumulated powers as decibels, in the transform's own order.
    std::vector<float> db_line_;
    float smoothing_ = 0.5f;
    bool smoothed_primed_ = false;
    bool has_line_ = false;
};

// Maps a full-resolution spectrum line onto a display viewport.
//
// `src` covers [src_low_hz, src_high_hz] in `src_bins` bins, index 0 lowest.
// The viewport [view_low_hz, view_high_hz] is rendered into `width` output
// bins. When zoomed out, each output bin takes the MAXIMUM of the source bins
// behind it: averaging would sink a narrow carrier into the noise and make it
// invisible at anything but full zoom, which is precisely when a user is
// scanning for one. When zoomed in past 1:1, values are interpolated.
void render_viewport(const float* src, size_t src_bins, double src_low_hz, double src_high_hz,
                     double view_low_hz, double view_high_hz, float* dst, size_t width);

// A peak-decimated mipmap of one spectrum line, built once per band and shared
// by every listener on it.
//
// Without this, a listener looking at the whole of a 32 MHz front end costs a
// scan of all 2^21 bins for each of the ~1500 pixels' worth of line it is
// sent - and it costs that again for the next listener, and the next.  With
// it, the whole-band scan happens once (level 0 -> level 1 -> ...), and each
// listener then reads roughly two source bins per output pixel from whichever
// level is just fine enough for its zoom.  Per-listener cost stops depending
// on the front end's width, which is what makes 200 users on a wideband
// receiver arithmetic rather than aspiration.
//
// Decimation takes the maximum of each pair, matching what render_viewport
// does when zoomed out, so a coarse level answers a zoomed-out query with the
// same value the full-resolution line would have given.  Averaging here would
// sink narrow carriers into the noise floor exactly when someone is hunting
// for them.
class SpectrumPyramid {
public:
    // `bins` values covering [low_hz, high_hz], index 0 lowest.  The line is
    // copied; the caller may reuse its buffer immediately.
    void build(const float* line, size_t bins, double low_hz, double high_hz);

    bool empty() const { return levels_.empty(); }
    size_t levels() const { return levels_.size(); }
    double low_hz() const { return low_hz_; }
    double high_hz() const { return high_hz_; }

    // Same contract as render_viewport, reading from the coarsest level that
    // still resolves one output pixel. Safe to call from several threads.
    void render(double view_low_hz, double view_high_hz, float* dst, size_t width) const;

    // When the view oversamples the FFT, send its native bins and interpolation
    // neighbours instead. The returned bounds are bin-cell edges, including
    // those neighbours. Returns false if this would not reduce the row size.
    bool native_viewport(double view_low_hz, double view_high_hz, size_t requested_width,
                         std::vector<float>& out, double& row_low_hz, double& row_high_hz) const;

private:
    void render_uncached(double view_low_hz, double view_high_hz, float* dst, size_t width) const;

    std::vector<std::vector<float>> levels_;
    double low_hz_ = 0.0;
    double high_hz_ = 0.0;
    // Which line this is, unique across every pyramid in the process, so a
    // row rendered from it can be recognised and reused; 0 before any build.
    uint64_t version_ = 0;
};

}  // namespace fernsdr
