// NAC - FernSDR Audio Codec.
//
// A low-delay MDCT transform codec sized for SDR audio.  The design goals,
// in priority order:
//
//  1. Waveform fidelity.  There is no pitch prediction, no time warping, no
//     bandwidth extension and no packet-loss time-stretching, because every
//     one of those smears the signal in time and breaks the digital modes
//     users decode from the audio stream.  Decode is a pure linear
//     dequantise + IMDCT + overlap-add.
//  2. Low delay.  128-sample hop with a 256-sample window: 21.3 ms of
//     algorithmic delay at 12 kHz, and no look-ahead beyond the window.
//  3. Uniform SNR across the passband.  Quantiser step size tracks each
//     band's own RMS, so a weak tone sitting 40 dB below a strong carrier
//     elsewhere in the passband survives - which is exactly the case a
//     psychoacoustic codec would throw away.
//  4. ~30-55 kbit/s for a typical SSB channel.
//
// The bitstream is documented in docs/CODEC.md and mirrored by the decoder in
// web/src/dsp/nac.ts.  tests/test_nac.cpp pins the round trip.
#pragma once
#include <cstdint>
#include <vector>

#include "../dsp/mdct.h"
#include "../util/bitio.h"

namespace fernsdr {
namespace nac {

// Samples consumed/produced per frame.  The MDCT window is twice this.
constexpr size_t kFrameHop = 128;
constexpr size_t kNumCoeffs = kFrameHop;

// Coefficients are grouped into bands that widen with frequency: narrow at the
// bottom where a mistuned quantiser is most audible, wider up top where the
// scale-factor side cost would otherwise dominate.
constexpr int kNumBands = 17;
extern const uint8_t kBandWidths[kNumBands];
// Start index of each band, plus a terminating kNumCoeffs.
extern const uint8_t kBandStarts[kNumBands + 1];

// Global quality is carried as a 6-bit index in quarter-bit steps:
// Q = index / 4, and the quantiser step for a band is rms * 2^-Q.
constexpr int kQualityBits = 6;
constexpr int kMaxQualityIndex = (1 << kQualityBits) - 1;

// Bands quieter than this (relative to full scale) are coded as a single zero
// bit.  Demodulated audio is AGC-normalised to roughly unit RMS, so this sits
// about 120 dB down and only ever catches genuinely empty spectrum.
constexpr float kSilenceThreshold = 1e-6f;

// Bands more than this far below the loudest band in the frame are coded as
// silent.  Without it, window leakage keeps every band nominally "active" and
// the rate loop spends the entire budget holding 80 dB-down noise to the same
// relative accuracy as the signal.  60 dB of coded dynamic range is far more
// than any demodulated channel carries, so nothing decodable is lost.
// Units are quarter-steps of log2, matching the exponent scale: 40 ~= 60 dB.
constexpr int kDynamicRangeQuarterSteps = 40;

// Exponents are log2(rms) in quarter-octave steps; the first band in a frame
// is coded relative to this so a quiet frame stays cheap.
constexpr int kExponentReference = -40;

struct EncoderStats {
    int quality_index = 0;   // chosen by the rate loop
    int payload_bits = 0;    // size of the last frame
    int active_bands = 0;    // bands that were not coded as silent
    int side_bits = 0;       // NAC3: mask, steps and Rice parameters
};

// Frame layouts. Each audio packet names the layout it carries (see
// core/protocol.h), so a connection can mix them.
enum class Layout : uint8_t {
    Original,  // NAC: global quality, 17 activity bits, one Rice parameter
    Compact,   // NAC2: the same quantiser with packed side information
    PerBand,   // NAC3: a step and a Rice parameter for every band
};

// What a NAC3 frame has to preserve.
//
// Receiver audio is mostly the channel's own noise, and NAC and NAC2 give
// every band the same SNR relative to its own energy. That codes a band of
// noise 26 dB below itself, which nobody can hear or decode, while a band that
// holds one strong and one weak station is coded relative to the strong one
// and can lose the weak one. NAC3 instead sets each band's quantisation noise
// relative to the channel noise measured in the passband: `noise_margin_db`
// below it, where the codec adds 10*log10(1 + 10^(-margin/10)) dB to the
// noise a listener already hears (0.27 dB at 12). A band far above the noise
// needs no more than `max_snr_db` relative to itself.
struct Nac3Target {
    float noise_margin_db = 12.0f;
    float max_snr_db = 36.0f;
    // And no band gets less than `min_snr_db` relative to itself. This is what
    // holds when the noise estimate is wrong: continuous broadcast audio that
    // never leaves a gap looks like a raised noise floor.
    float min_snr_db = 12.0f;
    // The audio passband in Hz, where the channel noise is measured. Outside it
    // the channel filter has already removed the signal, and bands there only
    // have to stay well below the passband noise. 0/0 means the whole band.
    float passband_low_hz = 0.0f;
    float passband_high_hz = 0.0f;
    // Let a band's own minimum lower its noise estimate below the pooled one.
    bool band_minimum = true;
};

// Estimates the channel noise per NAC band from the codec's own coefficients.
//
// The passband's coefficients are pooled into a decaying histogram of their
// energies. The MDCT coefficients of Gaussian noise are Gaussian, so their
// energy follows a scaled chi-square distribution with one degree of freedom,
// whose 20th percentile is 0.0642 times its mean. Pooling across the passband
// is what makes this robust to the signals that fool a per-band minimum
// tracker: an FT8 transmission lasts 12.6 s, a WSPR one 110 s and a beacon
// forever, but each occupies one or two of the passband's coefficients, so
// the low percentile of the pool still lands on noise until most of the
// time-frequency plane is occupied. A per-band minimum over about a second
// can only lower a band's estimate below the pooled one, which is what the
// filter's transition bands and FM's shaped noise need.
class NoiseEstimate {
public:
    void configure(int sample_rate, float passband_low_hz, float passband_high_hz);
    // Moves the passband without forgetting the noise heard so far: dragging a
    // filter edge changes where to look, not what the channel sounds like.
    void set_passband(float passband_low_hz, float passband_high_hz);
    void reset();
    // Feeds one frame of MDCT coefficients and their per-band mean energies.
    // `power_gain` is the gain the receiver's AGC applied to this audio, as a
    // power ratio. The estimate is kept before that gain, so the AGC turning
    // down for a strong station does not read as the noise floor dropping.
    void update(const float* coefficients, const float* band_energy, float power_gain = 1.0f);
    // Channel noise as a mean energy per coefficient at the current gain.
    float band_noise(int band) const { return band_noise_[band] * gain_; }
    // The pooled passband estimate, same units.
    float passband_noise() const { return passband_noise_ * gain_; }
    bool in_passband(int band) const { return band_in_passband_[band] != 0; }

private:
    static constexpr int kBins = 320;       // quarter-octave bins of energy
    static constexpr int kLowestOctave = -60;
    static constexpr int kSubwindows = 8;
    // A sixteen-second memory of the pooled estimate. Crowded digital
    // segments fill the passband with strong signals and the codec transform's
    // leakage, which lifts every percentile for as long as they transmit; but
    // FT8 stations transmit in step and all pause for 2.4 s of every 15, and
    // the minimum over this history remembers the noise heard then.
    static constexpr int kHistory = 32;

    void refresh();

    int sample_rate_ = 0;
    int first_coefficient_ = 0;
    int last_coefficient_ = kNumCoeffs;  // exclusive
    float decay_ = 0.99f;
    int subwindow_frames_ = 12;
    float histogram_[kBins]{};
    int lowest_bin_ = kBins;  // no bin below this has ever been added to
    double weight_ = 1.0;   // what one new observation adds, grown instead of decaying the bins
    double total_ = 0.0;
    int frames_ = 0;
    float smoothed_[kNumBands]{};
    float window_minimum_[kNumBands][kSubwindows]{};
    float current_minimum_[kNumBands]{};
    int subwindow_position_ = 0;
    int subwindow_count_ = 0;
    uint8_t band_in_passband_[kNumBands]{};
    float band_noise_[kNumBands]{};    // before the AGC's gain
    float passband_noise_ = 0.0f;      // before the AGC's gain
    float short_term_noise_ = 0.0f;    // the last second alone
    float gain_ = 1.0f;
    float history_[kHistory]{};
    int history_frames_ = 47;
    int history_position_ = 0;
    int history_count_ = 0;
};

class Encoder {
public:
    explicit Encoder(int sample_rate);

    int sample_rate() const { return sample_rate_; }
    double frame_rate() const { return static_cast<double>(sample_rate_) / kFrameHop; }

    // Target bitrate for the encoded audio stream.  The rate loop treats this
    // as a ceiling per frame, so the actual rate sits at or below it.
    void set_bitrate(int bits_per_second);
    int bitrate() const { return bitrate_; }

    // Caps how much quality the rate loop will buy even when the target
    // bitrate would allow more.  Keeps quiet channels from spending the full
    // budget encoding noise.
    void set_max_quality_index(int index);

    // Encodes exactly kFrameHop new samples.  The returned buffer stays valid
    // until the next call. `allow_compact` permits NAC2 whenever it is smaller.
    const std::vector<uint8_t>& encode(const float* samples, bool allow_compact = false);
    // Encodes with an explicit layout. PerBand frames are NAC3; the ceiling set
    // by set_bitrate() still bounds every frame, but a frame uses only what
    // its Nac3Target needs, so the average rate follows the signal. A PerBand
    // call produces a packet holding just this frame.
    const std::vector<uint8_t>& encode(const float* samples, Layout layout);

    // NAC3 packets carry one to kMaxPacketFrames consecutive frames. Frames
    // after the first predict their side information from the frame before
    // them. A packet arrives whole or not at all, so nothing is predicted
    // across packets and a lost packet costs only its own frames.
    void begin_packet();
    void add_frame(const float* samples);
    int packet_frames() const { return packet_frames_; }
    const std::vector<uint8_t>& finish_packet();
    bool used_compact() const { return layout_ == Layout::Compact; }
    Layout layout_used() const { return layout_; }

    void set_target(const Nac3Target& target);
    const Nac3Target& target() const { return target_; }
    // The power gain the AGC applied to the audio about to be encoded. NAC3
    // tracks the channel noise before this gain; see NoiseEstimate.
    void set_signal_gain(float power_gain) { signal_gain_ = power_gain; }

    const EncoderStats& stats() const { return stats_; }
    void reset();

    // The last NAC3 frame's quantised coefficients and which bands carried
    // them, for measurement tools. Meaningless after other layouts.
    const int32_t* last_quantised() const { return quantised_; }
    bool last_band_active(int band) const { return coded_active_[band] != 0; }
    int last_band_rice(int band) const { return band_rice_[band]; }
    int last_band_step(int band) const { return coded_step_[band]; }

private:
    void analyse(const float* samples);
    void prepare_costs();
    // Measures what the frame would cost at a given quality, without emitting.
    int estimate_bits(int quality_index, int* scale_bias = nullptr) const;
    void emit(int quality_index, int scale_bias, int original_bits, bool allow_compact);
    // NAC3: quantises every active band at its step plus `coarser` quarter
    // octaves and returns the frame's exact size in bits, including side
    // information coded as `predicted` from the previous frame or not.
    int per_band_cost(int coarser, bool predicted);
    void emit_frame(bool predicted);
    void update_references(bool first);

    int sample_rate_;
    int bitrate_ = 48000;
    int target_bits_per_frame_ = 512;
    int max_quality_index_ = kMaxQualityIndex;

    Mdct mdct_;
    const float* window_;          // the sine window, one copy for every encoder
    std::vector<float> overlap_;   // previous hop, kept for the MDCT window
    std::vector<float> normalised_;  // coeffs divided by their band scale
    std::vector<int> band_exponent_;
    std::vector<uint8_t> band_active_;
    std::vector<float> active_coeffs_;
    int side_bits_[3]{};

    BitWriter writer_;
    std::vector<uint8_t> frame_;
    EncoderStats stats_;
    Layout layout_ = Layout::Original;

    // NAC3 state.
    Nac3Target target_;
    // The target's decibels as power ratios, worked out once per target.
    float margin_ = 0.0625f, finest_ = 1.6e-5f, coarsest_ = 0.0625f;
    float relevance_inside_ = 0.0f, relevance_outside_ = 0.0f;
    int coded_coefficients_ = 0;  // nonzero after the last cost, for the ceiling search
    int previous_coarser_ = 0;    // the last frame's; the next usually needs about as much
    int costed_coarser_ = -1;     // what the state per_band_cost left behind was costed at
    // Within one frame, the coarsening from which each band is known to round
    // to nothing. A coarser step only shrinks every value, so a band that
    // vanished once stays vanished for the rest of the ceiling search.
    int silent_from_[kNumBands]{};
    // These two stay in the encoder rather than in the thread's scratch: the
    // costing loops that read and write them executed a tenth more
    // instructions when they lived in a thread-local object, which cost more
    // with few listeners than the cache saved with many.
    float scaled_[kNumCoeffs]{};  // coefficients over their uncoarsened step
    int32_t quantised_[kNumCoeffs]{};
    float signal_gain_ = 1.0f;
    NoiseEstimate noise_;
    float band_energy_[kNumBands]{};
    int band_step_[kNumBands]{};      // quarter-octave step index before any coarsening
    int coded_step_[kNumBands]{};     // what the frame actually carries
    int band_rice_[kNumBands]{};
    uint8_t coded_active_[kNumBands]{};
    uint8_t band_dropped_[kNumBands]{};
    int step_selector_ = 0;
    int per_band_side_bits_ = 0;
    // The packet being built, and what its next frame predicts from.
    int packet_frames_ = 0;
    int reference_step_[kNumBands]{};
    int reference_rice_[kNumBands]{};
    uint8_t reference_active_[kNumBands]{};
};

class Decoder {
public:
    explicit Decoder(int sample_rate);

    int sample_rate() const { return sample_rate_; }

    // Decodes one frame into exactly kFrameHop samples.  Returns false on a
    // malformed frame, in which case `out` is filled by concealment.
    bool decode(const uint8_t* data, size_t size, float* out, bool compact = false);
    // A PerBand payload must hold exactly one frame here; see decode_packet.
    bool decode(const uint8_t* data, size_t size, float* out, Layout layout);

    // Decodes a NAC3 packet into `out`, which holds `capacity` frames of
    // kFrameHop samples. Returns how many frames the packet carried, or 0 when
    // even its frame count is unreadable or exceeds `capacity`. `ok` is false
    // when any frame was malformed; that frame and those after it are then
    // concealed, so the output always holds whole frames on the sample clock.
    int decode_packet(const uint8_t* data, size_t size, float* out, int capacity, bool& ok);

    // Produces kFrameHop samples for a lost frame.  Deliberately a decaying
    // repeat of the last spectrum rather than a time-stretch: it keeps the
    // sample clock honest so digital decoders stay in sync.
    void conceal(float* out);

    void reset();

private:
    struct PacketReferences {
        int step[kNumBands]{};
        int rice[kNumBands]{};
        uint8_t active[kNumBands]{};
    };
    bool decode_per_band(BitReader& reader, bool predicted, PacketReferences& references);
    void synthesise(float* out);

    int sample_rate_;
    Mdct mdct_;
    std::vector<float> window_;
    std::vector<float> coeffs_;
    std::vector<float> prev_coeffs_;
    std::vector<float> time_;
    std::vector<float> overlap_;
    float conceal_gain_ = 1.0f;
};

// NAC3 step indices are quarter octaves of log2(step), within this range, and
// Rice parameters fit in four bits. A packet holds at most four frames.
constexpr int kMinStep = -200;
constexpr int kMaxStep = 200;
constexpr int kMaxRiceParameter = 15;
constexpr int kMaxPacketFrames = 4;

// After a packet's first frame, every band it left silent predicts from the
// nearest active band: the last one to its left, or the first active band for
// bands before it. With no active band everything predicts from zero. The
// encoder and decoder must fill exactly alike.
inline void fill_references(const uint8_t* active, int* step, int* rice) {
    int first = -1, last = -1;
    for (int b = 0; b < kNumBands; b++) {
        if (active[b]) {
            if (first < 0) first = b;
            last = b;
        } else if (last >= 0) {
            step[b] = step[last];
            rice[b] = rice[last];
        }
    }
    for (int b = 0; b < kNumBands && (first < 0 || b < first); b++) {
        step[b] = first < 0 ? 0 : step[first];
        rice[b] = first < 0 ? 0 : rice[first];
    }
}

// Rice parameter for a given global quality.  Shared so the encoder's cost
// estimate and the decoder agree without any side information.
inline uint32_t rice_k_for_quality(int quality_index) {
    int k = (quality_index / 4) - 1;
    if (k < 0) k = 0;
    if (k > 20) k = 20;
    return static_cast<uint32_t>(k);
}

}  // namespace nac
}  // namespace fernsdr
