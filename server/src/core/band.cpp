#include <chrono>
#include "band.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../util/log.h"
#include "../util/subprocess.h"

namespace fernsdr {

size_t choose_fft_size(double sample_rate) {
    // bin = rate/K, block = K/2 samples = K/(2*rate) seconds.  Asking for
    // ~50 Hz bins gives K ~ rate/50, and therefore a block of ~10 ms: fine
    // enough to tune with, short enough to stay low-latency.
    const double ideal = sample_rate / 50.0;
    size_t size = 1024;
    while (size * 2 <= ideal && size < (1u << 20)) size *= 2;
    return std::clamp<size_t>(size, 1024, 1u << 20);
}

namespace {

// What an input covers and what a band shows of it by default, as the band
// works them out; check_band_settings() and the band itself both take them
// from here, so what one accepts the other runs.
struct Coverage {
    double first = 0.0, last = 0.0;         // what the input covers
    double low = 0.0, high = 0.0;           // the default range shown
    double tolerance = 1.0;                 // hertz a stated edge may lie outside
};

// How much of the sample rate a band shows unless told otherwise. An IQ
// input shows all of it, as other web receivers do: the outer few percent
// are a few dB darker where the radio's own filter rolls off, and a very
// strong station just outside can show there as a faint mirror, but cutting
// a fifth off every radio to hide that cost more than it saved (an SDRplay
// at 10 Msps showed 1 to 9 MHz). usable_fraction, low and high narrow it.
// A real input's own filter sits just under half the rate, so its last
// percent above 0.94 is alias rather than signal.
double default_usable_fraction(SignalKind kind) { return kind == SignalKind::Real ? 0.94 : 1.0; }

Coverage coverage_of(const ConfigSection& section, double sample_rate, SignalKind kind, double source_center_hz) {
    const double offset = section.get_double("frequency_offset", 0.0);
    const double ppm = section.get_double("ppm", 0.0);
    const double scale = 1.0 + (std::isfinite(ppm) ? std::clamp(ppm, -500.0, 500.0) : 0.0) * 1e-6;
    const double rate = sample_rate * scale;
    const double usable =
        std::clamp(section.get_double("usable_fraction", default_usable_fraction(kind)), 0.1, 1.0);
    Coverage c;
    const double centre = kind == SignalKind::Real ? offset + rate / 4.0 : source_center_hz * scale + offset;
    if (kind == SignalKind::Real) {
        c.first = offset;
        c.last = offset + rate / 2.0;
        c.low = offset;
        c.high = offset + rate * usable / 2.0;
    } else {
        c.first = centre - rate / 2.0;
        c.last = centre + rate / 2.0;
        c.low = centre - rate * usable / 2.0;
        c.high = centre + rate * usable / 2.0;
        // Below 0 Hz on the dial an IQ input shows only the mirror of what
        // lies just above it, as when an SDRplay sits at 4 MHz with 10 Msps.
        // The floor is the input's own 0 Hz with a transverter's offset
        // added, and 0 Hz behind an upconverter, whose offset is negative.
        const double floor = std::max(0.0, offset);
        if (centre > floor) {
            c.first = std::max(c.first, floor);
            c.low = std::max(c.low, floor);
        }
    }
    // An edge written as the nominal one, before the crystal correction
    // moved it, still counts as inside.
    c.tolerance = 1.0 + std::fabs(scale - 1.0) * (std::fabs(centre) + rate / 2.0);
    return c;
}

// The range a band shows: low and high as stated, each defaulting to the
// usable range's edge, when they make a range inside the input.
bool stated_range(const ConfigSection& section, const Coverage& c, double& low, double& high) {
    low = section.get_double("low", c.low);
    high = section.get_double("high", c.high);
    return std::isfinite(low) && std::isfinite(high) && low < high && low >= c.first - c.tolerance &&
           high <= c.last + c.tolerance;
}

}  // namespace

bool check_band_settings(const ConfigSection& section, const Source& source, std::string& error) {
    const std::string where = "[" + section.name() + "] ";
    const auto power_of_two = [](long value) { return value > 0 && (value & (value - 1)) == 0; };
    // Both follow the sample rate by themselves; a value set by hand is held
    // to what the transforms are built and measured for, rather than taken
    // as it comes or quietly replaced.
    // Left out or empty is 0, which follows the sample rate; a value that is
    // not a number is -1, refused like any other, not read as left out.
    const auto stated = [&](const char* key) { return trim(section.get(key)).empty() ? 0 : section.get_int(key, -1); };
    const long fft = stated("fft_size");
    if (fft != 0 && (!power_of_two(fft) || fft < 1024 || fft > (1L << 20))) {
        error = where + "fft_size is a power of two from 1024 to 1048576; left out, it follows the sample rate";
        return false;
    }
    const long bins = stated("spectrum_bins");
    if (bins != 0 && (!power_of_two(bins) || bins < 1024 || bins > (1L << 21))) {
        error = where + "spectrum_bins is a power of two from 1024 to 2097152; left out, it follows the sample rate";
        return false;
    }
    if (!section.has("low") && !section.has("high")) return true;
    const Coverage c = coverage_of(section, source.sample_rate(), source.kind(), source.center_hz());
    double low = 0.0, high = 0.0;
    if (stated_range(section, c, low, high)) return true;
    char text[160];
    if (!(std::isfinite(low) && std::isfinite(high) && low < high)) {
        std::snprintf(text, sizeof text, "low (%.0f Hz) must be below high (%.0f Hz)", low, high);
    } else {
        std::snprintf(text, sizeof text, "low and high must lie in what the input covers, %.0f to %.0f Hz", c.first,
                      c.last);
    }
    error = where + text;
    return false;
}

Band::Band(std::string id, std::string name, std::unique_ptr<Source> source, const ConfigSection& section)
    : id_(std::move(id)), name_(std::move(name)), source_(std::move(source)) {
    configured_ = section.values();
    sample_rate_ = source_->sample_rate();
    signal_kind_ = source_->kind();

    fft_size_ = static_cast<size_t>(section.get_int("fft_size", 0));
    // Out of what check_band_settings() accepts, the default: a receiver that
    // started with such a file before an update starts with it after one.
    if (fft_size_ < 1024 || fft_size_ > (1u << 20) || (fft_size_ & (fft_size_ - 1)) != 0)
        fft_size_ = choose_fft_size(sample_rate_);

    usable_fraction_ = std::clamp(section.get_double("usable_fraction", default_usable_fraction(signal_kind_)), 0.1, 1.0);

    // An upconverter or transverter shifts everything; the operator states the
    // offset and the whole band plan lines up again.
    const double offset = section.get_double("frequency_offset", 0.0);
    frequency_offset_hz_ = offset;
    // A crystal that is off stretches the axis instead; see rf_scale(). Held
    // to what a crystal can plausibly be off by, so a typo in the file cannot
    // move the band somewhere else entirely.
    const double ppm = section.get_double("ppm", 0.0);
    rf_scale_ = 1.0 + (std::isfinite(ppm) ? std::clamp(ppm, -500.0, 500.0) : 0.0) * 1e-6;

    if (signal_kind_ == SignalKind::Real) {
        // Direct sampling: the spectrum runs from DC to half the sample rate.
        low_hz_ = offset;
        high_hz_ = offset + rf_rate() * usable_fraction_ / 2.0;
        center_hz_ = offset + rf_rate() / 4.0;
        spectrum_origin_hz_ = offset;
    } else {
        center_hz_ = source_->center_hz() * rf_scale_ + offset;
        low_hz_ = center_hz_ - rf_rate() * usable_fraction_ / 2.0;
        high_hz_ = center_hz_ + rf_rate() * usable_fraction_ / 2.0;
        spectrum_origin_hz_ = center_hz_;
    }
    // An operator can always state the usable range directly, within what
    // the input covers (check_band_settings() says why it was not taken).
    {
        const Coverage c = coverage_of(section, sample_rate_, signal_kind_, source_->center_hz());
        double low = 0.0, high = 0.0;
        if (stated_range(section, c, low, high)) {
            low_hz_ = low;
            high_hz_ = high;
        }
    }
    max_bandwidth_hz_ = std::max(1000.0, section.get_double("max_bandwidth", 20000.0));
    // Left out, wide FM is offered where the band shows FM broadcasting, so
    // that a long, medium or short wave band does not carry a mode with
    // nothing to hear; wfm = yes or no decides it either way.
    wfm_allowed_ = section.has("wfm") ? section.get_bool("wfm", true)
                                      : high_hz_ > kFmBroadcastLowHz && low_hz_ < kFmBroadcastHighHz;
    max_user_bitrate_ = static_cast<int>(std::clamp(section.get_int("max_user_bitrate", 100000), 16000L, 1000000L));
    default_audio_bitrate_ =
        static_cast<int>(std::clamp(section.get_int("audio_bitrate", 48000), 8000L, 128000L));

    spectrum_bins_ = static_cast<size_t>(section.get_int("spectrum_bins", 0));
    if (spectrum_bins_ < 1024 || spectrum_bins_ > (1u << 21) || (spectrum_bins_ & (spectrum_bins_ - 1)) != 0) {
        // Twice the channelizer's resolution: the waterfall is what users
        // hunt signals on, so it is worth resolving finer than the audio path.
        spectrum_bins_ = std::min<size_t>(fft_size_ * 2, 1u << 16);
    }
    spectrum_lines_per_second_ = std::clamp(section.get_double("spectrum_rate", 25.0), 1.0, 60.0);

    // What the operator measured, so the S-meter can quote dBm instead of a
    // number that only means something on this receiver. Refused rather than
    // guessed at: a meter that is confidently wrong is worse than one that
    // admits it is relative.
    // The archive is off unless the operator asks for it, and asking says who
    // may look: "private" is the admin only, "public" is anyone with the
    // link. It defaults to off because a rolling record of a band is also a
    // record of who transmitted when, and that is a decision to make
    // deliberately rather than to inherit.
    const std::string access = section.get("history", "off");
    history_path_ = section.get("history_path", "fernsdr-history-" + id_ + ".wfa");
    if (access == "private" || access == "public") {
        history_access_ = access;
        history_hours_ = static_cast<int>(section.get_int("history_hours", 24));
        history_bins_ = static_cast<size_t>(section.get_int("history_bins", 1024));
        history_interval_ = section.get_double("history_interval", 1.0);
        // Opened by start(), not here: the settings saved from the admin panel
        // are applied in between, and an archive opened in the configured
        // shape and then reshaped to the saved one would start again empty
        // on every restart.
        history_deferred_ = true;
    } else if (access != "off") {
        LOG_WARN("band", "%s history: \"%s\" is not one of off, private or public", id_.c_str(),
                 access.c_str());
    }

    const std::string calibration_text = section.get("calibration", "");
    if (!calibration_text.empty()) {
        std::string problem;
        if (calibration_.parse(calibration_text, problem)) {
            LOG_INFO("band", "%s S-meter calibration: %s", id_.c_str(),
                     calibration_.to_string().c_str());
        } else {
            LOG_WARN("band", "%s calibration ignored: %s", id_.c_str(), problem.c_str());
        }
    }
    spectrum_averages_ = static_cast<int>(std::clamp(section.get_int("spectrum_averages", 8), 1L, 64L));

    blanker_.configure(sample_rate_);
    blanker_.set_strength(static_cast<float>(std::clamp(section.get_double("noise_blanker", 0.0), 0.0, 1.0)));
    conditioner_.configure(sample_rate_);
    conditioner_.set_swap(signal_kind_ == SignalKind::Iq && section.get_bool("iq_swap", false));
    conditioner_.set_dc_remove(section.get_bool("dc_remove", false));
    conditioner_.set_balance(signal_kind_ == SignalKind::Iq && section.get_bool("iq_balance", false));

    channelizer_ = std::make_unique<Channelizer>(sample_rate_, fft_size_, signal_kind_);
    spectrum_smoothing_ = static_cast<float>(std::clamp(section.get_double("spectrum_smoothing", 0.5), 0.0, 0.95));
    spectrum_ = std::make_unique<SpectrumAnalyzer>(sample_rate_, spectrum_bins_,
                                                   spectrum_lines_per_second_, spectrum_averages_,
                                                   spectrum_smoothing_, signal_kind_);
    zoom_bank_ = std::make_unique<ZoomBank>(*channelizer_, spectrum_origin_hz_, rf_scale_,
                                            rf_rate() / static_cast<double>(spectrum_bins_), spectrum_lines_per_second_,
                                            spectrum_averages_, spectrum_smoothing_);
}

Band::~Band() {
    stop();
}

bool Band::start(std::string& error) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (restart_thread_.joinable()) restart_thread_.join();
    stopping_.store(false);
    open_deferred_history();
    const bool started = start_running(error);
    std::lock_guard<std::mutex> error_lock(error_mutex_);
    last_error_ = started ? std::string() : error;
    return started;
}

void Band::set_schedule(bool on_air, std::string hours, int64_t next_change_ms) {
    on_air_.store(on_air);
    next_change_ms_.store(next_change_ms);
    std::lock_guard<std::mutex> lock(error_mutex_);
    hours_text_ = std::move(hours);
}

void Band::open_deferred_history() {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    if (!history_deferred_) return;
    history_deferred_ = false;
    std::string problem;
    if (!archive_.open(history_path_, history_bins_, history_interval_, history_hours_, problem,
                       spectrum_low_hz(), spectrum_high_hz())) {
        history_access_ = "off";
        LOG_WARN("band", "%s history disabled: %s", id_.c_str(), problem.c_str());
        return;
    }
    LOG_INFO("band", "%s history %s: %d hours, %zu bins, %.1f s per line", id_.c_str(),
             history_access_.c_str(), history_hours_, history_bins_, history_interval_);
}

bool Band::start_running(std::string& error) {
    if (running_.load()) return true;
    // EOF ends the work, not the std::thread lifetime. Reap that handle before
    // assigning another one, including when an operator restarts a file input.
    if (thread_.joinable()) thread_.join();
    source_->stop();
    source_->reset_interrupt();
    // The band thread is not running here, so its failure memory is safe to
    // clear: whatever happens after an operator's start is news again.
    last_logged_failure_.clear();
    source_->set_retrying(false);
    if (!source_->start(error)) return false;
    // Whatever came before this start, minutes or hours ago, is not what
    // comes next: decoder channels time the new samples from scratch, as
    // after a module restart, rather than stamping them with a clock
    // anchored before the stop.
    {
        std::lock_guard<std::mutex> lock(taps_mutex_);
        for (const auto& tap : taps_) tap->reanchor();
    }
    source_online_.store(false);
    attempts_ = 0;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        status_ = source_->restartable() ? "starting" : "";
        offline_kind_ = SourceFailure::None;
    }

    running_.store(true);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& failure) {
        running_.store(false);
        source_->stop();
        error = failure.what();
        return false;
    }

    LOG_INFO("band", "%s started: %.4f - %.4f MHz, %.3f Msps %s, fft %zu, %.1f Hz bins, spectrum %zu @ %.0f/s",
             id_.c_str(), low_hz_ / 1e6, high_hz_ / 1e6, sample_rate_ / 1e6,
             signal_kind_ == SignalKind::Real ? "real" : "IQ", fft_size_, channelizer_->bin_hz(),
             spectrum_->bins(), spectrum_lines_per_second_);
    return true;
}

bool Band::restart() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (stopping_.load() || restarting_.load() || !on_air_.load()) return false;

    // The previous restart thread has finished by construction - restarting_
    // was false - but it still has to be joined before the handle is reused.
    if (restart_thread_.joinable()) restart_thread_.join();

    restarting_.store(true);
    try {
      restart_thread_ = std::thread([this] {
        LOG_INFO("band", "%s restarting at operator request", id_.c_str());
        stop_running();
        std::string error;
        if (!stopping_.load() && !start_running(error)) {
            LOG_ERROR("band", "%s failed to restart: %s", id_.c_str(), error.c_str());
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = error;
        } else {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_.clear();
        }
        restarting_.store(false);
      });
    } catch (const std::system_error& failure) {
        restarting_.store(false);
        std::lock_guard<std::mutex> error_lock(error_mutex_);
        last_error_ = failure.what();
        return false;
    }
    return true;
}

std::string Band::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool Band::healthy() const {
    if (online() || !on_air_.load()) return true;
    std::lock_guard<std::mutex> lock(error_mutex_);
    return running_.load() && offline_kind_ == SourceFailure::Disabled;
}

double Band::retry_in_seconds() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (next_attempt_ == std::chrono::steady_clock::time_point{} || !running_.load()) return -1.0;
    const double left = std::chrono::duration<double>(next_attempt_ - std::chrono::steady_clock::now()).count();
    return std::max(0.0, left);
}

std::string Band::status() const {
    if (!on_air_.load()) {
        const int64_t back = next_change_ms_.load();
        if (back < 0) return "off the air by its hours";
        const int64_t minute = back / 60000;
        char text[64];
        std::snprintf(text, sizeof text, "off the air until %02d:%02d UTC", static_cast<int>(minute / 60 % 24),
                      static_cast<int>(minute % 60));
        return text;
    }
    if (online()) return "";
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (!running_.load()) return last_error_.empty() ? "stopped" : "stopped: " + last_error_;
    return status_;
}

bool Band::supervise() {
    // Delays between attempts, in seconds, and then every 30 for as long as
    // it takes. A module that streamed for a minute starts from the top.
    static const int kDelays[] = {1, 2, 4, 8, 16, 30};
    const auto now = std::chrono::steady_clock::now();
    if (source_online_.load() && now - online_since_ >= std::chrono::minutes(1)) attempts_ = 0;
    source_online_.store(false);
    const SourceFailure kind = source_->failure_kind();
    std::string why = source_->failure();
    if (why.empty()) why = "the input stopped";
    // Stop the failed run now rather than after the wait, so a module does
    // not keep hold of its device meanwhile.
    source_->stop();

    const bool wait_for_operator = kind == SourceFailure::Operator || kind == SourceFailure::Disabled;
    const int delay = kDelays[std::min<size_t>(static_cast<size_t>(attempts_), std::size(kDelays) - 1)];
    attempts_++;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        offline_kind_ = kind;
        const bool sentence = !why.empty() && (why.back() == '.' || why.back() == '!' || why.back() == '?');
        if (kind == SourceFailure::Disabled) status_ = "switched off: " + why;
        else if (wait_for_operator) status_ = "waiting for the operator: " + why;
        else status_ = "offline: " + why + (sentence ? " " : ". ") + "Trying again in " + std::to_string(delay) + " s.";
        next_attempt_ = wait_for_operator ? std::chrono::steady_clock::time_point{}
                                          : std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    }
    if (why != last_logged_failure_) {
        if (wait_for_operator) LOG_WARN("band", "%s is offline until the operator acts: %s", id_.c_str(), why.c_str());
        else LOG_WARN("band", "%s is offline, trying again in %d s: %s", id_.c_str(), delay, why.c_str());
        last_logged_failure_ = why;
    } else {
        LOG_DEBUG("band", "%s still offline, trying again in %d s", id_.c_str(), delay);
    }

    {
        std::unique_lock<std::mutex> lock(supervision_mutex_);
        const auto stopping = [this] { return !running_.load() || shutdown_requested(); };
        if (wait_for_operator) supervision_wake_.wait(lock, stopping);
        else supervision_wake_.wait_for(lock, std::chrono::seconds(delay), stopping);
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        next_attempt_ = std::chrono::steady_clock::time_point{};
    }
    if (!running_.load() || shutdown_requested()) return false;
    std::string error;
    // Deliberately not reset_interrupt(): an interrupt that arrived during
    // the wait belongs to a stop that is joining this thread.
    source_->set_retrying(true);
    if (!source_->start(error)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        status_ = "offline: " + error;
    }
    return running_.load();
}

bool Band::reconfigure_source(const ConfigSection& section, std::string& error) {
    if (!source_->reconfigure(section, error)) return false;
    for (auto it = configured_.begin(); it != configured_.end();) {
        if (it->first.rfind("module.", 0) == 0) it = configured_.erase(it);
        else ++it;
    }
    for (const auto& [key, value] : section.values()) {
        if (key.rfind("module.", 0) == 0) configured_[key] = value;
    }
    return true;
}

void Band::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stopping_.store(true);
    // Only the public entry points acquire lifecycle_mutex_. The restart
    // worker uses the helpers, so joining it here cannot deadlock with stop.
    // Joining before closing the input also prevents it reviving a stopped band.
    if (restart_thread_.joinable()) restart_thread_.join();
    stop_running();
}

void Band::stop_running() {
    const bool was_running = running_.exchange(false);
    {
        // Taken and released so a supervisor between checking running_ and
        // starting to wait cannot miss this wake.
        std::lock_guard<std::mutex> lock(supervision_mutex_);
    }
    supervision_wake_.notify_all();
    source_->interrupt();
    if (thread_.joinable()) thread_.join();
    source_->stop();
    if (was_running) LOG_INFO("band", "%s stopped", id_.c_str());
}

void Band::add_listener(const std::shared_ptr<Listener>& listener) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_.push_back(listener);
}

void Band::remove_listener(uint64_t id) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [id](const std::shared_ptr<Listener>& l) { return l->id() == id; }),
                     listeners_.end());
}

std::shared_ptr<SharedFm> Band::share_fm(const FmKey& key) const {
    std::lock_guard<std::mutex> lock(fm_mutex_);
    for (const auto& weak : shared_fm_) {
        if (auto running = weak.lock(); running && running->key() == key) return running;
    }
    auto made = std::make_shared<SharedFm>(*this, key);
    shared_fm_.push_back(made);
    return made;
}

std::shared_ptr<SharedWaterfall> Band::share_waterfall(const WaterfallKey& key) const {
    std::lock_guard<std::mutex> lock(waterfall_mutex_);
    for (const auto& weak : shared_waterfalls_) {
        if (auto running = weak.lock(); running && running->key() == key) return running;
    }
    auto made = std::make_shared<SharedWaterfall>(*this, key);
    shared_waterfalls_.push_back(made);
    return made;
}

std::shared_ptr<ZoomSpectrum> Band::share_zoom(const ViewportSettings& viewport) const {
    return zoom_bank_->share(viewport.low_hz, viewport.high_hz, viewport.width);
}

size_t Band::zoom_count() const {
    return zoom_bank_->zoom_count();
}

size_t Band::zoom_tile_count() const {
    return zoom_bank_->tile_count();
}

size_t Band::shared_waterfall_count() const {
    std::lock_guard<std::mutex> lock(waterfall_mutex_);
    return static_cast<size_t>(std::count_if(shared_waterfalls_.begin(), shared_waterfalls_.end(),
                                             [](const auto& weak) { return !weak.expired(); }));
}

size_t Band::shared_fm_count() const {
    std::lock_guard<std::mutex> lock(fm_mutex_);
    return static_cast<size_t>(std::count_if(shared_fm_.begin(), shared_fm_.end(),
                                              [](const std::weak_ptr<SharedFm>& weak) { return !weak.expired(); }));
}

std::shared_ptr<DecoderTap> Band::add_decoder_tap(uint16_t index, double dial_hz, double offset_hz, double width_hz) {
    auto tap = std::make_shared<DecoderTap>(index, *channelizer_, spectrum_origin_hz_, rf_scale_, dial_hz, offset_hz,
                                            width_hz);
    std::lock_guard<std::mutex> lock(taps_mutex_);
    taps_.push_back(tap);
    return tap;
}

void Band::remove_decoder_tap(const std::shared_ptr<DecoderTap>& tap) {
    std::lock_guard<std::mutex> lock(taps_mutex_);
    taps_.erase(std::remove(taps_.begin(), taps_.end(), tap), taps_.end());
}

size_t Band::decoder_tap_count() const {
    std::lock_guard<std::mutex> lock(taps_mutex_);
    return taps_.size();
}

int Band::listener_count() const {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    return static_cast<int>(listeners_.size());
}

BandInfo Band::info() const {
    BandInfo out;
    out.id = id_;
    out.name = name_;
    out.center_hz = center_hz_;
    out.sample_rate = sample_rate_;
    out.low_hz = low_hz();
    out.high_hz = high_hz();
    out.spectrum_bins = spectrum_bins_;
    out.max_bandwidth_hz = max_bandwidth_hz_;
    out.max_user_bitrate = max_user_bitrate_;
    out.noise_blanker = blanker_.strength();
    out.iq_swap = conditioner_.swap();
    out.dc_remove = conditioner_.dc_remove();
    out.iq_balance = conditioner_.balance();
    out.dc_offset_dbfs = conditioner_.dc_offset_dbfs();
    out.gain_error_db = conditioner_.gain_error_db();
    out.phase_error_degrees = conditioner_.phase_error_degrees();
    out.image_rejection_db = conditioner_.image_rejection_db();
    out.ppm = (rf_scale_ - 1.0) * 1e6;
    out.blanked_fraction = blanker_.blanked_fraction();
    out.noise_floor_dbfs = noise_floor_dbfs_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        out.calibration = calibration_.points();
    }
    {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        out.history_access = history_access_;
        if (archive_.is_open()) {
            out.history_bytes = archive_.size_bytes();
            out.history_oldest_ms = archive_.oldest_ms();
            out.history_newest_ms = archive_.newest_ms();
        }
    }
    out.listeners = listener_count();
    out.running = running_.load();
    out.online = online();
    out.status = status();
    out.on_air = on_air_.load();
    out.next_change_ms = next_change_ms_.load();
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        out.hours = hours_text_;
    }
    out.source_kind = source_->kind_name();
    out.signal = signal_kind_ == SignalKind::Real ? "real" : "iq";
    return out;
}

void Band::ensure_block_buffers() {
    if (!block_input_[0].empty() || !block_real_[0].empty()) return;
    // Zeroed: before the first block the older half is silence.
    for (size_t i = 0; i < 2; i++) {
        if (signal_kind_ == SignalKind::Real) block_real_[i].assign(channelizer_->block_size(), 0.0f);
        else block_input_[i].assign(channelizer_->block_size(), cfloat(0.0f, 0.0f));
    }
    spectrum_low_hz_ = spectrum_low_hz();
    spectrum_high_hz_ = spectrum_high_hz();
}

bool Band::process_one_block() {
    return advance_block(false);
}

void Band::finish_listeners() {
    auto job = std::move(listener_job_);
    try {
        if (job) job->wait();
    } catch (...) {
        listener_snapshot_.clear();
        throw;
    }
    listener_snapshot_.clear();
}

void Band::channelize(size_t now) {
    const size_t before = now ^ 1;
    if (signal_kind_ == SignalKind::Real)
        channelizer_->process_real_pair(block_real_[before].data(), block_real_[now].data());
    else
        channelizer_->process_pair(block_input_[before].data(), block_input_[now].data());
}

bool Band::advance_block(bool overlap) {
    ensure_block_buffers();

    // The previous block stays as it was: the channelizer reads it below.
    DspVector<float>& real = block_real_[block_current_];
    DspVector<cfloat>& input = block_input_[block_current_];
    if (signal_kind_ == SignalKind::Real) {
        if (!source_->read_real(real.data(), real.size())) {
            finish_listeners();
            return false;
        }
        conditioner_.process_real(real.data(), real.size());
        blanker_.process_real(real.data(), real.size());
    } else {
        if (!source_->read(input.data(), input.size())) {
            finish_listeners();
            return false;
        }
        // The front end's own flaws first, so the blanker, the spectrum and
        // every listener work on what the antenna delivered.
        conditioner_.process(input.data(), input.size());
        blanker_.process(input.data(), input.size());
    }
    // A block that does not follow the last, after a FIFO producer came
    // back: decoder channels drop a frame half built from before and time
    // what comes now from scratch, as after a restart.
    if (source_->take_discontinuity()) {
        std::lock_guard<std::mutex> lock(taps_mutex_);
        for (const auto& tap : taps_) tap->reanchor();
    }
    // When the block's last sample arrived, as near as this thread can say:
    // what decoder channels are timed by.
    const int64_t arrived_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const auto channel = [](void* context) {
        auto& band = *static_cast<Band*>(context);
        band.channelize(band.block_current_);
    };
    const auto spectrum = [](void* context) {
        auto& band = *static_cast<Band*>(context);
        const size_t now = band.block_current_;
        if (band.signal_kind_ == SignalKind::Real)
            band.spectrum_->push_real(band.block_real_[now].data(), band.block_real_[now].size());
        else
            band.spectrum_->push(band.block_input_[now].data(), band.block_input_[now].size());
    };
    // Only listeners read the channelizer's spectrum, and on an idle band its
    // transform is a third of the work. It is left out while nobody listens;
    // a listener that arrives before the snapshot below gets it done late,
    // from the same two blocks, before anything reads it.
    bool channelized = false;
    {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        channelized = !listeners_.empty();
    }
    {
        std::lock_guard<std::mutex> lock(taps_mutex_);
        tap_snapshot_ = taps_;
    }
    channelized = channelized || !tap_snapshot_.empty();
    // Both transforms read this immutable, already blanked block, with their
    // own windows and history. The barrier precedes every spectrum consumer
    // and the next source read, so offloading adds no block of latency.
    if (!channelized) spectrum(this);
    else if (dsp_workers_ && fft_size_ >= (1u << 18))
        dsp_workers_->parallel_pair(channel, spectrum, this);
    else { channel(this); spectrum(this); }
    // Decoder channels read the channelizer as it stands now, before anything
    // else touches it; each is a slice and a short inverse transform.
    for (const auto& tap : tap_snapshot_) tap->process(*channelizer_, arrived_us);
    tap_snapshot_.clear();
    // The block just read is the next one's older half.
    block_current_ ^= 1;
    // The producer only touched its own FFT and analyzer storage above. Join
    // the old consumers before reusing their bins, listener list or pyramid.
    finish_listeners();
    // Build the mipmap once for the whole band.  Every listener then reads
    // from it at whatever resolution its zoom needs, instead of each one
    // scanning the full-resolution line.
    bool have_line = false;
    {
        std::lock_guard<std::mutex> lock(spectrum_mutex_);
        have_line = spectrum_->take_line(spectrum_line_);
    }
    if (have_line) {
        record_recent();
        pyramid_.build(spectrum_line_.data(), spectrum_line_.size(), spectrum_low_hz_,
                       spectrum_high_hz_);
        if (previous_line_.size() == spectrum_line_.size()) {
            paired_line_.resize(spectrum_line_.size());
            for (size_t i = 0; i < spectrum_line_.size(); i++) {
                paired_line_[i] = 0.5f * (previous_line_[i] + spectrum_line_[i]);
            }
            paired_pyramid_.build(paired_line_.data(), paired_line_.size(), spectrum_low_hz_, spectrum_high_hz_);
        } else {
            paired_pyramid_.build(spectrum_line_.data(), spectrum_line_.size(), spectrum_low_hz_,
                                  spectrum_high_hz_);
        }
        previous_line_ = spectrum_line_;
        // The floor is what every other level on this band is judged against,
        // so it is measured from the same line the waterfall is drawn from
        // rather than from a separate estimate that could disagree with it.
        noise_floor_.update(spectrum_line_.data(), spectrum_line_.size(), spectrum_->last_line_seconds());
        noise_floor_dbfs_.store(noise_floor_.dbfs(), std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(archive_mutex_);
        if (history_access_ != "off") {
            // Wall-clock, because an archive is read by time of day and has to
            // survive a restart; a monotonic clock cannot say when yesterday
            // evening was. The archive itself decides whether a line is due.
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            archive_.append(spectrum_line_.data(), spectrum_line_.size(), now_ms);
        }
    }

    // Copy the listener list rather than holding the lock through the DSP: a
    // client connecting must never stall audio for everyone else.
    {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listener_snapshot_ = listeners_;
    }
    if (!channelized) {
        // block_current_ was flipped above, so the block just read is the
        // other one.
        if (listener_snapshot_.empty()) channelizer_->skip_block();
        else channelize(block_current_ ^ 1);
    }
    // Nobody listening and no archive to keep: the noise floor and the admin
    // panel's row every kRecentIntervalMs are all that read the lines, so
    // two lines in each interval are made, and every line again from the
    // next block on once a listener arrives. Two rather than one, because
    // the admin's rows are due on the wall clock and lines arrive on the
    // input's: one line an interval would sometimes land just before its row
    // was due and leave a gap of two intervals.
    bool history_on = false;
    {
        std::lock_guard<std::mutex> lock(archive_mutex_);
        history_on = history_access_ != "off";
    }
    const int idle_stride =
        std::max(1, static_cast<int>(spectrum_lines_per_second_ * kRecentIntervalMs / 2000.0));
    spectrum_->set_stride(listener_snapshot_.empty() && !history_on ? idle_stride : 1);

    // Broadcast FM once per station, before the listeners who copy it. Their
    // jobs from the last block were joined above, so nobody still reads the
    // audio this replaces.
    {
        std::lock_guard<std::mutex> lock(fm_mutex_);
        fm_snapshot_.clear();
        for (auto it = shared_fm_.begin(); it != shared_fm_.end();) {
            if (auto running = it->lock()) {
                fm_snapshot_.push_back(std::move(running));
                ++it;
            } else {
                it = shared_fm_.erase(it);
            }
        }
    }
    if (!fm_snapshot_.empty()) {
        const ChannelBlock block = channelizer_->current_block();
        for (const auto& fm : fm_snapshot_) fm->process(block);
        fm_snapshot_.clear();
    }

    // Zoomed views' spectra, before the rows and listeners that draw them.
    zoom_bank_->process(channelizer_->current_block(), dsp_workers_);

    // Waterfall rows once per view, on the same terms.
    {
        std::lock_guard<std::mutex> lock(waterfall_mutex_);
        waterfall_snapshot_.clear();
        for (auto it = shared_waterfalls_.begin(); it != shared_waterfalls_.end();) {
            if (auto running = it->lock()) {
                waterfall_snapshot_.push_back(std::move(running));
                ++it;
            } else {
                it = shared_waterfalls_.erase(it);
            }
        }
    }
    if (!waterfall_snapshot_.empty()) {
        const double block_seconds = channelizer_->current_block().block_seconds();
        for (const auto& waterfall : waterfall_snapshot_) {
            waterfall->process(block_seconds, have_line ? &pyramid_ : nullptr, have_line ? &paired_pyramid_ : nullptr);
        }
        waterfall_snapshot_.clear();
    }

    listener_line_ = have_line;
    const auto process = [](void* context, size_t index) {
        auto& band = *static_cast<Band*>(context);
        band.listener_snapshot_[index]->process_block(band.listener_block_,
            band.listener_line_ ? &band.pyramid_ : nullptr, band.listener_line_ ? &band.paired_pyramid_ : nullptr);
    };
    if (overlap && dsp_workers_ && dsp_workers_->has_workers() &&
        fft_size_ >= (1u << 18) && listener_snapshot_.size() >= 64) {
        listener_spectrum_re_.resize(fft_size_, 0.0f);
        listener_spectrum_im_.resize(fft_size_, 0.0f);
        listener_block_ = channelizer_->take_block(listener_spectrum_re_, listener_spectrum_im_);
        listener_job_ = dsp_workers_->start(listener_snapshot_.size(), process, this,
            [](void* context, bool failed) {
                auto& band = *static_cast<Band*>(context);
                // No consumer can still use the snapshot here. Release it
                // even if the next source read never produces another block.
                band.listener_snapshot_.clear();
                // A producer may already be blocked waiting for the next
                // input. Publish completed audio now, and unblock that read
                // on failure so wait() can report the worker's exception.
                if (failed) band.source_->interrupt();
                try { if (band.wake_) band.wake_(); }
                catch (...) { band.source_->interrupt(); throw; }
            });
        return true;
    }

    listener_block_ = channelizer_->current_block();
    try {
        if (dsp_workers_) dsp_workers_->run(listener_snapshot_.size(), process, this);
        else for (size_t i = 0; i < listener_snapshot_.size(); i++) process(this, i);
    } catch (...) {
        // The pool has joined this batch before rethrowing. Release clients
        // here too, or a stopped band pins them after they disconnect.
        listener_snapshot_.clear();
        throw;
    }
    listener_snapshot_.clear();
    return true;
}

bool Band::set_history(const std::string& access, int hours, size_t width, double interval,
                       std::string& error) {
    if (access != "off" && access != "private" && access != "public") {
        error = "history must be off, private or public";
        return false;
    }
    std::lock_guard<std::mutex> lock(archive_mutex_);
    if (history_path_.empty()) history_path_ = "fernsdr-history-" + id_ + ".wfa";
    if (access == "off") {
        archive_.close();
        history_deferred_ = false;
        history_access_ = "off";
        LOG_INFO("band", "%s history off", id_.c_str());
        return true;
    }

    // Only reopen when the shape actually changed. Reopening a ring of a
    // different shape starts the record again, and doing that because
    // somebody flipped private to public would throw away the archive for no
    // reason at all.
    const bool reshaped = !archive_.is_open() || hours != history_hours_ ||
                          width != history_bins_ || interval != history_interval_;
    if (reshaped && !archive_.open(history_path_, width, interval, hours, error, spectrum_low_hz(), spectrum_high_hz())) {
        return false;
    }

    // What this said replaces what the configuration asked start() to open;
    // refused, the configured archive still opens.
    history_deferred_ = false;
    history_hours_ = hours;
    history_bins_ = width;
    history_interval_ = interval;
    history_access_ = access;
    LOG_INFO("band", "%s history %s: %d hours, %zu bins, %.1f s per line", id_.c_str(),
             access.c_str(), hours, width, interval);
    return true;
}

bool Band::spectrum_snapshot(size_t bins, std::vector<float>& levels, double& low_hz, double& high_hz) const {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    if (spectrum_line_.empty() || bins == 0) return false;
    const size_t source = spectrum_line_.size();
    bins = std::min(bins, source);
    levels.assign(bins, -160.0f);
    for (size_t i = 0; i < bins; i++) {
        const size_t first = i * source / bins;
        const size_t last = std::max(first + 1, (i + 1) * source / bins);
        float peak = -160.0f;
        for (size_t j = first; j < last; j++) peak = std::max(peak, spectrum_line_[j]);
        levels[i] = peak;
    }
    low_hz = spectrum_low_hz_;
    high_hz = spectrum_high_hz_;
    return true;
}

void Band::record_recent() {
    const auto now = std::chrono::steady_clock::now();
    if (now < recent_due_) return;
    // On the schedule rather than half a second after the last line, so the
    // rows stay evenly spaced in time; a stall does not bunch them up after.
    recent_due_ = std::max(recent_due_ + std::chrono::milliseconds(kRecentIntervalMs), now);
    const size_t source = spectrum_line_.size();
    if (source == 0) return;
    // Reduced outside the lock: the band thread is the only writer of the
    // line, and an HTTP thread reading the ring should not wait on this.
    recent_line_.resize(kRecentBins);
    for (size_t i = 0; i < kRecentBins; i++) {
        const size_t first = std::min(source - 1, i * source / kRecentBins);
        const size_t last = std::max(first + 1, (i + 1) * source / kRecentBins);
        float peak = -160.0f;
        for (size_t j = first; j < last && j < source; j++) peak = std::max(peak, spectrum_line_[j]);
        recent_line_[i] = WaterfallArchive::quantise(peak);
    }
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    recent_.push(recent_line_.data());
    recent_taken_ = now;
}

size_t Band::recent_spectrum(std::vector<uint8_t>& rows, int64_t& age_ms) const {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    const size_t count = recent_.copy_oldest_first(rows);
    age_ms = count == 0 ? -1
                        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                 recent_taken_)
                              .count();
    return count;
}

bool Band::find_carrier(double hz, double span_hz, double& found_hz, float& level_dbfs) const {
    if (!std::isfinite(hz) || !std::isfinite(span_hz) || span_hz <= 0.0) return false;
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    const size_t bins = spectrum_line_.size();
    const double span = spectrum_high_hz_ - spectrum_low_hz_;
    if (bins < 3 || span <= 0.0) return false;
    const double width = span / static_cast<double>(bins);
    // Cell p is centred at low + (p + 0.5) * width.
    const double first = (hz - span_hz / 2.0 - spectrum_low_hz_) / width - 0.5;
    const double last = (hz + span_hz / 2.0 - spectrum_low_hz_) / width - 0.5;
    if (last < 1.0 || first > static_cast<double>(bins - 2)) return false;
    const size_t from = static_cast<size_t>(std::max(1.0, std::ceil(first)));
    const size_t to = static_cast<size_t>(std::min(static_cast<double>(bins - 2), std::floor(last)));
    if (from > to) return false;
    size_t peak = from;
    for (size_t i = from + 1; i <= to; i++) {
        if (spectrum_line_[i] > spectrum_line_[peak]) peak = i;
    }
    const double left = spectrum_line_[peak - 1], centre = spectrum_line_[peak], right = spectrum_line_[peak + 1];
    const double curvature = left - 2.0 * centre + right;
    const double shift = curvature < 0.0 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
    found_hz = spectrum_low_hz_ + (static_cast<double>(peak) + 0.5 + shift) * width;
    level_dbfs = static_cast<float>(centre - 0.25 * (left - right) * shift);
    return true;
}

float Band::peak_dbfs(double hz, double width_hz) const {
    if (!std::isfinite(hz) || !std::isfinite(width_hz) || width_hz <= 0) return -160.0f;
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    if (spectrum_line_.empty()) return -160.0f;

    const double span = spectrum_high_hz_ - spectrum_low_hz_;
    if (span <= 0.0) return -160.0f;
    const double bins = static_cast<double>(spectrum_line_.size());
    const double half = width_hz / 2.0;
    const double low = std::max(hz - half, spectrum_low_hz_);
    const double high = std::min(hz + half, spectrum_high_hz_);
    if (low > high) return -160.0f;
    // Intersect in frequency space before converting to indices. Even a
    // finite query can be far outside the range of an integer bin index.
    const size_t first = static_cast<size_t>(std::clamp((low - spectrum_low_hz_) / span * bins, 0.0, bins - 1));
    const size_t last = static_cast<size_t>(std::clamp((high - spectrum_low_hz_) / span * bins, 0.0, bins - 1));

    float peak = -160.0f;
    for (size_t i = first; i <= last; i++) {
        peak = std::max(peak, spectrum_line_[i]);
    }
    return peak;
}

void Band::run() {
    std::exception_ptr error;
    try {
        while (running_.load()) {
            if (!advance_block(true)) {
                if (!running_.load()) break;
                if (!source_->restartable()) {
                    LOG_WARN("band", "%s source ended", id_.c_str());
                    break;
                }
                if (!supervise()) break;
                // The samples after a restart are not a continuation: decoder
                // channels time them from scratch and say so.
                {
                    std::lock_guard<std::mutex> lock(taps_mutex_);
                    for (const auto& tap : taps_) tap->reanchor();
                }
                continue;
            }
            if (source_->restartable() && !source_online_.load()) {
                source_online_.store(true);
                last_logged_failure_.clear();
                online_since_ = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(error_mutex_);
                status_.clear();
                offline_kind_ = SourceFailure::None;
                LOG_INFO("band", "%s is receiving", id_.c_str());
            }
            if (!listener_job_ && wake_) wake_();
        }
    } catch (...) { error = std::current_exception(); }
    // A source or transform can fail while consumers still own the previous
    // block. Drain them before publishing stopped or allowing a restart.
    try { finish_listeners(); }
    catch (...) { if (!error) error = std::current_exception(); }
    if (error) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        try { std::rethrow_exception(error); }
        catch (const std::exception& failure) { last_error_ = failure.what(); }
        catch (...) { last_error_ = "unknown DSP worker failure"; }
        LOG_ERROR("band", "%s stopped: %s", id_.c_str(), last_error_.c_str());
    }
    // A module left running would sit blocked on a full pipe, holding its
    // USB device from every other program until someone restarted the band.
    if (error && source_->restartable()) source_->stop();
    running_.store(false);
}

}  // namespace fernsdr
