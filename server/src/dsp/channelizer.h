// Shared FFT channelizer: the reason this server can carry hundreds of users
// on modest hardware, and a 32 MHz front end without a GPU.
//
// The expensive transform is done ONCE per band per block, no matter how many
// listeners there are. Each listener then costs only:
//   - a gather of L bins out of the shared spectrum, times a filter mask
//   - one L-point inverse FFT (L = 256 for a 12 kHz channel)
//   - a complex rotation for sub-bin tuning
//
// Structurally this is a weighted overlap-add filter bank: a forward FFT of
// size K over a windowed, 50%-overlapped block; a slice of L bins around the
// listener's frequency; an inverse FFT of size L; then a synthesis window and
// overlap-add.
//
// The windows are what make it clean. With a rectangular analysis window the
// transform of a single tone leaks into every bin, and the bins outside the
// extracted slice are simply thrown away - which is an error proportional to
// 1/L and put a hard ceiling near 22 dB on the whole receive chain, whatever
// the filter did. A sine window's leakage falls away far faster, and paired
// with the matching synthesis window the reconstruction is exact.
//
// Real front ends are handled by transforming the real signal directly and
// keeping only the positive-frequency bins. That IS the analytic signal, so
// the Hilbert transform a real input would otherwise need comes free with the
// transform we were doing anyway.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fft.h"
#include "fft_split.h"

namespace fernsdr {

enum class SignalKind {
    Iq,    // complex samples; spectrum spans centre +/- rate/2
    Real,  // real samples; spectrum spans 0 .. rate/2
};

class Channel;
class Channelizer;

// A view of one completed transform. The owner keeps the arrays unchanged
// until every consumer finishes. The index travels with the bins because
// odd-bin downconversion uses its parity, even after the producer advances.
class ChannelBlock {
public:
    const float* spectrum_re() const { return re_; }
    const float* spectrum_im() const { return im_; }
    size_t fft_size() const { return size_; }
    double bin_hz() const { return rate_ / static_cast<double>(size_); }
    double block_seconds() const { return static_cast<double>(size_) / (2.0 * rate_); }
    uint64_t block_index() const { return index_; }

private:
    friend class Channelizer;
    const float* re_ = nullptr;
    const float* im_ = nullptr;
    size_t size_ = 0;
    double rate_ = 0.0;
    uint64_t index_ = 0;
};

class Channelizer {
public:
    // `fft_size` must be a power of two; `sample_rate` is the raw input rate.
    Channelizer(double sample_rate, size_t fft_size, SignalKind kind = SignalKind::Iq);

    double sample_rate() const { return sample_rate_; }
    size_t fft_size() const { return fft_size_; }
    SignalKind kind() const { return kind_; }
    // Samples consumed per block.
    size_t block_size() const { return fft_size_ / 2; }
    double bin_hz() const { return sample_rate_ / static_cast<double>(fft_size_); }
    double block_seconds() const { return static_cast<double>(block_size()) / sample_rate_; }

    // Consumes exactly block_size() samples and refreshes the shared spectrum.
    // Use the one matching kind(); the other is a no-op.
    void process(const cfloat* input);
    void process_real(const float* input);
    // The same with the previous block passed in rather than kept: `older` is
    // the block before `newer`, block_size() samples each. A caller that
    // keeps its last block alive saves the copy process() makes of every
    // block, 2 MB a block at 64 Msps.
    void process_pair(const cfloat* older, const cfloat* newer);
    void process_real_pair(const float* older, const float* newer);
    // Counts a block nobody will read without transforming it. Nothing but
    // the index carries over between blocks, so the next process_pair() is
    // as valid as if every block had been transformed; the index keeps
    // counting blocks of time, so a channel on an odd centre bin, which
    // flips its sign on odd blocks, starts with the sign a busy band's would.
    void skip_block() { block_index_++; }

    // The analysis window, exposed so a Channel can pair it with the matching
    // synthesis window.
    const float* analysis_window() const { return analysis_window_.data(); }

    // The current block's spectrum, fft_size() bins, split real/imaginary.
    // For a real input the upper half is permanently zero, so a slice that
    // reaches past Nyquist reads silence rather than an alias.
    const float* spectrum_re() const { return spectrum_re_.data(); }
    const float* spectrum_im() const { return spectrum_im_.data(); }

    uint64_t block_index() const { return block_index_; }
    ChannelBlock current_block() const;
    // Exchanges the completed spectrum for equally sized reusable arrays.
    // The returned view borrows `re`/`im`. The channelizer's own arrays become
    // work storage and must be transformed again before they are consumed.
    // For real input, replacements must start zeroed and their upper halves
    // must remain untouched, like the channelizer's original spectrum arrays.
    ChannelBlock take_block(DspVector<float>& re, DspVector<float>& im);

private:
    double sample_rate_;
    size_t fft_size_;
    SignalKind kind_;

    // Only the selected input kind gets a full plan. At 64 Msps the unused
    // complex plan alone would hold another 16 MiB of scratch and twiddles.
    FftSplit fft_;
    RealFft real_fft_;

    DspVector<float> analysis_window_;
    // The previous block for process() and process_real(), made on first use.
    DspVector<float> history_re_;
    DspVector<cfloat> previous_;
    DspVector<float> spectrum_re_;
    DspVector<float> spectrum_im_;
    uint64_t block_index_ = 0;
};

// One listener's slice of a band. Holds the filter mask and the fine-tuning
// phase; owns no copy of the band's samples.
class Channel {
public:
    // `ifft_size` (L) must divide the channelizer's FFT size. The output rate
    // is sample_rate * L / K, and L/2 samples are produced per block.
    Channel(const Channelizer& parent, size_t ifft_size);

    double output_rate() const { return output_rate_; }
    size_t output_per_block() const { return ifft_size_ / 2; }

    // Sets the passband. `center_hz` is the tuning point relative to the
    // band's spectrum origin (see Channelizer); `low_hz` and `high_hz` are the
    // passband edges relative to that tuning point.
    void set_passband(double center_hz, double low_hz, double high_hz);

    double center_hz() const { return center_hz_; }
    double low_hz() const { return low_hz_; }
    double high_hz() const { return high_hz_; }

    // The width of an ideal filter that would pass as much white noise as this
    // one does, in hertz. The realised mask's edges are softer than the
    // passband asked for, which on a 500 Hz filter passes 0.7 dB less noise
    // than the nominal width says. Current once pull() has run since the last
    // set_passband().
    double noise_bandwidth_hz() const { return noise_bandwidth_hz_; }

    // Produces output_per_block() complex baseband samples, passband at DC.
    void pull(const Channelizer& parent, cfloat* out);
    void pull(const ChannelBlock& block, cfloat* out);

private:
    void rebuild_mask();

    size_t fft_size_;
    size_t ifft_size_;
    double sample_rate_;
    double bin_hz_;
    double output_rate_;
    FftSplit ifft_;

    double center_hz_ = 0.0;
    double low_hz_ = -1500.0;
    double high_hz_ = 1500.0;

    long center_bin_ = 0;
    double residual_hz_ = 0.0;
    std::vector<float> mask_;
    double noise_bandwidth_hz_ = 0.0;
    const float* synthesis_window_;  // one copy per length, for every channel
    // Second half of the previous frame, waiting to be added to the first half
    // of this one.
    std::vector<float> tail_re_;
    std::vector<float> tail_im_;
    double phase_ = 0.0;
    // The sub-bin rotation per output sample, and over four samples; they
    // change only with the tuning, so they are worked out there.
    double step_ = 0.0;
    double step_re_ = 1.0, step_im_ = 0.0;
    double step4_re_ = 1.0, step4_im_ = 0.0;
    bool mask_dirty_ = true;
};

}  // namespace fernsdr
