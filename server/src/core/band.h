// One receiver band: a source, the shared transforms, and the listeners
// tuned inside it.
//
// Each band owns its source thread. Large independent transforms and groups
// of listeners can use the radio's bounded worker pool. Wide bands overlap
// the next transform with one retained listener batch; transforms are shared
// across listeners.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../dsp/agc.h"
#include "../dsp/channelizer.h"
#include "../dsp/input_conditioner.h"
#include "archive.h"
#include "decoder_tap.h"
#include "shared_fm.h"
#include "shared_waterfall.h"
#include "zoom_bank.h"
#include "calibration.h"
#include "../dsp/noise_floor.h"
#include "../dsp/spectrum.h"
#include "../source/source.h"
#include "../util/config.h"
#include "listener.h"
#include "row_ring.h"
#include "dsp_workers.h"

namespace fernsdr {

struct BandInfo {
    std::string id;
    std::string name;
    double center_hz = 0.0;
    double sample_rate = 0.0;
    double low_hz = 0.0;
    double high_hz = 0.0;
    size_t spectrum_bins = 0;
    int max_user_bitrate = 0;
    float noise_blanker = 0.0f;
    float blanked_fraction = 0.0f;
    // The front end's corrections; see InputConditioner.
    bool iq_swap = false;
    bool dc_remove = false;
    bool iq_balance = false;
    float dc_offset_dbfs = -160.0f;
    float gain_error_db = 0.0f;
    float phase_error_degrees = 0.0f;
    float image_rejection_db = 0.0f;
    // The crystal error the frequency axis is corrected for, in parts per
    // million; see Band::rf_scale().
    double ppm = 0.0;
    double max_bandwidth_hz = 0.0;
    /** Level between the signals, in dBFS. -160 until the first line. */
    float noise_floor_dbfs = -160.0f;
    /** dBFS to dBm across this band. Empty means uncalibrated. */
    std::vector<CalibrationPoint> calibration;
    /** "off", "private" or "public". See BandHistory. */
    std::string history_access = "off";
    uint64_t history_bytes = 0;
    int64_t history_oldest_ms = 0;
    int64_t history_newest_ms = 0;
    int listeners = 0;
    bool running = false;
    // Whether the band's hours have it on the air now (see BandHours), what
    // they are, and when that next changes: UTC ms, -1 for not in the next
    // days. A band off the air is stopped on purpose, not down.
    bool on_air = true;
    std::string hours = "always";
    int64_t next_change_ms = -1;
    // Receiving right now; see Band::online().
    bool online = false;
    std::string status;
    std::string source_kind;
    std::string signal;
};

class Band {
public:
    Band(std::string id, std::string name, std::unique_ptr<Source> source, const ConfigSection& section);
    ~Band();

    const std::string& id() const { return id_; }

    // The section this band was built from, kept so a saved configuration can
    // be diffed against what is actually running.
    const std::map<std::string, std::string>& configured_values() const { return configured_; }
    const std::string& name() const { return name_; }

    /**
     * The strongest level in a window around `hz`, in dBFS, from the same
     * spectrum the waterfall is drawn from. Returns -160 before the first
     * line.
     *
     * This is what makes calibration something the receiver can do rather
     * than something the operator has to work out: feed in a known level,
     * read this, and the offset is the difference.
     */
    float peak_dbfs(double hz, double width_hz) const;

    /**
     * The latest spectrum line, reduced to `bins` cells by keeping each
     * cell's strongest level, so a narrow carrier survives the reduction.
     * False before the first line. For the admin panel's live view.
     */
    bool spectrum_snapshot(size_t bins, std::vector<float>& levels, double& low_hz, double& high_hz) const;

    /**
     * The last minute of the band as the admin panel's waterfall draws it: a
     * line every kRecentIntervalMs, each reduced to kRecentBins cells by
     * keeping the strongest level and quantised the way the archive is. The
     * panel opens a band on its recent past instead of on a waterfall that
     * takes a minute to fill. `rows` gets the lines oldest first; returns how
     * many, and `age_ms` says how long ago the newest was taken, so a band
     * that stopped is not shown as if it were live.
     */
    static constexpr size_t kRecentBins = 512;
    static constexpr size_t kRecentLines = 120;
    static constexpr int kRecentIntervalMs = 500;
    size_t recent_spectrum(std::vector<uint8_t>& rows, int64_t& age_ms) const;

    /**
     * The strongest carrier within `span_hz` around `hz`, for calibrating the
     * frequency axis against a station whose frequency is known. Found to a
     * fraction of a bin: the peak and its two neighbours, in dB, lie on a
     * parabola, as the Blackman-Harris window's main lobe nearly does. False
     * before the first spectrum line or when `hz` is not on the band.
     */
    bool find_carrier(double hz, double span_hz, double& found_hz, float& level_dbfs) const;

    /** Where the waterfall archive is kept, or would be once it is turned on. */
    std::string history_path() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return history_path_;
    }

    /** "off", "private" (admin only) or "public" (anyone). */
    std::string history_access() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return history_access_;
    }

    /**
     * Turns the archive on or off, or changes who may read it, while the band
     * runs. Changing the retention or the width reopens the file, which
     * starts the record again: the shape of the ring is fixed when it is
     * created, and reading old lines at a new width would draw nonsense.
     */
    bool set_history(const std::string& access, int hours, size_t width, double interval,
                     std::string& error);

    /** What the archive costs on disk, and the span it holds. */
    uint64_t history_bytes() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return archive_.is_open() ? archive_.size_bytes() : 0;
    }
    int history_hours() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return history_hours_;
    }
    size_t history_bins() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return history_bins_;
    }
    double history_interval() const {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        return history_interval_;
    }

    /**
     * Reads a span of the archive. Returns false when there is no archive.
     * The caller has already decided whether this person may see it.
     */
    bool read_history(int64_t from_ms, int64_t to_ms, std::vector<uint8_t>& rows,
                      std::vector<int64_t>& times_ms, size_t& bins, double* row_ms = nullptr,
                      size_t max_rows = 0, size_t width = 0) const {
        WaterfallArchive::Reading reading;
        {
            // Only while the positions are taken: the reading below may be
            // slow, and this is the lock the band's thread takes each line.
            std::lock_guard<std::mutex> lock(archive_mutex_);
            if (!archive_.is_open() || !archive_.begin_read(reading)) return false;
        }
        // Rounded up: a little under a bin per pixel does not show, and it
        // halves what a full-width picture of a wide archive costs.
        const size_t group = width > 0 ? std::max<size_t>(1, (reading.bins + width - 1) / width) : 1;
        return WaterfallArchive::read(reading, from_ms, to_ms, rows, times_ms, row_ms, max_rows, group, &bins);
    }
    // Frequencies on the band are RF frequencies: the sample rate as the
    // radio frequencies see it, rf_rate(), not the rate the samples are
    // processed at. The two differ by the crystal error the operator set with
    // `ppm`.
    double sample_low_hz() const {
        return signal_kind_ == SignalKind::Real ? spectrum_origin_hz_ : center_hz_ - rf_rate() / 2;
    }
    double sample_high_hz() const { return sample_low_hz() + rf_rate() / (signal_kind_ == SignalKind::Real ? 2 : 1); }
    // FFT bin zero is centred at sample_low_hz(), while rendered cells start
    // half a bin earlier. Mixing these conventions visibly offsets a tone
    // from its tuning marker when the display is zoomed into individual bins.
    double spectrum_low_hz() const { return sample_low_hz() - rf_rate() / spectrum_bins_ / 2; }
    double spectrum_high_hz() const { return sample_high_hz() - rf_rate() / spectrum_bins_ / 2; }
    // The rate the DSP runs at: what the source says it delivers.
    double sample_rate() const { return sample_rate_; }
    /**
     * How the band's RF frequencies stand to the samples: 1 + ppm / 1e6. One
     * crystal clocks both the tuner and the converter in most receivers, so
     * an error in it stretches the whole frequency axis about zero: the
     * centre is off by its own frequency times the error, and every hertz
     * of offset from the centre by the same factor. A frequency offset from
     * an upconverter is outside this; its crystal is another one.
     */
    double rf_scale() const { return rf_scale_; }
    // The upconverter or transverter offset the operator stated.
    double frequency_offset_hz() const { return frequency_offset_hz_; }
    double rf_rate() const { return sample_rate_ * rf_scale_; }
    // The RF frequency the channelizer's spectrum is centred on. For a real
    // front end the spectrum starts at DC, so this is rate/4 plus any
    // upconverter offset.
    double center_hz() const { return center_hz_; }
    double low_hz() const { return low_hz_; }
    double high_hz() const { return high_hz_; }
    // The RF frequency of channelizer bin 0. For an IQ front end that is the
    // tuner's centre; for a direct-sampling one it is DC, plus any
    // upconverter offset. A listener's slice is positioned relative to this,
    // and getting it wrong tunes every real-input receiver to the wrong place.
    double spectrum_origin_hz() const { return spectrum_origin_hz_; }
    SignalKind signal_kind() const { return signal_kind_; }
    size_t fft_size() const { return fft_size_; }
    // Widest passband a single listener may ask for.
    double max_bandwidth_hz() const { return max_bandwidth_hz_; }
    // Whether listeners may use broadcast FM here: a band sampled wide enough
    // for its channel, unless the operator said wfm = no.
    bool wfm() const { return wfm_allowed_ && sample_rate_ >= kWfmMinimumBandRate; }
    static constexpr double kWfmMinimumBandRate = 240000.0;

    // Settable while the band is running, from the admin panel. Atomic
    // because the DSP thread reads them every block and the HTTP thread
    // writes them; a torn read of a float here would be a click in
    // somebody's audio, and there is no reason to risk one.
    void set_display_name(std::string name) { name_ = std::move(name); }
    void set_noise_blanker(float strength) { blanker_.set_strength(strength); }
    void set_iq_swap(bool on) { conditioner_.set_swap(on); }
    void set_dc_remove(bool on) { conditioner_.set_dc_remove(on); }
    void set_iq_balance(bool on) { conditioner_.set_balance(on); }
    void set_max_bandwidth_hz(double hz) { max_bandwidth_hz_ = hz; }

    /**
     * Replaces the S-meter calibration while the band runs.
     *
     * Guarded because the band thread reads it when it fills a BandInfo; the
     * points are a vector, so publishing a new one is not a single store.
     */
    bool set_calibration(const std::string& text, std::string& error) {
        Calibration parsed;
        if (!parsed.parse(text, error)) return false;
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        calibration_ = std::move(parsed);
        return true;
    }

    std::string calibration_text() const {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        return calibration_.to_string();
    }
    void set_max_user_bitrate(int bits) { max_user_bitrate_ = bits; }
    void set_default_audio_bitrate(int bits) { default_audio_bitrate_ = bits; }
    // Ceiling on one listener's combined audio + waterfall stream.
    int max_user_bitrate() const { return max_user_bitrate_; }
    /**
     * The audio bitrate a listener starts on for this band.
     *
     * Worth setting per band because the right answer depends on what the band
     * carries. Measured against a real crowded 20 m recording, 48 kbit/s keeps
     * 87.5% of FT8 decodes and 64 kbit/s keeps 95.2%; a receiver pointed at a
     * digital segment should start its listeners higher, and one carrying
     * broadcast voice has no reason to.
     */
    int default_audio_bitrate() const { return default_audio_bitrate_; }

    const Channelizer& channelizer() const { return *channelizer_; }
    // Spectrum lines the band's analyser makes each second.
    double spectrum_lines_per_second() const { return spectrum_lines_per_second_; }

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(); }

    /**
     * Whether samples are arriving. A band whose module has lost its device
     * is running, its thread waiting to try again, but not online. For any
     * other input the two are the same.
     */
    bool online() const {
        return running_.load() && (!source_->restartable() || source_online_.load());
    }
    /** Online, or offline only because the operator switched its input off. */
    bool healthy() const;
    /** Why the band is not online, for the operator; empty when it is. */
    std::string status() const;
    /** Seconds until the next attempt to restart a failed input, or -1. */
    double retry_in_seconds() const;
    /** What the input reports about itself, for the admin panel. */
    Json source_details() const { return source_->describe(); }
    bool apply_source_settings(const Json& settings, Json& result, std::string& error) {
        return source_->apply_live(settings, result, error);
    }
    /**
     * Gives the input new settings from a band section, for its next start,
     * and records them as what the band runs with. Only a module takes new
     * settings this way; everything else is fixed when the band is built.
     */
    bool reconfigure_source(const ConfigSection& section, std::string& error);

    /**
     * Stops and starts the band without touching its listeners.
     *
     * Runs on its own thread and returns immediately, because stop() joins the
     * DSP thread and that thread may be blocked in a read from a source that
     * has gone quiet - a FIFO with no writer, a dead UDP feed. Doing this
     * synchronously from the server's event loop would stall every other
     * listener on the receiver while one band refused to die.
     *
     * Returns false if a restart is already in flight.
     */
    bool restart();
    bool restarting() const { return restarting_.load(); }

    /**
     * What the schedule says of the band: whether it is on the air, the
     * hours as written, and when that next changes (UTC ms, -1 for not in
     * the next days). Radio sets this and starts or stops the band to
     * match; the band only reports it. Off the air, the band is healthy,
     * says when it comes back, and refuses an operator's restart.
     */
    void set_schedule(bool on_air, std::string hours, int64_t next_change_ms);
    bool on_air() const { return on_air_.load(); }
    int64_t next_change_ms() const { return next_change_ms_.load(); }
    /** Why the last start failed, empty if it did not. */
    std::string last_error() const;

    void add_listener(const std::shared_ptr<Listener>& listener);
    void remove_listener(uint64_t id);

    // A decoder's channel on this band; see DecoderTap. While any exist the
    // band keeps transforming for channels even with nobody listening, which
    // costs about a third of an idle band's work.
    // Broadcast FM for `key`, shared with every listener on it: the one
    // already running, or a new one that runs from the next block. Released
    // with the last listener holding it. Safe from any listener's thread.
    std::shared_ptr<SharedFm> share_fm(const FmKey& key) const;
    size_t shared_fm_count() const;
    // The shared rows for a view (see SharedWaterfall), made on first ask.
    std::shared_ptr<SharedWaterfall> share_waterfall(const WaterfallKey& key) const;
    size_t shared_waterfall_count() const;
    // A spectrum finer than the band's for a view it is too coarse for (see
    // ZoomSpectrum), shared by everyone on that view and made on first ask;
    // null when the band's own line resolves the view. It runs from the next
    // block and is released with the last holder. Safe from any listener's
    // thread.
    std::shared_ptr<ZoomSpectrum> share_zoom(const ViewportSettings& viewport) const;
    size_t zoom_count() const;
    size_t zoom_tile_count() const;

    std::shared_ptr<DecoderTap> add_decoder_tap(uint16_t index, double dial_hz, double offset_hz, double width_hz);
    void remove_decoder_tap(const std::shared_ptr<DecoderTap>& tap);
    size_t decoder_tap_count() const;
    int listener_count() const;

    BandInfo info() const;

    // Reads one block from the source and runs it through the shared
    // transforms and every listener.  This is the whole per-block body of the
    // band thread; exposing it lets a band be driven deterministically from a
    // test or a batch tool instead of only from its own thread.
    bool process_one_block();

    // Called after every block that produced output, so the network thread
    // can be woken to flush.  Set before start().
    void set_wake_callback(std::function<void()> callback) { wake_ = std::move(callback); }
    void set_dsp_workers(DspWorkers* workers) { dsp_workers_ = workers; }

private:
    bool start_running(std::string& error);
    void stop_running();
    // Opens the archive the configuration asked for, unless a set_history()
    // since has said otherwise.
    void open_deferred_history();
    void run();
    // Waits to restart a failed input. False when the band is stopping.
    bool supervise();
    void ensure_block_buffers();
    // Runs the channelizer on block `now` and the one before it.
    void channelize(size_t now);
    bool advance_block(bool overlap);
    void finish_listeners();

    std::string id_;
    std::string name_;
    std::unique_ptr<Source> source_;

    double sample_rate_ = 0.0;
    double rf_scale_ = 1.0;
    double frequency_offset_hz_ = 0.0;
    double center_hz_ = 0.0;
    size_t fft_size_ = 0;
    // Fraction of the sample rate that is actually usable; the outer edges of
    // any SDR's passband are past the anti-alias filter's corner and are not
    // worth showing as if they were real spectrum.
    double usable_fraction_ = 0.8;
    double low_hz_ = 0.0;
    double high_hz_ = 0.0;
    SignalKind signal_kind_ = SignalKind::Iq;
    double spectrum_origin_hz_ = 0.0;
    std::atomic<double> max_bandwidth_hz_{20000.0};
    bool wfm_allowed_ = true;
    std::atomic<int> max_user_bitrate_{100000};
    std::atomic<int> default_audio_bitrate_{48000};

    // Blanking happens once for the whole band, before any channel filtering:
    // see NoiseBlanker for why that placement is the only one that works.
    NoiseBlanker blanker_;
    InputConditioner conditioner_;
    std::unique_ptr<Channelizer> channelizer_;
    std::map<std::string, std::string> configured_;
    std::unique_ptr<SpectrumAnalyzer> spectrum_;
    size_t spectrum_bins_ = 0;
    double spectrum_lines_per_second_ = 25.0;
    int spectrum_averages_ = 8;
    float spectrum_smoothing_ = 0.5f;

    std::atomic<bool> running_{false};
    std::atomic<bool> restarting_{false};
    std::atomic<bool> on_air_{true};
    std::atomic<int64_t> next_change_ms_{-1};
    std::string hours_text_ = "always";  // guarded by error_mutex_
    std::atomic<bool> stopping_{false};
    // Supervision of a restartable input. The wait between attempts is
    // woken by stop_running(), which every way of stopping a band goes
    // through; the band thread alone touches attempts_ and online_since_.
    std::atomic<bool> source_online_{false};
    int attempts_ = 0;
    std::chrono::steady_clock::time_point online_since_{};
    std::mutex supervision_mutex_;
    std::condition_variable supervision_wake_;
    std::string status_;                    // guarded by error_mutex_
    SourceFailure offline_kind_ = SourceFailure::None;  // guarded by error_mutex_
    // When supervise() tries again; epoch when it is not waiting on a timer.
    std::chrono::steady_clock::time_point next_attempt_{};  // guarded by error_mutex_
    // The last failure logged, so a dongle left unplugged for a day writes
    // one warning rather than one every thirty seconds. Band thread only.
    std::string last_logged_failure_;
    std::mutex lifecycle_mutex_;
    std::thread thread_;
    std::thread restart_thread_;
    mutable std::mutex error_mutex_;
    std::string last_error_;

    mutable std::mutex listeners_mutex_;
    std::vector<std::shared_ptr<Listener>> listeners_;
    mutable std::mutex taps_mutex_;
    std::vector<std::shared_ptr<DecoderTap>> taps_;
    // The band only watches them: listeners own them, and one nobody holds
    // any more is dropped at the next block.
    mutable std::mutex fm_mutex_;
    mutable std::vector<std::weak_ptr<SharedFm>> shared_fm_;
    std::vector<std::shared_ptr<SharedFm>> fm_snapshot_;
    mutable std::mutex waterfall_mutex_;
    mutable std::vector<std::weak_ptr<SharedWaterfall>> shared_waterfalls_;
    std::vector<std::shared_ptr<SharedWaterfall>> waterfall_snapshot_;
    // Made with the channelizer; its own lock makes share_zoom() safe from
    // any listener's thread.
    std::unique_ptr<ZoomBank> zoom_bank_;
    std::vector<std::shared_ptr<DecoderTap>> tap_snapshot_;

    std::function<void()> wake_;

    // Two input blocks for process_one_block, used in turn: the channelizer
    // reads the previous one as its older half, so it keeps no copy of it.
    // Owned here so a single-stepped band does not reallocate every call.
    DspVector<cfloat> block_input_[2];
    DspVector<float> block_real_[2];
    size_t block_current_ = 0;
    std::vector<float> spectrum_line_;
    // Guarded because an HTTP thread reads it while the band thread writes it.
    mutable std::mutex spectrum_mutex_;
    // The recent spectrum, guarded by spectrum_mutex_ with the time its newest
    // row was taken. `recent_line_` and `recent_due_` are the band thread's.
    void record_recent();
    RowRing recent_{kRecentBins, kRecentLines};
    std::chrono::steady_clock::time_point recent_taken_{};
    std::chrono::steady_clock::time_point recent_due_{};
    std::vector<uint8_t> recent_line_;
    NoiseFloor noise_floor_;
    Calibration calibration_;
    mutable std::mutex calibration_mutex_;

    /**
     * The rolling record of this band, when the operator has asked for one.
     *
     * Guarded because the band thread writes lines to it while an HTTP thread
     * reads ranges out of it.
     */
    WaterfallArchive archive_;
    mutable std::mutex archive_mutex_;
    std::string history_access_ = "off";
    // The configuration asked for an archive that start() has yet to open.
    bool history_deferred_ = false;
    std::string history_path_;
    int history_hours_ = 24;
    size_t history_bins_ = 1024;
    double history_interval_ = 1.0;
    // Read by the admin and telemetry threads while the band thread writes it.
    std::atomic<float> noise_floor_dbfs_{-160.0f};
    SpectrumPyramid pyramid_;
    // The mean, in dB, of this spectrum line and the one before, for
    // listeners who show a line for every two or more the band makes. Taking
    // one and skipping the next threw away half of what was measured; the
    // mean has less of the random speckle, which the waterfall codec otherwise
    // spends most of its bits on, and faint carriers stand out of it better.
    SpectrumPyramid paired_pyramid_;
    std::vector<float> previous_line_;
    std::vector<float> paired_line_;
    std::vector<std::shared_ptr<Listener>> listener_snapshot_;
    DspWorkers* dsp_workers_ = nullptr;
    DspVector<float> listener_spectrum_re_, listener_spectrum_im_;
    ChannelBlock listener_block_;
    bool listener_line_ = false;
    std::unique_ptr<DspWorkers::Job> listener_job_;
    double spectrum_low_hz_ = 0.0;
    double spectrum_high_hz_ = 0.0;
};

// Chooses a channelizer FFT size for a sample rate.  Aims for roughly 50 Hz
// bins, which simultaneously puts the block length near 10 ms - the two
// constraints turn out to be the same one.
size_t choose_fft_size(double sample_rate);

}  // namespace fernsdr
