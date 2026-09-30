#include "source_module.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "../core/module_store.h"
#include "../util/log.h"
#include "../util/subprocess.h"
#include "../util/utf8.h"

namespace fernsdr {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxEventLine = 64 * 1024;
constexpr size_t kMaxLogLine = 1024;
constexpr size_t kLogLinesKept = 200;
constexpr int kLogLinesPerSecond = 10;
constexpr size_t kMaxOutbox = 64 * 1024;
constexpr size_t kReadChunk = 64 * 1024;

// What each documented exit status means, in the operator's terms.
const char* exit_meaning(int code) {
    switch (code) {
        case 0: return "it stopped by itself";
        case 2: return "its command line was wrong";
        case 3: return "no device matched its settings";
        case 4: return "the device is in use by another program";
        case 5: return "USB failed or the device was unplugged";
        case 6: return "its settings were invalid";
        default: return nullptr;
    }
}

constexpr size_t kMaxReportedKeys = 64;

// A flat object of short scalar values, which is all a device description or
// a list of effective settings should be.
Json flat_object(const Json& value) {
    Json out = Json::make_object();
    if (!value.is_object()) return out;
    size_t kept = 0;
    for (const auto& [key, entry] : value.members()) {
        if (++kept > kMaxReportedKeys) break;
        const std::string name = printable(key, 64);
        if (entry.is_string()) out.set(name, printable(entry.string(), 200));
        else if (entry.is_number() || entry.is_bool()) out.set(name, entry);
    }
    return out;
}

// A count a module reports, as a count: never negative, and never a double
// too large for the integer it goes into, where the conversion is undefined.
uint64_t reported_count(const Json& value) {
    const double number = value.number(0.0);
    if (!(number > 0)) return 0;
    return static_cast<uint64_t>(std::min(number, 9007199254740992.0));
}

class ModuleSource : public Source {
public:
    ModuleSource(std::string band, std::string id, SignalKind signal, double sample_rate, double center,
                 std::map<std::string, std::string> settings, std::shared_ptr<ModuleStore> store,
                 ModuleTiming timing)
        : band_(std::move(band)),
          id_(std::move(id)),
          signal_(signal),
          sample_rate_(sample_rate),
          center_hz_(center),
          store_(std::move(store)),
          timing_(timing),
          raw_settings_(std::move(settings)) {}

    ~ModuleSource() override { finish(true); }

    bool start(std::string& error) override {
        (void)error;
        // Nothing here can fail. Starting the program, and everything that can
        // go wrong with it, happens on the band's own thread at the first
        // read, so a dongle that is not plugged in leaves one band offline
        // rather than stopping the receiver from starting.
        finish(true);
        phase_ = Phase::Idle;
        failure_.clear();
        failure_kind_ = SourceFailure::None;
        fatal_code_.clear();
        fatal_message_.clear();
        partial_event_.clear();
        partial_log_.clear();
        module_ended_ = false;
        have_exit_ = false;
        std::lock_guard<std::mutex> lock(status_mutex_);
        state_ = "starting";
        outbox_.clear();
        return true;
    }

    void interrupt() override { interrupted_.store(true); }
    void reset_interrupt() override { interrupted_.store(false); }
    void stop() override { finish(true); }

    SignalKind kind() const override { return signal_; }
    double sample_rate() const override { return sample_rate_; }
    double center_hz() const override { return center_hz_; }
    const char* kind_name() const override { return "module"; }
    bool restartable() const override { return true; }
    void set_retrying(bool quietly) override { quiet_.store(quietly); }
    std::string failure() const override { return failure_; }
    SourceFailure failure_kind() const override { return failure_kind_; }

    SourceStats stats() const override {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return {samples_, module_dropped_, state_ == "streaming"};
    }

    bool read(cfloat* out, size_t count) override {
        if (signal_ != SignalKind::Iq || !streaming()) return false;
        if (!fill(count * bytes_per_sample(format_, signal_))) return false;
        convert_iq(format_, raw_.data(), out, count);
        count_samples(count);
        return true;
    }

    bool read_real(float* out, size_t count) override {
        if (signal_ != SignalKind::Real || !streaming()) return false;
        if (!fill(count * bytes_per_sample(format_, signal_))) return false;
        convert_real(format_, raw_.data(), out, count);
        count_samples(count);
        return true;
    }

    Json describe() const override {
        std::lock_guard<std::mutex> lock(status_mutex_);
        Json out = Json::make_object();
        out.set("module", id_);
        out.set("version", version_);
        out.set("state", state_);
        if (pid_ > 0) out.set("pid", static_cast<double>(pid_));
        if (device_.is_object()) out.set("device", device_);
        if (effective_.is_object()) out.set("settings", effective_);
        if (state_ == "streaming") out.set("center", tuned_hz_);
        out.set("samples", static_cast<double>(samples_));
        out.set("module_samples", static_cast<double>(module_samples_));
        out.set("dropped", static_cast<double>(module_dropped_));
        if (has_clipping_) out.set("clipping", clipping_);
        if (std::isfinite(module_gain_)) out.set("gain_now", module_gain_);
        if (!failure_text_.empty()) out.set("failure", failure_text_);
        if (!exit_text_.empty()) out.set("exit", exit_text_);
        Json live = Json::make_array();
        for (const ModuleSetting& setting : running_manifest_.settings) {
            if (!setting.live) continue;
            Json entry = Json::make_object();
            entry.set("key", setting.key);
            entry.set("type", setting.type);
            entry.set("label", setting.label);
            if (!setting.unit.empty()) entry.set("unit", setting.unit);
            if (setting.has_min) entry.set("min", setting.min);
            if (setting.has_max) entry.set("max", setting.max);
            if (!setting.choices.empty()) {
                Json choices = Json::make_array();
                for (const std::string& choice : setting.choices) choices.push_back(choice);
                entry.set("choices", choices);
            }
            live.push_back(entry);
        }
        out.set("live", live);
        if (last_set_.is_object()) out.set("last_set", last_set_);
        Json log = Json::make_array();
        for (const std::string& line : log_) log.push_back(line);
        out.set("log", log);
        return out;
    }

    bool apply_live(const Json& settings, Json& result, std::string& error) override {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (state_ != "streaming") {
            error = id_ + " is not running";
            return false;
        }
        if (!settings.is_object() || settings.members().empty()) {
            error = "no settings were given";
            return false;
        }
        Json checked = Json::make_object();
        for (const auto& [key, value] : settings.members()) {
            const ModuleSetting* setting = running_manifest_.setting(key);
            if (!setting) {
                error = id_ + " has no setting '" + key + "'";
                return false;
            }
            if (!setting->live) {
                error = "'" + key + "' cannot change while " + id_ +
                        " runs; change it in the configuration and restart the band";
                return false;
            }
            Json typed;
            if (!check_module_setting(*setting, value, typed, error)) return false;
            checked.set(key, typed);
        }
        const double number = static_cast<double>(next_set_++);
        Json command = Json::make_object();
        command.set("type", "set");
        command.set("id", number);
        command.set("settings", checked);
        const std::string line = command.serialize() + "\n";
        if (outbox_.size() + line.size() > kMaxOutbox) {
            error = id_ + " is not reading its commands";
            return false;
        }
        outbox_ += line;
        result = Json::make_object();
        result.set("id", number);
        result.set("settings", checked);
        return true;
    }

    bool reconfigure(const ConfigSection& section, std::string& error) override {
        std::map<std::string, std::string> settings;
        if (!collect_settings(section, settings, error)) return false;
        std::lock_guard<std::mutex> lock(status_mutex_);
        raw_settings_ = std::move(settings);
        return true;
    }

    // The `module.<key>` values of a band section, without the prefix.
    static bool collect_settings(const ConfigSection& section, std::map<std::string, std::string>& out,
                                 std::string& error) {
        for (const auto& [key, value] : section.values()) {
            if (key.rfind("module.", 0) != 0) continue;
            const std::string name = key.substr(7);
            if (name.empty() || name == "sample_rate" || name == "center" || name == "signal") {
                error = "[" + section.name() + "] " + key + " is not a module setting: " +
                        (name.empty() ? std::string("it has no name") : "the band's own " + name + " is used");
                return false;
            }
            out[name] = value;
        }
        return true;
    }

private:
    enum class Phase { Idle, Hello, Opening, Streaming, Failed };

    bool streaming() {
        if (phase_ == Phase::Streaming) return true;
        if (phase_ != Phase::Idle) return false;
        return launch();
    }

    void count_samples(size_t count) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        samples_ += count;
    }

    // Records why this run is over. The process itself is ended by
    // conclude(), never here: this is called from inside the loop that is
    // still reading its pipes.
    void mark_failed(SourceFailure kind, const std::string& message) {
        if (phase_ == Phase::Failed) return;
        failure_kind_ = kind;
        failure_ = message;
        phase_ = Phase::Failed;
    }

    void queue_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        outbox_ += line;
        outbox_ += '\n';
    }

    bool launch() {
        ModuleStore::Launch launch;
        std::string error;
        if (!store_) {
            mark_failed(SourceFailure::Operator, "this receiver has no module directory");
            conclude();
            return false;
        }
        switch (store_->acquire(id_, launch, error)) {
            case ModuleStore::Resolve::Ok: break;
            case ModuleStore::Resolve::Disabled:
                mark_failed(SourceFailure::Disabled, error);
                conclude();
                return false;
            default:
                mark_failed(SourceFailure::Operator, error);
                conclude();
                return false;
        }
        lease_version_ = launch.version;
        if (launch.manifest.kind != "input") {
            store_->release(id_, launch.version);
            lease_version_.clear();
            mark_failed(SourceFailure::Operator, id_ + " is a " + launch.manifest.kind +
                                                     " module; a band's source has to be an input module");
            conclude();
            return false;
        }

        std::map<std::string, std::string> raw;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            raw = raw_settings_;
            version_ = launch.version;
            running_manifest_ = launch.manifest;
            device_ = Json();
            effective_ = Json();
            last_set_ = Json();
            failure_text_.clear();
            exit_text_.clear();
            module_samples_ = 0;
            module_dropped_ = 0;
            has_clipping_ = false;
            clipping_ = 0.0;
            module_gain_ = NAN;
        }
        Json settings = Json::make_object();
        for (const auto& [key, text] : raw) {
            const ModuleSetting* setting = launch.manifest.setting(key);
            if (!setting) {
                mark_failed(SourceFailure::Operator, id_ + " " + launch.version + " has no setting '" + key +
                                                         "'; remove module." + key + " from [band:" + band_ + "]");
                conclude();
                return false;
            }
            Json value;
            if (!type_module_setting(*setting, text, value, error)) {
                mark_failed(SourceFailure::Operator, "[band:" + band_ + "] module." + error);
                conclude();
                return false;
            }
            settings.set(key, value);
        }

        Subprocess::Options options;
        options.path = launch.executable;
        options.arguments = {"--fernsdr-module", "1"};
        options.environment = module_environment();
        options.streams = {Subprocess::Stream::ToChild, Subprocess::Stream::FromChild, Subprocess::Stream::FromChild,
                           Subprocess::Stream::FromChild};
        process_ = std::make_unique<Subprocess>();
        if (!process_->start(options, error)) {
            const int reason = errno;
            process_.reset();
            // A program that cannot be executed at all, for the wrong
            // architecture or from a filesystem mounted noexec, will not
            // start on the next attempt either. Running out of memory,
            // processes or descriptors is a moment, not a setting.
            const bool passing = reason == EAGAIN || reason == ENOMEM || reason == EMFILE || reason == ENFILE;
            mark_failed(passing ? SourceFailure::Transient : SourceFailure::Operator, error);
            conclude();
            return false;
        }
        // A deeper pipe rides out a stall in the DSP without the module's own
        // buffer overflowing. The system may cap it; the default still works.
        ::fcntl(process_->fd(1), F_SETPIPE_SZ, 1 << 20);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            pid_ = process_->pid();
            state_ = "starting";
        }
        log_write(routine(), "band", "%s: started %s %s (pid %d)", band_.c_str(), id_.c_str(), launch.version.c_str(),
                  static_cast<int>(process_->pid()));

        phase_ = Phase::Hello;
        const auto hello_by = Clock::now() + timing_.hello;
        size_t unused = 0;
        while (phase_ == Phase::Hello) {
            if (interrupted_.load()) return abandon();
            if (Clock::now() >= hello_by) {
                mark_failed(SourceFailure::Transient,
                            id_ + " did not introduce itself within " + seconds(timing_.hello));
                break;
            }
            service(50, nullptr, 0, unused);
        }
        if (phase_ == Phase::Opening) {
            Json open = Json::make_object();
            open.set("type", "open");
            open.set("sample_rate", sample_rate_);
            open.set("center", center_hz_);
            open.set("signal", signal_ == SignalKind::Iq ? "iq" : "real");
            open.set("settings", settings);
            queue_line(open.serialize());
            const auto ready_by = Clock::now() + timing_.ready;
            while (phase_ == Phase::Opening) {
                if (interrupted_.load()) return abandon();
                if (Clock::now() >= ready_by) {
                    mark_failed(SourceFailure::Transient,
                                id_ + " did not become ready within " + seconds(timing_.ready));
                    break;
                }
                service(50, nullptr, 0, unused);
            }
        }
        if (phase_ != Phase::Streaming) {
            conclude();
            return false;
        }
        LOG_INFO("band", "%s: %s %s is streaming", band_.c_str(), id_.c_str(), launch.version.c_str());
        return true;
    }

    static std::string seconds(std::chrono::milliseconds duration) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3g s", duration.count() / 1000.0);
        return text;
    }

    // The band is being stopped: end the process without calling it a failure.
    bool abandon() {
        finish(true);
        phase_ = Phase::Idle;
        return false;
    }

    bool fill(size_t bytes) {
        raw_.resize(bytes);
        size_t got = 0;
        // Measured from the start of this wait, not from the last sample: a
        // band thread that was busy elsewhere for a while has not seen the
        // module stall.
        auto progress = Clock::now();
        while (got < bytes) {
            if (interrupted_.load()) return false;
            const size_t before = got;
            const auto waited = Clock::now() - progress;
            if (waited >= timing_.stall) {
                mark_failed(SourceFailure::Transient,
                            id_ + " stopped delivering samples: nothing arrived for " + seconds(timing_.stall));
                conclude();
                return false;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(timing_.stall - waited).count();
            service(static_cast<int>(std::clamp<long long>(left, 1, 100)), raw_.data(), bytes, got);
            if (phase_ == Phase::Failed) {
                conclude();
                return false;
            }
            if (got > before) progress = Clock::now();
        }
        return true;
    }

    // One round of waiting on the module: samples into `samples` (up to
    // `want` bytes, counting in `got`), events, log lines, and commands out.
    void service(int timeout_ms, uint8_t* samples, size_t want, size_t& got) {
        if (!process_) return;
        pollfd descriptors[4];
        int kinds[4];
        nfds_t used = 0;
        bool have_output;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            have_output = !outbox_.empty();
        }
        auto watch = [&](int child_fd, short events) {
            const int fd = process_->fd(static_cast<size_t>(child_fd));
            if (fd < 0) return;
            descriptors[used] = {fd, events, 0};
            kinds[used] = child_fd;
            used++;
        };
        // Events first. Two pipes keep no order between them, so a module that
        // wrote `ready` and then its first samples can have both waiting in
        // the same round, and the samples must not be read as coming first.
        watch(3, POLLIN);
        watch(2, POLLIN);
        if (have_output) watch(0, POLLOUT);
        // Before `ready` samples are not wanted, but a module that ends then
        // shows it here first.
        watch(1, POLLIN);
        if (used == 0) {
            module_ended_ = true;
            mark_failed(SourceFailure::Transient, id_ + " closed its pipes");
            return;
        }
        const int ready = ::poll(descriptors, used, timeout_ms);
        if (ready <= 0) return;
        for (nfds_t i = 0; i < used && phase_ != Phase::Failed; i++) {
            if (!descriptors[i].revents) continue;
            switch (kinds[i]) {
                case 0: write_commands(); break;
                case 1: read_samples(samples, want, got); break;
                case 2: read_lines(2); break;
                case 3: read_lines(3); break;
                default: break;
            }
        }
    }

    void write_commands() {
        std::lock_guard<std::mutex> lock(status_mutex_);
        while (!outbox_.empty() && process_ && process_->fd(0) >= 0) {
            const ssize_t put = process_->write_to(0, outbox_.data(), outbox_.size());
            if (put > 0) {
                outbox_.erase(0, static_cast<size_t>(put));
                continue;
            }
            if (put < 0 && errno == EINTR) continue;
            if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            // EPIPE: it closed its command pipe, which means it is ending.
            process_->close_fd(0);
            outbox_.clear();
            return;
        }
    }

    void read_samples(uint8_t* samples, size_t want, size_t& got) {
        const int fd = process_->fd(1);
        if (fd < 0) return;
        if (phase_ == Phase::Opening) {
            // `ready` may be in the event pipe unread; only once that is empty
            // is a sample early.
            read_lines(3);
            if (phase_ == Phase::Failed) return;
        }
        if (phase_ != Phase::Streaming) {
            char probe;
            const ssize_t n = ::read(fd, &probe, 1);
            if (n > 0) {
                mark_failed(SourceFailure::Operator, id_ + " wrote samples before it was ready, which breaks the "
                                                           "module protocol; update or replace it");
            } else if (n == 0) {
                module_ended_ = true;
                mark_failed(SourceFailure::Transient, id_ + " ended");
            }
            return;
        }
        while (got < want) {
            const ssize_t n = ::read(fd, samples + got, want - got);
            if (n > 0) {
                got += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            module_ended_ = true;
            mark_failed(SourceFailure::Transient, id_ + " ended");
            return;
        }
    }

    void read_lines(int child_fd) {
        const int fd = process_->fd(static_cast<size_t>(child_fd));
        if (fd < 0) return;
        std::string& partial = child_fd == 3 ? partial_event_ : partial_log_;
        char buffer[kReadChunk];
        // Bounded, so a module that floods its log cannot keep the band
        // thread from its samples.
        for (int round = 0; round < 4; round++) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n == 0) {
                process_->close_fd(static_cast<size_t>(child_fd));
                if (!partial.empty()) {
                    const std::string last = partial;
                    partial.clear();
                    child_fd == 3 ? handle_event(last) : handle_log(last);
                }
                return;
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) process_->close_fd(static_cast<size_t>(child_fd));
                return;
            }
            partial.append(buffer, static_cast<size_t>(n));
            size_t start = 0;
            while (true) {
                const size_t end = partial.find('\n', start);
                if (end == std::string::npos) break;
                if (child_fd == 3 && end - start > kMaxEventLine) {
                    mark_failed(SourceFailure::Operator, id_ + " sent an event line longer than 64 KiB, which "
                                                               "breaks the module protocol");
                    return;
                }
                const std::string line = partial.substr(start, end - start);
                start = end + 1;
                if (child_fd == 3) handle_event(line);
                else handle_log(line);
                if (phase_ == Phase::Failed) return;
            }
            partial.erase(0, start);
            if (child_fd == 3 && partial.size() > kMaxEventLine) {
                mark_failed(SourceFailure::Operator, id_ + " sent an event line longer than 64 KiB, which breaks the "
                                                           "module protocol");
                return;
            }
            if (child_fd == 2 && partial.size() > kMaxLogLine) {
                handle_log(partial);
                partial.clear();
            }
        }
    }

    void handle_log(std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string text = printable(line, kMaxLogLine);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            log_.push_back(text);
            while (log_.size() > kLogLinesKept) log_.pop_front();
        }
        const auto now = Clock::now();
        if (now - log_window_ >= std::chrono::seconds(1)) {
            if (log_suppressed_ > 0) {
                log_write(routine(), "module", "%s %s: %d more line(s) in the admin panel", band_.c_str(),
                          id_.c_str(), log_suppressed_);
            }
            log_window_ = now;
            log_count_ = 0;
            log_suppressed_ = 0;
        }
        if (log_count_ < kLogLinesPerSecond) {
            log_count_++;
            log_write(routine(), "module", "%s %s: %s", band_.c_str(), id_.c_str(), text.c_str());
        } else {
            log_suppressed_++;
        }
    }

    void handle_event(const std::string& line) {
        Json event;
        if (!Json::parse(line, event) || !event.is_object()) {
            handle_log("(not a JSON event: " + printable(line, 120) + ")");
            return;
        }
        const std::string type = event["type"].string();
        if (type == "hello") {
            if (phase_ != Phase::Hello) return;
            if (!event["api"].is_number() || event["api"].number() != 1) {
                mark_failed(SourceFailure::Operator, id_ + " speaks module API " +
                                                         printable(event["api"].serialize(), 16) +
                                                         "; this receiver speaks API 1");
            } else if (event["id"].string() != id_) {
                mark_failed(SourceFailure::Operator, "the program installed as " + id_ + " calls itself '" +
                                                         printable(event["id"].string(), 40) + "'");
            } else if (event["kind"].string() != "input") {
                mark_failed(SourceFailure::Operator, id_ + " is not an input module");
            } else {
                phase_ = Phase::Opening;
            }
            return;
        }
        if (type == "ready") {
            if (phase_ != Phase::Opening) return;
            accept_ready(event);
            return;
        }
        if (type == "stats") {
            std::lock_guard<std::mutex> lock(status_mutex_);
            if (event["samples"].is_number()) module_samples_ = reported_count(event["samples"]);
            if (event["dropped"].is_number()) module_dropped_ = reported_count(event["dropped"]);
            // The share of the samples at the converter's limit since the
            // last stats, and the gain a module that sets it itself is using;
            // both optional.
            if (event["clipping"].is_number()) {
                const double share = event["clipping"].number(0.0);
                clipping_ = std::isfinite(share) ? std::clamp(share, 0.0, 1.0) : 0.0;
                has_clipping_ = true;
            }
            module_gain_ = event["gain"].is_number() ? event["gain"].number() : NAN;
            // A module that sets the gain itself takes care of clipping, and
            // says when it cannot; one left at a gain that clips, or with
            // none to lower, needs the operator.
            const auto now = Clock::now();
            if (clipping_ > kClippingToLog && !std::isfinite(module_gain_) && now >= next_clipping_log_) {
                LOG_WARN("band", "%s: the converter clips %.2g%% of its samples, which splatters over the whole band; "
                                 "lower the gain, or put an attenuator in front of the receiver",
                         band_.c_str(), clipping_ * 100);
                next_clipping_log_ = now + std::chrono::minutes(10);
            }
            return;
        }
        if (type == "applied") {
            std::lock_guard<std::mutex> lock(status_mutex_);
            const Json applied = flat_object(event["settings"]);
            if (!effective_.is_object()) effective_ = Json::make_object();
            // Existing keys take new values; new keys only while there is
            // room, or a module inventing names could grow this without end.
            for (const auto& [key, value] : applied.members()) {
                if (effective_.has(key) || effective_.size() < kMaxReportedKeys) effective_.set(key, value);
            }
            last_set_ = Json::make_object();
            last_set_.set("id", event["id"].number(0));
            last_set_.set("ok", true);
            last_set_.set("settings", applied);
            return;
        }
        if (type == "error") {
            const std::string message = printable(event["message"].string(), 400);
            const std::string code = printable(event["code"].string(), 32);
            if (event.has("id")) {
                std::lock_guard<std::mutex> lock(status_mutex_);
                last_set_ = Json::make_object();
                last_set_.set("id", event["id"].number(0));
                last_set_.set("ok", false);
                last_set_.set("message", message);
                return;
            }
            if (event["fatal"].boolean(false)) {
                fatal_code_ = code;
                fatal_message_ = message.empty() ? id_ + " reported " + code : message;
            } else {
                handle_log("(error " + code + ") " + message);
            }
            return;
        }
        // Unknown types are ignored, so a newer module can talk to this receiver.
    }

    void accept_ready(const Json& event) {
        SampleType type;
        bool implies_iq = false;
        const std::string format = event["format"].string();
        const std::string wanted = signal_ == SignalKind::Iq ? "iq" : "real";
        char numbers[160];
        if (!sample_type_from_name(format, type, implies_iq) || implies_iq) {
            mark_failed(SourceFailure::Operator, id_ + " announced sample format '" + printable(format, 16) +
                                                     "'; the protocol allows u8, s8, u16, s16 and f32");
            return;
        }
        if (event["signal"].string() != wanted) {
            mark_failed(SourceFailure::Operator, id_ + " delivers " + printable(event["signal"].string(), 16) +
                                                     " samples, but [band:" + band_ + "] has signal = " + wanted);
            return;
        }
        const double rate = event["sample_rate"].number(0.0);
        if (!event["sample_rate"].is_number() || !(rate > 0) || std::fabs(rate - sample_rate_) > sample_rate_ * 100e-6) {
            std::snprintf(numbers, sizeof(numbers), "%.0f Hz, but [band:%s] has sample_rate = %.0f", rate,
                          band_.c_str(), sample_rate_);
            mark_failed(SourceFailure::Operator, id_ + " runs at " + numbers +
                                                     ". Set the band to a rate the device supports.");
            return;
        }
        // Where the hardware really tuned, to the resolution it can be set
        // to: an RTL-SDR's RTL2832U, for one, sets its mixer in 6.9 Hz steps.
        // A few hertz of that are less than a crystal's own tolerance, and the
        // band keeps its axis; more means the module tuned somewhere else.
        constexpr double kCentreToleranceHz = 10.0;
        const double centre = event["center"].number(NAN);
        if (!event["center"].is_number() || !std::isfinite(centre) ||
            std::fabs(centre - center_hz_) > kCentreToleranceHz) {
            std::snprintf(numbers, sizeof(numbers), "%.1f Hz, but [band:%s] has center = %.1f", centre,
                          band_.c_str(), center_hz_);
            mark_failed(SourceFailure::Operator, id_ + " is tuned to " + numbers + ".");
            return;
        }
        format_ = type;
        phase_ = Phase::Streaming;
        // Streaming again: from here on what it says is news.
        quiet_.store(false);
        std::lock_guard<std::mutex> lock(status_mutex_);
        device_ = flat_object(event["device"]);
        effective_ = flat_object(event["settings"]);
        tuned_hz_ = centre;
        state_ = "streaming";
    }

    // Ends this run's process after a failure, and words the failure from
    // what the module said and how it exited.
    void conclude() {
        const bool ended = module_ended_;
        finish(false);
        if (ended) {
            SourceFailure kind = SourceFailure::Transient;
            std::string message = fatal_message_;
            if (fatal_code_ == "invalid") kind = SourceFailure::Operator;
            if (have_exit_ && exit_.exited && (exit_.code == 2 || exit_.code == 6)) kind = SourceFailure::Operator;
            if (message.empty()) {
                if (have_exit_ && exit_.exited && exit_meaning(exit_.code)) {
                    message = id_ + " ended: " + exit_meaning(exit_.code);
                } else if (have_exit_) {
                    message = id_ + " " + exit_.describe();
                } else {
                    message = id_ + " ended";
                }
                std::string last;
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    if (!log_.empty()) last = log_.back();
                }
                if (!last.empty()) message += ". Its last words: " + last;
            }
            failure_kind_ = kind;
            failure_ = message;
        }
        // One that survived SIGKILL is stuck in the kernel with its device.
        // Starting another would only get stuck beside it, a process and a
        // waiting thread at a time.
        if (have_exit_ && exit_.abandoned) {
            failure_kind_ = SourceFailure::Operator;
            failure_ = id_ + " is stuck in the kernel and could not be stopped, usually in a USB driver. Unplug "
                             "the device, or restart the machine, then restart the band";
        }
        phase_ = Phase::Failed;
        std::lock_guard<std::mutex> lock(status_mutex_);
        state_ = failure_kind_ == SourceFailure::Disabled ? "disabled" : "failed";
        failure_text_ = failure_;
    }

    // Stops the process if there is one and gives back the version lease.
    // `asked` is a stop the band wanted, which a module may answer by exiting,
    // by dying of the broken pipe or of SIGTERM: all of those did as asked.
    void finish(bool asked) {
        if (process_) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                outbox_ += "{\"type\":\"stop\"}\n";
            }
            write_commands();
            // Closing our end of fd 1 with fd 0 means a module blocked
            // writing samples gets EPIPE at once instead of waiting out the
            // grace period.
            exit_ = process_->terminate({0, 1}, timing_.grace, timing_.kill_wait, [this] { drain(); });
            have_exit_ = true;
            drain();
            process_.reset();
            std::lock_guard<std::mutex> lock(status_mutex_);
            pid_ = 0;
            outbox_.clear();
            if (state_ == "streaming" || state_ == "starting") state_ = "stopped";
            const bool obeyed = (exit_.exited && exit_.code == 0) || exit_.signal == SIGPIPE || exit_.signal == SIGTERM;
            const char* meaning = exit_.exited ? exit_meaning(exit_.code) : nullptr;
            exit_text_ = asked && obeyed ? "stopped as asked"
                         : asked && exit_.signal == SIGKILL ? "did not stop when asked, and was killed"
                         : meaning && exit_.code != 0 ? exit_.describe() + ": " + meaning
                                                      : exit_.describe();
        }
        if (!lease_version_.empty()) {
            store_->release(id_, lease_version_);
            lease_version_.clear();
        }
    }

    // Collects whatever the module still has to say while it is stopping.
    void drain() {
        if (!process_) return;
        for (int child_fd : {3, 2}) {
            if (process_->fd(static_cast<size_t>(child_fd)) >= 0) read_lines(child_fd);
        }
    }

    const std::string band_;
    const std::string id_;
    const SignalKind signal_;
    const double sample_rate_;
    const double center_hz_;
    double tuned_hz_ = 0.0;  // the centre the module reported, under status_mutex_
    const std::shared_ptr<ModuleStore> store_;
    const ModuleTiming timing_;

    // The band thread's own; stop() runs on it or after it has been joined.
    std::unique_ptr<Subprocess> process_;
    Phase phase_ = Phase::Idle;
    SampleType format_ = SampleType::U8;
    std::string lease_version_;
    std::vector<uint8_t> raw_;
    std::string partial_event_;
    std::string partial_log_;
    std::string failure_;
    SourceFailure failure_kind_ = SourceFailure::None;
    std::string fatal_code_;
    std::string fatal_message_;
    bool module_ended_ = false;
    bool have_exit_ = false;
    Subprocess::Exit exit_;
    Clock::time_point log_window_{};
    int log_count_ = 0;
    int log_suppressed_ = 0;

    std::atomic<bool> interrupted_{false};
    // Set while the band retries a failure it has logged; see set_retrying().
    std::atomic<bool> quiet_{false};

    /** The level for lines that only say the module is doing what it always does. */
    LogLevel routine() const { return quiet_.load() ? LogLevel::Debug : LogLevel::Info; }

    // Shared with the admin panel's thread.
    mutable std::mutex status_mutex_;
    std::map<std::string, std::string> raw_settings_;
    std::string state_ = "idle";
    std::string version_;
    ModuleManifest running_manifest_;
    int pid_ = 0;
    Json device_;
    Json effective_;
    Json last_set_;
    uint64_t samples_ = 0;
    uint64_t module_samples_ = 0;
    uint64_t module_dropped_ = 0;
    // The share of the samples that clipped between the module's last two
    // stats, when it counts them.
    bool has_clipping_ = false;
    double clipping_ = 0.0;
    double module_gain_ = NAN;  // dB, when the module sets it itself
    // Worth a line in the log: 1 sample in 1000.
    static constexpr double kClippingToLog = 1e-3;
    Clock::time_point next_clipping_log_{};
    std::string failure_text_;
    std::string exit_text_;
    std::deque<std::string> log_;
    std::string outbox_;
    uint64_t next_set_ = 1;
};

}  // namespace

std::unique_ptr<Source> make_module_source(const ConfigSection& section, const std::shared_ptr<ModuleStore>& store,
                                           std::string& error, const ModuleTiming& timing) {
    const std::string band = section.name().substr(section.name().find(':') + 1);
    const std::string id = section.get("module", "");
    if (!valid_module_id(id)) {
        error = "[" + section.name() + "] source = module needs module = <id>: 1 to 32 lowercase letters, "
                "digits and dashes, starting with a letter";
        return nullptr;
    }
    if (!store) {
        error = "[" + section.name() + "] source = module needs the receiver's module directory";
        return nullptr;
    }
    if (section.has("format")) {
        error = "[" + section.name() + "] a module announces its own sample format; remove format";
        return nullptr;
    }
    // What the installed module says of itself: the signal it delivers,
    // the rates worth offering (the first its default) and the centres it
    // tunes. A band that leaves signal or sample_rate out takes them from
    // there, and one that contradicts them is refused now, with the file
    // still open in front of the operator, rather than when the band starts.
    ModuleStore::Module installed;
    const ModuleManifest* active = nullptr;
    if (store->find(id, installed)) {
        for (const ModuleManifest& manifest : installed.versions) {
            if (manifest.version == installed.active) active = &manifest;
        }
    }
    const ModuleManifest::Tuning* tuning = active && !active->tuning.signal.empty() ? &active->tuning : nullptr;
    const std::string signal = section.get("signal", tuning ? tuning->signal : "iq");
    if (signal != "iq" && signal != "real") {
        error = "[" + section.name() + "] signal must be 'iq' or 'real', not '" + signal + "'";
        return nullptr;
    }
    if (tuning && signal != tuning->signal) {
        error = "[" + section.name() + "] the " + id + " module delivers " +
                (tuning->signal == "real" ? "a real signal from 0 Hz up" : "IQ about its centre") +
                ": set signal = " + tuning->signal + ", or leave the line out";
        return nullptr;
    }
    const double sample_rate =
        section.get_double("sample_rate", tuning && !tuning->rates.empty() ? tuning->rates.front() : 0.0);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        error = "[" + section.name() + "] sample_rate is required for a module input";
        return nullptr;
    }
    const double center = section.get_double("center", 0.0);
    if (!std::isfinite(center) || center < 0.0) {
        error = "[" + section.name() + "] center must be a frequency in Hz";
        return nullptr;
    }
    if (signal == "real" && center != 0.0) {
        error = "[" + section.name() + "] a real signal starts at 0 Hz, so center is 0 (or left out); "
                "low and high say what listeners see";
        return nullptr;
    }
    if (tuning && signal == "iq" && !tuning->ranges.empty() &&
        std::none_of(tuning->ranges.begin(), tuning->ranges.end(),
                     [&](const auto& range) { return center >= range.first && center <= range.second; })) {
        char text[160];
        std::snprintf(text, sizeof text, "the %s module tunes %.3f to %.3f MHz; center %.3f MHz is outside that",
                      id.c_str(), tuning->ranges.front().first / 1e6, tuning->ranges.back().second / 1e6, center / 1e6);
        error = "[" + section.name() + "] " + text;
        return nullptr;
    }
    std::map<std::string, std::string> settings;
    if (!ModuleSource::collect_settings(section, settings, error)) return nullptr;

    // A setting the module does not declare, or a value of the wrong type,
    // is refused now as well.
    if (active) {
        for (const auto& [key, text] : settings) {
            const ModuleSetting* setting = active->setting(key);
            if (!setting) {
                error = "[" + section.name() + "] " + id + " " + active->version + " has no setting '" + key + "'";
                return nullptr;
            }
            Json value;
            if (!type_module_setting(*setting, text, value, error)) {
                error = "[" + section.name() + "] module." + error;
                return nullptr;
            }
        }
    }
    return std::make_unique<ModuleSource>(band, id, signal == "real" ? SignalKind::Real : SignalKind::Iq, sample_rate,
                                          center, std::move(settings), store, timing);
}

}  // namespace fernsdr
