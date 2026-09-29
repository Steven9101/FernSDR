#include "listener.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstring>

#include "band.h"
#include "protocol.h"

namespace fernsdr {

namespace {

// Beyond this the client is not keeping up.  Waterfall lines are dropped
// first because a missing line is invisible, whereas a gap in the audio is
// exactly what users come here to avoid.
constexpr size_t kOutboxSoftLimitBytes = 256 * 1024;
constexpr size_t kOutboxHardLimitBytes = 1024 * 1024;

// Rounds fs/target to a power of two, so the channelizer's inverse transform
// length stays a power of two.  The resulting audio rate is rarely a round
// number - the client resamples anyway, and forcing 12000 exactly would mean
// a resampler on the server, per user.
size_t decimation_for(double band_rate, int target_rate, size_t fft_size) {
    const double ideal = band_rate / std::max(1, target_rate);
    size_t decimation = 1;
    while (decimation * 2 <= static_cast<size_t>(ideal * 1.4142) && decimation < fft_size / 16) {
        decimation *= 2;
    }
    return std::max<size_t>(1, decimation);
}

// The lowest rate the receiver page plays.
constexpr double kMinimumAudioRate = 4000.0;

// The lowest channel rate that still carries the passband, so that a narrow
// mode is not processed at a wide one's rate. Everything after the channel,
// the AGC, the demodulator, the filters and the encoder, costs in proportion
// to the rate, and the channel filter is no worse at a lower one: its length
// in time is set by the channelizer's block, not by the rate.
//
// The channel is complex and holds half its rate either side of the carrier.
// The passband's outer edge must fit, with the transition the mask puts past
// it and the spread of the realisable filter, six bins; see
// Channel::rebuild_mask. The rate asked for is the ceiling.
size_t channel_decimation(double band_rate, int requested_rate, size_t fft_size, double low_hz, double high_hz) {
    size_t decimation = decimation_for(band_rate, requested_rate, fft_size);
    const double bin_hz = band_rate / static_cast<double>(fft_size);
    const double width = high_hz - low_hz;
    const double transition = std::max(3.0 * bin_hz, std::min(0.15 * width, 300.0));
    const double needed = std::max(std::fabs(low_hz), std::fabs(high_hz)) + transition / 2.0 + 6.0 * bin_hz;
    while (decimation * 2 <= fft_size / 16) {
        const double rate = band_rate / static_cast<double>(decimation * 2);
        if (rate < kMinimumAudioRate || rate / 2.0 < needed) break;
        decimation *= 2;
    }
    return decimation;
}

}  // namespace

double channel_audio_rate(double band_rate, int requested_rate, size_t fft_size) {
    return band_rate / decimation_for(band_rate, std::clamp(requested_rate, 4000, 48000), fft_size);
}

double passband_audio_rate(double band_rate, int requested_rate, size_t fft_size, double low_hz, double high_hz) {
    return band_rate / channel_decimation(band_rate, std::clamp(requested_rate, 4000, 48000), fft_size, low_hz, high_hz);
}

size_t wfm_channel_decimation(double band_rate, size_t fft_size, double low_hz, double high_hz) {
    // No ceiling but the band: the channel is as narrow as the passband allows.
    return channel_decimation(band_rate, static_cast<int>(std::min(band_rate, 1e9)), fft_size, low_hz, high_hz);
}

double wfm_channel_rate(double band_rate, size_t fft_size, double low_hz, double high_hz) {
    return band_rate / wfm_channel_decimation(band_rate, fft_size, low_hz, high_hz);
}


Listener::Listener(uint64_t id, const Band& band, uint8_t generation_seed)
    : id_(id),
      band_(band),
      band_sample_rate_(band.sample_rate()),
      band_center_hz_(band.center_hz()),
      spectrum_origin_hz_(band.spectrum_origin_hz()),
      band_rf_scale_(band.rf_scale()),
      band_fft_size_(band.fft_size()) {
    bitrate_budget_ = band.max_user_bitrate();
    audio_generation_ = generation_seed;
    pending_channel_.frequency_hz = band_center_hz_;
    apply_mode_defaults(pending_channel_);
    pending_viewport_.low_hz = band.low_hz();
    pending_viewport_.high_hz = band.high_hz();
    active_ = pending_channel_;
    active_viewport_ = pending_viewport_;
}

void Listener::set_channel(const ChannelSettings& settings) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    pending_channel_ = settings;
    channel_dirty_ = true;
    settings_pending_.store(true, std::memory_order_release);
}

// Interpret the configured bitrate as the SSB budget. Narrow CW gets less;
// AM/SAM and NFM get more room for their wider audio. These are allocation
// weights, not guarantees of equal perceived quality or fixed codec SNR.
// The operator's per-listener ceiling still bounds every mode.
float bitrate_scale_for(Mode mode) {
    switch (mode) {
        case Mode::Cw:
        case Mode::CwL:
            return 28.0f / 49.0f;
        case Mode::Am:
        case Mode::Sam:
            return 69.0f / 49.0f;
        case Mode::Nfm:
            return 89.0f / 49.0f;
        // Music to 15 kHz, rather than voice to 3.
        case Mode::Wfm:
            return 2.0f;
        default:
            return 1.0f;
    }
}

int Listener::scaled_bitrate() const {
    const float scaled = static_cast<float>(active_.audio_bitrate) * bitrate_scale_for(active_.mode);
    // The per-listener ceiling still decides: a mode that wants more than the
    // operator has budgeted gets what there is.
    const int ceiling = std::max(8000, bitrate_budget_.load(std::memory_order_relaxed) - 16000);
    return std::clamp(static_cast<int>(std::lround(scaled)), 8000, ceiling);
}

void Listener::set_bitrate_budget(int bits_per_second) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    bitrate_budget_ = std::max(16000, bits_per_second);
}

void Listener::set_viewport(const ViewportSettings& viewport) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    pending_viewport_ = viewport;
    viewport_dirty_ = true;
    settings_pending_.store(true, std::memory_order_release);
}

ChannelSettings Listener::channel() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return pending_channel_;
}

ViewportSettings Listener::viewport() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return pending_viewport_;
}

double Listener::actual_audio_rate() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return actual_audio_rate_;
}

uint8_t Listener::audio_generation() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return audio_generation_;
}

ListenerTelemetry Listener::telemetry() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return telemetry_;
}

RdsState Listener::rds() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return rds_;
}

void Listener::drain(std::vector<std::vector<uint8_t>>& out, AudioFormat* format) {
    // Most passes find nothing new for most listeners. A producer that adds
    // something sets the flag and then wakes the network thread, so a pass
    // that reads it clear misses nothing the next one will not deliver.
    if (!outbox_ready_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_ready_.store(false, std::memory_order_relaxed);
    if (format) *format = outbox_format_;
    while (!outbox_.empty()) {
        out.push_back(std::move(outbox_.front()));
        outbox_.pop_front();
    }
    outbox_bytes_ = 0;
}

size_t Listener::queued_bytes() const {
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    return outbox_bytes_;
}

bool Listener::enqueue(std::vector<uint8_t>&& message, bool droppable) {
    std::lock_guard<std::mutex> lock(outbox_mutex_);

    if (outbox_bytes_ >= kOutboxSoftLimitBytes && droppable) {
        dropped_messages_++;
        return false;
    }
    if (outbox_bytes_ >= kOutboxHardLimitBytes) {
        waterfall_reset_.store(true, std::memory_order_relaxed);
        // Even audio has to yield eventually: drop the oldest so the stream
        // resynchronises at the live edge rather than falling further behind.
        while (!outbox_.empty() && outbox_bytes_ >= kOutboxSoftLimitBytes) {
            outbox_bytes_ -= outbox_.front().size();
            outbox_.pop_front();
            dropped_messages_++;
        }
    }

    outbox_bytes_ += message.size();
    outbox_.push_back(std::move(message));
    outbox_ready_.store(true, std::memory_order_release);
    return true;
}

void Listener::apply_pending() {
    ChannelSettings channel_copy;
    ViewportSettings viewport_copy;
    bool channel_changed = false;
    bool viewport_changed = false;

    // Settings change a few times a minute; blocks arrive a hundred times a
    // second for every listener.
    if (!settings_pending_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        settings_pending_.store(false, std::memory_order_relaxed);
        if (channel_dirty_) {
            channel_copy = pending_channel_;
            channel_dirty_ = false;
            channel_changed = true;
        }
        if (viewport_dirty_) {
            viewport_copy = pending_viewport_;
            viewport_dirty_ = false;
            viewport_changed = true;
        }
    }

    if (viewport_changed) {
        const bool width_changed = viewport_copy.width != active_viewport_.width;
        const bool span_moved = viewport_copy.low_hz != active_viewport_.low_hz ||
                                viewport_copy.high_hz != active_viewport_.high_hz;
        const bool codec_changed = viewport_copy.adaptive_codec != active_viewport_.adaptive_codec ||
                                    viewport_copy.native_grid != active_viewport_.native_grid ||
                                    viewport_copy.step_db != active_viewport_.step_db ||
                                    viewport_copy.range_coded != active_viewport_.range_coded;
        active_viewport_ = viewport_copy;
        // A moved or resized viewport invalidates the decoder's previous line,
        // so the next one must be self-contained.
        if (width_changed || span_moved || codec_changed) {
            force_intra_ = true;
            waterfall_bits_per_line_ = 0;
            // Give a new view its first independent row on the next published
            // spectrum. Waiting for credit from the old view delays zooming.
            waterfall_credit_ = 1;
        }
    }

    if (!channel_changed) return;

    const bool rate_changed = channel_copy.requested_audio_rate != active_.requested_audio_rate;
    const bool bitrate_changed = channel_copy.audio_bitrate != active_.audio_bitrate;
    const bool mode_changed = channel_copy.mode != active_.mode;
    // Into or out of broadcast FM the channel's rate and the audio's part or
    // meet again: always a new channel.
    const bool wfm_changed = (channel_copy.mode == Mode::Wfm) != (active_.mode == Mode::Wfm);
    active_ = channel_copy;
    // Onto another station, passband or de-emphasis: another shared
    // demodulation, whose audio rate may differ too.
    const bool station_changed = active_.mode == Mode::Wfm && (!fm_ || fm_->key() != wfm_key());

    // A new channel at a new rate is a gap in the audio while the client
    // refills. So a passband that no longer fits moves up at once, but one
    // that has narrowed only moves down with the mode: dragging a filter edge
    // back and forth must not rebuild the channel every time it crosses.
    const size_t wanted = wanted_decimation();
    if (rate_changed || wfm_changed || station_changed || !channel_ || wanted < decimation_ ||
        (mode_changed && wanted != decimation_)) {
        needs_channel_rebuild_ = true;
    }

    // rebuild_channel() configures the demodulator for the new channel.
    const bool rebuilt = needs_channel_rebuild_;
    if (rebuilt) {
        rebuild_channel();
    } else {
        configure_filter();
    }
    if (encoder_) encoder_->set_target(audio_target());
    // Frames already in a packet carry sequence numbers the client expects,
    // so a packet cut short by leaving NAC3 is sent, not discarded.
    if (packet_frames_ > 0 && !active_.packet_audio) flush_audio_packet();

    if (mode_changed && !rebuilt) {
        demodulator_.configure(active_.mode, actual_audio_rate_);
        ctcss_.configure(active_.mode == Mode::Nfm ? actual_audio_rate_ : 0.0);
        tone_notch_.reset();
    }
    demodulator_.set_deemphasis(active_.mode == Mode::Wfm ? active_.wfm_deemphasis_us : active_.deemphasis_us);
    highpass_.set_highpass(actual_audio_rate_, active_.highpass_hz);

    configure_agc();

    post_.set_noise_reduction(active_.noise_reduction);
    post_.set_auto_notch(active_.auto_notch);
    post_.set_notches(active_.notches);

    if ((bitrate_changed || mode_changed) && encoder_) {
        encoder_->set_bitrate(scaled_bitrate());
    }
}

void Listener::configure_agc() {
    // NFM has no gain control: the discriminator hears only the phase, so a
    // gain ahead of it changes nothing but the S-meter and squelch reading,
    // which the AGC still takes. AM and SAM follow their carrier.
    const AgcProfile profile = agc_in_effect(active_);
    // The noise estimate is only kept up while a profile uses it; coming back
    // to one after Off, it starts from a fresh look rather than an old one.
    if (profile != AgcProfile::Off && agc_.profile() == AgcProfile::Off) channel_noise_.reset();
    agc_.set_profile(profile);
    agc_.set_follow_carrier(active_.mode == Mode::Am || active_.mode == Mode::Sam);
    agc_.set_manual_gain_db(active_.mode == Mode::Nfm || active_.mode == Mode::Wfm ? 0.0f : active_.manual_gain_db);
    agc_.set_max_gain_db(active_.max_gain_db);
}

size_t Listener::wanted_decimation() const {
    if (active_.mode == Mode::Wfm) {
        return channel_decimation(band_sample_rate_, static_cast<int>(std::min(band_sample_rate_, 1e9)), band_fft_size_,
                                  active_.bandwidth_low, active_.bandwidth_high);
    }
    return channel_decimation(band_sample_rate_, active_.requested_audio_rate, band_fft_size_, active_.bandwidth_low,
                              active_.bandwidth_high);
}

void Listener::rebuild_channel() {
    decimation_ = wanted_decimation();
    const size_t ifft_size = std::max<size_t>(16, band_fft_size_ / decimation_);

    channel_ = std::make_unique<Channel>(band_.channelizer(), ifft_size);
    real_input_ = band_.channelizer().kind() == SignalKind::Real;
    channel_noise_.reset();

    // Exact, not rounded: 64 Msps over 8192 is 7812.5 Hz, and a client told
    // 7813 would play 64 parts per million fast and drift against the stream.
    // Broadcast FM is demodulated at the channel's rate and heard at a
    // whole fraction of it.
    const double channel_rate = channel_->output_rate();
    double rate = channel_rate;
    if (active_.mode == Mode::Wfm) {
        fm_ = band_.share_fm(wfm_key());
        rate = fm_->audio_rate();
    } else {
        fm_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        actual_audio_rate_ = rate;
        audio_generation_ = static_cast<uint8_t>((audio_generation_ + 1) & 0x0F);
    }
    cw_filter_.reset();
    configure_filter();

    demodulator_.configure(active_.mode, channel_rate);
    ctcss_.configure(active_.mode == Mode::Nfm ? rate : 0.0);
    tone_notch_.reset();
    demodulator_.set_deemphasis(active_.mode == Mode::Wfm ? active_.wfm_deemphasis_us : active_.deemphasis_us);
    highpass_.reset();
    highpass_.set_highpass(rate, active_.highpass_hz);
    agc_.configure(channel_rate);
    configure_agc();
    post_.configure(rate);
    post_.set_noise_reduction(active_.noise_reduction);
    post_.set_auto_notch(active_.auto_notch);
    post_.set_notches(active_.notches);

    encoder_ = std::make_unique<nac::Encoder>(static_cast<int>(std::lround(rate)));
    audio_wire_bits_per_frame_ = 0;
    encoder_->set_bitrate(scaled_bitrate());
    encoder_->set_target(audio_target());
    packet_frames_ = 0;

    baseband_.assign(channel_->output_per_block(), cfloat(0.0f, 0.0f));
    audio_.assign(channel_->output_per_block(), 0.0f);
    pending_audio_.clear();
    {
        // Publish the format with its queue. A network tick can otherwise
        // announce the old rate and then drain frames produced at the new
        // rate between the two reads. Old audio is stale after a rate change.
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        outbox_.clear();
        outbox_bytes_ = 0;
        outbox_format_ = {rate, audio_generation_};
        outbox_ready_.store(true, std::memory_order_release);
        // Any row still queued went with the rest: the page will see the
        // gap, and needs a key row next, from this listener's own coder.
        force_intra_ = true;
        waterfall_lost_ = true;
    }
    needs_channel_rebuild_ = false;
}

nac::Nac3Target Listener::audio_target() const {
    nac::Nac3Target target;
    target.noise_margin_db = active_.audio_noise_margin_db;
    const double low = std::min(std::fabs(active_.bandwidth_low), std::fabs(active_.bandwidth_high));
    const double high = std::max(std::fabs(active_.bandwidth_low), std::fabs(active_.bandwidth_high));
    // A passband that spans the carrier folds both sides onto the same audio.
    const bool spans_carrier = active_.bandwidth_low < 0 && active_.bandwidth_high > 0;
    switch (active_.mode) {
        case Mode::Am:
        case Mode::Sam:
            // Broadcast audio fills its band without pauses, so the noise it
            // is heard against is often out of sight of the estimate; keep
            // every band at least 20 dB clear of the codec's own noise.
            target.max_snr_db = 36.0f;
            target.min_snr_db = 20.0f;
            target.passband_low_hz = 0.0f;
            target.passband_high_hz = static_cast<float>(high);
            break;
        case Mode::Nfm:
            // The discriminator's noise rises with audio frequency and the
            // de-emphasis tilts it again; measure it everywhere.
            target.max_snr_db = 36.0f;
            target.min_snr_db = 18.0f;
            break;
        case Mode::Wfm:
            // Programme audio, as on AM but four times wider.
            target.max_snr_db = 36.0f;
            target.min_snr_db = 20.0f;
            target.passband_low_hz = 0.0f;
            target.passband_high_hz = static_cast<float>(kWfmAudioHz);
            break;
        default:
            // SSB, CW and DSB carry digital modes and weak signals beside
            // strong ones; let a strong band keep up to 48 dB.
            target.max_snr_db = 48.0f;
            target.min_snr_db = 12.0f;
            target.passband_low_hz = spans_carrier ? 0.0f : static_cast<float>(low);
            target.passband_high_hz = static_cast<float>(high);
            break;
    }
    return target;
}

void Listener::configure_filter() {
    const bool narrow_cw = (active_.mode == Mode::Cw || active_.mode == Mode::CwL) &&
                          active_.bandwidth_high - active_.bandwidth_low <= 1000;
    narrow_cw_ = narrow_cw;
    // Let the decimated selector define narrow CW cutoffs. A narrow mask in
    // the short shared transform produces overlap aliases from strong nearby
    // tones inside this selector's passband, where the AGC then boosts them.
    // Passing the full decimated channel avoids that second narrow filter.
    const double half_rate = channel_->output_rate() / 2;
    channel_->set_passband(channel_offset_hz(),
        narrow_cw ? -half_rate : active_.bandwidth_low,
        narrow_cw ? half_rate : active_.bandwidth_high);
    cw_filter_.configure(channel_->output_rate(), active_.bandwidth_low, active_.bandwidth_high, narrow_cw);
}

void Listener::gather_passband(const ChannelBlock& channelizer) {
    const double bin_hz = channelizer.bin_hz();
    if (bin_hz <= 0.0) {
        squelch_re_.clear();
        squelch_im_.clear();
        return;
    }
    // The passband edges are relative to the tuning point, and the tuning
    // point is relative to the band's spectrum origin.
    const double center = channel_offset_hz();
    const long first = static_cast<long>(std::floor((center + active_.bandwidth_low) / bin_hz));
    const long last = static_cast<long>(std::ceil((center + active_.bandwidth_high) / bin_hz));
    const long bins = static_cast<long>(channelizer.fft_size());

    squelch_re_.clear();
    squelch_im_.clear();
    const float* re = channelizer.spectrum_re();
    const float* im = channelizer.spectrum_im();
    for (long bin = first; bin <= last; bin++) {
        // The transform wraps, so a passband straddling zero is contiguous
        // there even though its indices are not.
        const long index = ((bin % bins) + bins) % bins;
        squelch_re_.push_back(re[index]);
        squelch_im_.push_back(im[index]);
    }
}

void Listener::process_block(const Channelizer& channelizer, const SpectrumPyramid* spectrum,
                             const SpectrumPyramid* paired) {
    process_block(channelizer.current_block(), spectrum, paired);
}

void Listener::process_block(const ChannelBlock& channelizer, const SpectrumPyramid* spectrum,
                             const SpectrumPyramid* paired) {
    apply_pending();
    if (!channel_ || !encoder_) return;
    const int requested_bitrate = scaled_bitrate();
    stream_budget_.update(transport_backlog_.load(std::memory_order_relaxed), channelizer.block_seconds(), requested_bitrate,
                          control_bytes_.exchange(0, std::memory_order_relaxed),
                          media_expired_.exchange(false, std::memory_order_relaxed), measured_audio_bitrate_,
                          queue_delay_ms_.load(std::memory_order_relaxed));
    const int audio_bitrate = stream_budget_.audio_bitrate(requested_bitrate);
    if (encoder_->bitrate() != audio_bitrate) encoder_->set_bitrate(audio_bitrate);

    size_t heard = 0;
    float signal_dbfs = -160.0f;
    if (fm_) {
        // The station's audio, demodulated once for everyone on it this
        // block; empty on the block a new one is made in.
        const std::vector<float>& shared = fm_->audio();
        heard = shared.size();
        audio_.assign(shared.begin(), shared.end());
        signal_dbfs = fm_->level_dbfs();
        const RdsState* rds = fm_->rds();
        const uint64_t seen = rds ? rds->sequence : 0;
        if (fm_.get() != rds_source_ || seen != rds_seen_) {
            const bool station = fm_.get() != rds_source_;
            rds_source_ = fm_.get();
            rds_seen_ = seen;
            std::lock_guard<std::mutex> lock(telemetry_mutex_);
            rds_ = rds ? *rds : RdsState{};
            telemetry_.rds_sequence++;
            if (station) telemetry_.rds_station++;
        }
    } else {
        if (rds_source_) {
            rds_source_ = nullptr;
            rds_seen_ = 0;
            std::lock_guard<std::mutex> lock(telemetry_mutex_);
            rds_ = RdsState{};
            telemetry_.rds_sequence++;
            telemetry_.rds_station++;
        }
        const size_t count = channel_->output_per_block();
        baseband_.resize(count);
        audio_.resize(count);

        channel_->pull(channelizer, baseband_.data());
        cw_filter_.process(baseband_.data(), count);

        if (agc_.profile() != AgcProfile::Off) {
            // The AGC holds the channel's noise a fixed distance under its target,
            // so it has to be told how much there is. Narrow CW is cut by the
            // selector after the channel, whose own mask is left wide open.
            const double width = narrow_cw_ ? active_.bandwidth_high - active_.bandwidth_low
                                            : channel_->noise_bandwidth_hz();
            channel_noise_.update(channelizer, channel_offset_hz() + 0.5 * (active_.bandwidth_low + active_.bandwidth_high),
                                  real_input_);
            agc_.set_noise_power(channel_noise_.channel_power(width));
        }

        // The AGC runs on the complex signal ahead of demodulation, which is
        // where a hardware receiver puts it: the envelope is smooth so the gain
        // does not ripple, and AM stays undistorted when the carrier fades.
        // Impulse blanking has already happened once for the whole band, before
        // any channel filtering - see NoiseBlanker.
        agc_.process(baseband_.data(), count);

        demodulator_.process(baseband_.data(), count, audio_.data());
        heard = count;
        signal_dbfs = agc_.input_dbfs();
    }
    // The subaudible tone is measured before the low cut, which exists to
    // take it out of what the listener hears.
    if (active_.mode == Mode::Nfm) ctcss_.process(audio_.data(), heard);
    // After demodulation, where hum and subaudible tones are audio; ahead of
    // the squelch and the codec, so neither spends anything on them.
    highpass_.process(audio_.data(), heard);
    if (active_.mode == Mode::Nfm) {
        // About 10 Hz wide at the tone's measured frequency: deep enough to
        // take it out, narrow enough to leave everything else, and nothing
        // until a tone has been measured.
        const double tone = active_.ctcss_filter ? ctcss_.measured_hz() : 0.0;
        tone_notch_.set_notch(actual_audio_rate_, std::round(tone * 20.0) / 20.0, 10.0);
        tone_notch_.process(audio_.data(), heard);
    }

    bool squelch_open = signal_dbfs >= static_cast<float>(active_.squelch_dbfs);

    if (active_.auto_squelch) {
        // Taken from the band's transform over this listener's passband, and
        // only the passband: a signal two kilohertz away is not what they are
        // trying to hear. Read before demodulation, so the decision is about
        // what arrived rather than about what the AGC made of it.
        gather_passband(channelizer);
        auto_squelch_.update_from_bins(squelch_re_.data(), squelch_im_.data(), squelch_re_.size());
        squelch_open = squelch_open && auto_squelch_.open();
    } else if (auto_squelch_.statistic() != 0.0) {
        // Turned off: forget the hang counter, or switching it on again
        // inherits a decision made minutes ago.
        auto_squelch_.reset();
    }
    if (active_.mode == Mode::Nfm) {
        ctcss_.set_squelch_tone(active_.ctcss_squelch);
        squelch_open = squelch_open && ctcss_.squelch_open();
    }

    if (active_.volume != 1.0f) {
        for (size_t i = 0; i < heard; i++) audio_[i] *= active_.volume;
    }
    if (!squelch_open) {
        std::fill(audio_.begin(), audio_.begin() + static_cast<long>(heard), 0.0f);
    }

    // Hand whole codec frames to the encoder, carrying any remainder forward.
    if (active_.audio_enabled) {
        pending_audio_.insert(pending_audio_.end(), audio_.begin(), audio_.begin() + static_cast<long>(heard));
        // NAC3 follows the channel noise from before the AGC, so it needs to
        // know what gain this audio was given. The discriminator's output does
        // not depend on the level the AGC hands it, so FM audio carries only
        // the volume.
        if (active_.packet_audio) {
            const float agc_power = active_.mode == Mode::Nfm || active_.mode == Mode::Wfm
                                        ? 1.0f : std::pow(10.0f, agc_.gain_db() / 10.0f);
            encoder_->set_signal_gain(agc_power * active_.volume * active_.volume);
        }
        size_t offset = 0;
        while (pending_audio_.size() - offset >= nac::kFrameHop) {
            float* frame = pending_audio_.data() + offset;
            if (post_.active()) post_.process(frame);
            if (active_.packet_audio) {
                const bool muted = !squelch_open;
                // The muted flag belongs to a whole packet.
                if (packet_frames_ > 0 && muted != packet_muted_) flush_audio_packet();
                if (packet_frames_ == 0) {
                    encoder_->begin_packet();
                    packet_muted_ = muted;
                    packet_sequence_ = audio_sequence_;
                }
                encoder_->add_frame(frame);
                packet_frames_++;
                audio_sequence_++;
                if (packet_frames_ >= active_.audio_packet_frames) flush_audio_packet();
            } else {
                const auto& payload = encoder_->encode(frame, active_.compact_audio);
                emit_audio_frame(payload, !squelch_open);
            }
            offset += nac::kFrameHop;
        }
        pending_audio_.erase(pending_audio_.begin(), pending_audio_.begin() + static_cast<long>(offset));
    } else {
        pending_audio_.clear();
        // A partly built packet is stale once audio stops; its sequence
        // numbers are simply never sent.
        packet_frames_ = 0;
    }

    // Waterfall, paced independently of both the block rate and the rate the
    // analyser produces lines at.  Credit accrues on every block, not only on
    // blocks that happen to carry a new spectrum line: the analyser emits a
    // line every several blocks, so charging credit per line would silently
    // divide the user's requested rate by that factor.
    // Waterfall shares the remaining budget after audio. Sustained transport
    // congestion can also lower audio bitrate through stream_budget_ above.
    {
        const int budget = bitrate_budget_.load(std::memory_order_relaxed);
        const double audio_share = active_.audio_enabled && encoder_
            ? (audio_wire_bits_per_frame_ > 0 ? audio_wire_bits_per_frame_ * actual_audio_rate_ / nac::kFrameHop :
                encoder_->bitrate() + (proto::kAudioHeaderBytes + 4) * 8.0 * actual_audio_rate_ / nac::kFrameHop) : 0;
        const double waterfall_budget = std::max(0.0, budget - audio_share - stream_budget_.control_bitrate());
        const double scale = stream_budget_.waterfall_scale(requested_bitrate);
        const double affordable = waterfall_bits_per_line_ > 1.0 ? waterfall_budget / waterfall_bits_per_line_
                                                                  : std::numeric_limits<double>::infinity();
        effective_lines_per_second_ = std::max(0.0, std::min(active_viewport_.lines_per_second * scale, affordable));
        // How many times the full rate the budget would carry, for sharing.
        waterfall_room_ = scale >= 1.0 && active_viewport_.lines_per_second > 0.0
                              ? affordable / active_viewport_.lines_per_second : 0.0;
    }

    // Rows shared with everyone on the same view (see SharedWaterfall), for
    // as long as this listener takes every one of them: a link that cannot
    // carry the full rate, a lost row or a page that needs a key row puts it
    // back on rows of its own, until the shared stream's next key row.
    // Joining wants a tenth to spare and leaving waits for a twentieth short:
    // a listener right at its limit otherwise went back and forth, a key row
    // each way, and was sent more than its budget.
    const bool shareable = active_viewport_.enabled && active_viewport_.range_coded &&
                           waterfall_room_ >= (waterfall_joined_ ? 0.95 : 1.1);
    if (shareable) {
        const WaterfallKey key = WaterfallKey::for_viewport(active_viewport_);
        if (!shared_waterfall_ || shared_waterfall_->key() != key) {
            shared_waterfall_ = band_.share_waterfall(key);
            leave_shared_waterfall(true);
        }
    } else if (shared_waterfall_) {
        shared_waterfall_.reset();
        // For want of budget: its own rows at the rate it can carry, from
        // where the credit comes due.
        leave_shared_waterfall(false);
    }
    // A row the page did not get (a full queue, or a channel rebuild that
    // emptied it), or its asking for a key row, ends the shared stream for
    // it; other reasons this listener's own coder would start over with a
    // key row change nothing on the shared one.
    if (waterfall_joined_ && (waterfall_lost_ || waterfall_reset_.load(std::memory_order_relaxed))) {
        leave_shared_waterfall(true);
    }
    const bool shared_row = shared_waterfall_ && shared_waterfall_->has_row() &&
                            (waterfall_joined_ || shared_waterfall_->row_is_key());

    if (active_viewport_.enabled && !waterfall_joined_ && !shared_row) {
        waterfall_credit_ += channelizer.block_seconds() * effective_lines_per_second_;
    } else {
        waterfall_credit_ = 0.0;
    }
    // At half the band's line rate or less, every line this listener shows
    // stands for two or more the band made: show their mean.
    if (paired && effective_lines_per_second_ <= 0.55 * band_.spectrum_lines_per_second()) spectrum = paired;
    if (shared_row) {
        // Joining at a key row: whatever was lost before is behind it, and
        // it is the key row a page that asked for one was waiting for.
        if (!waterfall_joined_) {
            waterfall_lost_ = false;
            waterfall_reset_.store(false, std::memory_order_relaxed);
        }
        waterfall_joined_ = true;
        const auto& payload = shared_waterfall_->payload();
        if (shared_waterfall_->row_is_key()) keyframe_seconds_ = 0;
        emit_waterfall_line(payload, shared_waterfall_->row_low_hz(), shared_waterfall_->row_high_hz(),
                            shared_waterfall_->row_width(), shared_waterfall_->row_native());
        note_waterfall_line(payload.size());
    } else if (spectrum && active_viewport_.enabled && !waterfall_joined_ && waterfall_credit_ >= 1.0) {
        // Subtract rather than reset so the long-run rate is exact, but cap
        // the carry so a stalled band cannot burst a backlog of lines.
        waterfall_credit_ = std::min(waterfall_credit_ - 1.0, 1.0);
        double low_hz = 0.0, high_hz = 0.0;
        const bool native_grid = render_waterfall_row(*spectrum, active_viewport_, viewport_line_, low_hz, high_hz);
        const size_t width = viewport_line_.size();
        force_intra_ = force_intra_ || waterfall_reset_.exchange(false, std::memory_order_relaxed) || keyframe_seconds_ >= 2.0;
        const auto& payload = active_viewport_.range_coded
            ? ranged_encoder_.encode(viewport_line_.data(), width, force_intra_, active_viewport_.step_db)
            : waterfall_encoder_.encode(viewport_line_.data(), width, force_intra_,
                  active_viewport_.zero_runs, active_viewport_.adaptive_codec, active_viewport_.step_db);
        if (force_intra_) keyframe_seconds_ = 0;
        force_intra_ = false;
        emit_waterfall_line(payload, low_hz, high_hz, static_cast<uint16_t>(width), native_grid);
        note_waterfall_line(payload.size());
    }

    // A line waits for audio at most this long: longer than a two-frame
    // packet at the lowest rate, so it only runs out when audio has stopped.
    const double block_seconds = channelizer.block_seconds();
    if (!held_line_.empty()) {
        held_line_seconds_ += block_seconds;
        if (held_line_seconds_ >= 0.08 || !active_.audio_enabled) release_held_line();
    }

    // Meters refresh about ten times a second, which is what makes an S-meter
    // feel live.
    keyframe_seconds_ += block_seconds;
    telemetry_window_seconds_ += block_seconds;
    if (telemetry_window_seconds_ >= 0.1) {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        telemetry_.signal_dbfs = signal_dbfs;
        telemetry_.squelch_statistic = static_cast<float>(auto_squelch_.statistic());
        telemetry_.agc_gain_db = agc_.gain_db();
        telemetry_.squelch_open = squelch_open;
        telemetry_.pll_locked = demodulator_.pll_locked();
        telemetry_.pll_offset_hz = demodulator_.pll_offset_hz();
        telemetry_.ctcss_hz = active_.mode == Mode::Nfm ? ctcss_.tone_hz() : 0.0;
        telemetry_.waterfall_lines_per_second = effective_lines_per_second_;
        telemetry_window_seconds_ = 0.0;
    }

    // Bitrates are averaged over a whole second instead.  Waterfall lines
    // arrive several blocks apart, so a tenth-of-a-second window reports
    // either zero or one line's worth scaled up tenfold - a readout that
    // swings between 40 and 140 kbit/s while nothing is actually changing.
    bitrate_window_seconds_ += block_seconds;
    if (bitrate_window_seconds_ >= 1.0) {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        telemetry_.audio_bitrate = static_cast<int>(audio_bits_ / bitrate_window_seconds_);
        measured_audio_bitrate_ = telemetry_.audio_bitrate;
        telemetry_.waterfall_bitrate = static_cast<int>(waterfall_bits_ / bitrate_window_seconds_);
        audio_bits_ = 0;
        waterfall_bits_ = 0;
        bitrate_window_seconds_ = 0.0;
    }
}

void Listener::emit_audio_frame(const std::vector<uint8_t>& payload, bool muted) {
    emit_audio_message(payload, audio_sequence_++, 1,
                       proto::audio_flags(muted, audio_generation_, encoder_->used_compact()));
}

void Listener::flush_audio_packet() {
    if (packet_frames_ == 0) return;
    const auto& payload = encoder_->finish_packet();
    emit_audio_message(payload, packet_sequence_, packet_frames_,
                       proto::audio_flags(packet_muted_, audio_generation_, false, true));
    packet_frames_ = 0;
}

void Listener::emit_audio_message(const std::vector<uint8_t>& payload, uint16_t sequence, int frames,
                                  uint8_t flags) {
    std::vector<uint8_t> message(proto::kAudioHeaderBytes + payload.size());
    message[0] = proto::kStreamAudio;
    message[1] = flags;
    proto::write_u16(message.data() + 2, sequence);
    std::memcpy(message.data() + proto::kAudioHeaderBytes, payload.data(), payload.size());
    const size_t message_bytes = message.size();
    audio_bits_ += message_bytes * 8;
    const double wire_bits = (message_bytes + (message_bytes < 126 ? 2 : 4)) * 8.0 / std::max(1, frames);
    // Reserve more immediately when the signal gets busy, and release it
    // slowly when frames get smaller. Reserving the nominal codec ceiling
    // forever would hide the savings from the waterfall.
    audio_wire_bits_per_frame_ = std::max(wire_bits,
        audio_wire_bits_per_frame_ + (wire_bits - audio_wire_bits_per_frame_) / 32);
    enqueue(std::move(message), /*droppable=*/false);
    release_held_line();
}

void Listener::emit_waterfall_line(const std::vector<uint8_t>& payload, double low_hz, double high_hz,
                                   uint16_t width, bool native_grid) {
    std::vector<uint8_t> message(proto::kWaterfallHeaderBytes + payload.size());
    message[0] = proto::kStreamWaterfall;
    const bool ranged = active_viewport_.range_coded;
    message[1] = (!ranged && waterfall_encoder_.used_zero_runs() ? 1 : 0) |
                 (!ranged && waterfall_encoder_.used_adaptive() ? 2 : 0) | (native_grid ? 4 : 0) |
                 (active_viewport_.step_db == 2 ? 8 : 0) | (ranged ? 16 : 0);
    proto::write_u16(message.data() + 2, waterfall_sequence_++);
    proto::write_f64(message.data() + 4, low_hz);
    proto::write_f64(message.data() + 12, high_hz);
    proto::write_u16(message.data() + 20, width);
    std::memcpy(message.data() + proto::kWaterfallHeaderBytes, payload.data(), payload.size());
    // While audio flows, the line waits for the next audio message and leaves
    // in the same write. Tens of milliseconds are invisible on a waterfall,
    // and a write of its own costs a TCP segment, its forty-odd bytes of
    // headers on the listener's link and the kernel's work for each.
    if (active_.audio_enabled && encoder_) {
        release_held_line();
        held_line_ = std::move(message);
        held_line_seconds_ = 0.0;
        return;
    }
    // Droppable: a lost line leaves a one-pixel seam, a lost audio frame is
    // audible.  But the codec predicts each line from the previous one, so a
    // dropped line would desynchronise the client's decoder until the next
    // viewport change.  Force the following line to be self-contained.
    if (!enqueue(std::move(message), /*droppable=*/true)) {
        force_intra_ = true;
        waterfall_lost_ = true;
    }
}

void Listener::note_waterfall_line(size_t payload_bytes) {
    const double bits = static_cast<double>(proto::kWaterfallHeaderBytes + payload_bytes) * 8.0;
    const double wire_bits = bits + (proto::kWaterfallHeaderBytes + payload_bytes < 126 ? 16 : 32);
    waterfall_bits_ += static_cast<uint64_t>(bits);
    // Smoothed so the rate limiter reacts to the trend rather than to one
    // unusually busy line.
    waterfall_bits_per_line_ = waterfall_bits_per_line_ > 0.0 ? 0.9 * waterfall_bits_per_line_ + 0.1 * wire_bits
                                                               : wire_bits;
}

void Listener::leave_shared_waterfall(bool at_once) {
    if (!waterfall_joined_) return;
    // Its own rows, starting with a key row: the page cannot continue the
    // shared stream's model with them. At once after a loss, which the page
    // is waiting to recover from; otherwise when the credit comes due.
    waterfall_joined_ = false;
    waterfall_lost_ = false;
    force_intra_ = true;
    waterfall_credit_ = at_once ? 1.0 : 0.0;
}

void Listener::release_held_line() {
    if (held_line_.empty()) return;
    if (!enqueue(std::move(held_line_), /*droppable=*/true)) {
        force_intra_ = true;
        waterfall_lost_ = true;
    }
    held_line_.clear();
}

}  // namespace fernsdr
