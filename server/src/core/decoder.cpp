#include "decoder.h"

#include "band.h"
#include "../util/log.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace fernsdr {

namespace {

constexpr size_t kMaxLine = 64 * 1024;
constexpr size_t kFrameHeader = 32;
constexpr int kDecodesPerSlot = 200;
// Seconds of slot time counted at once, across a decoder's channels: ten
// minutes of every second on several channels, and no more.
constexpr size_t kSlotsKept = 4096;

void put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>(value >> 8));
}
void put32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
void put64(std::string& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}

std::string plain(const std::string& text, size_t limit) {
    std::string out;
    for (char c : text) {
        if (out.size() >= limit) break;
        out.push_back(c >= 0x20 && c <= 0x7e ? c : '?');
    }
    return out;
}

}  // namespace

std::string DecoderChannelConfig::id() const {
    return band + "-" + mode + "-" + std::to_string(static_cast<long long>(std::llround(dial_hz / 1000.0)));
}

const std::vector<std::chrono::milliseconds>& Decoder::restart_delays() {
    static const std::vector<std::chrono::milliseconds> delays = {
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(4),
        std::chrono::seconds(8), std::chrono::seconds(16), std::chrono::seconds(30)};
    return delays;
}

Decoder::Decoder(DecoderConfig config, Resolve resolve, FindBand find_band, DecodeStore& store)
    : config_(std::move(config)), resolve_(std::move(resolve)), find_band_(std::move(find_band)), store_(store) {}

Decoder::~Decoder() { stop(); }

int64_t Decoder::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void Decoder::set_state(const std::string& state, const std::string& message) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    state_ = state;
    message_ = message;
}

bool Decoder::start(std::string& error) {
    if (thread_.joinable()) return true;
    if (config_.channels.empty()) {
        error = "[decoder:" + config_.id + "] has no channels";
        return false;
    }
    channels_.clear();
    for (size_t i = 0; i < config_.channels.size(); i++) {
        const DecoderChannelConfig& channel = config_.channels[i];
        Band* band = find_band_(channel.band);
        if (!band) {
            error = "[decoder:" + config_.id + "] names band '" + channel.band + "', which is not configured";
            return false;
        }
        const double low = channel.dial_hz + channel.offset_hz - channel.width_hz / 2.0;
        const double high = channel.dial_hz + channel.offset_hz + channel.width_hz / 2.0;
        if (low < band->sample_low_hz() || high > band->sample_high_hz()) {
            error = "[decoder:" + config_.id + "] channel " + channel.id() + " reaches outside what band '" +
                    channel.band + "' receives";
            return false;
        }
        channels_.push_back({channel.id(), channel.band, channel.mode, channel.dial_hz, channel.offset_hz, channel.width_hz});
    }
    const size_t count = channels_.size();
    taps_.clear();
    bands_.clear();
    max_pending_samples_ = 0;
    for (size_t i = 0; i < count; i++) {
        const DecoderChannelConfig& channel = config_.channels[i];
        Band* band = find_band_(channel.band);
        taps_.push_back(band->add_decoder_tap(static_cast<uint16_t>(i), channel.dial_hz, channel.offset_hz, channel.width_hz));
        bands_.push_back(band);
        // Past this whole frames go, and the decoder is told.
        max_pending_samples_ += taps_.back()->pending_budget();
    }
    frames_sent_.assign(count, 0);
    frames_dropped_.assign(count, 0);
    decodes_.assign(count, 0);
    stopping_.store(false);
    set_state("starting", "");
    thread_ = std::thread([this] { run(); });
    return true;
}

void Decoder::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        stopping_.store(true);
    }
    wake_.notify_all();
    thread_.join();
    for (size_t i = 0; i < taps_.size(); i++) bands_[i]->remove_decoder_tap(taps_[i]);
    taps_.clear();
    bands_.clear();
    set_state("stopped", "");
}

void Decoder::restart() {
    {
        // Under the lock the wait checks its condition with, so a request
        // made just as the wait begins is not missed.
        std::lock_guard<std::mutex> lock(wake_mutex_);
        restart_requested_.store(true);
    }
    wake_.notify_all();
}

void Decoder::run() {
    size_t attempt = 0;
    while (!stopping_.load()) {
        const auto began = std::chrono::steady_clock::now();
        std::string why;
        session(why);
        if (stopping_.load()) break;
        // A module that ran for a minute failed for a new reason: try again
        // soon, not after the longest wait.
        if (std::chrono::steady_clock::now() - began >= std::chrono::minutes(1) || why.rfind("restarting", 0) == 0) {
            attempt = 0;
        }
        const auto& delays = restart_delays();
        const auto delay = delays[std::min(attempt, delays.size() - 1)];
        attempt++;
        set_state("waiting", why + "; starting again in " +
                                 std::to_string(std::chrono::duration_cast<std::chrono::seconds>(delay).count()) + " s");
        LOG_WARN("decoder", "%s: %s", config_.id.c_str(), why.c_str());
        std::unique_lock<std::mutex> lock(wake_mutex_);
        // A restart asked for while waiting, after an update or from the
        // panel, starts the module at once rather than after the wait.
        wake_.wait_for(lock, delay, [this] { return stopping_.load() || restart_requested_.load(); });
        if (restart_requested_.load()) attempt = 0;
        if (!stopping_.load()) {
            std::lock_guard<std::mutex> status(status_mutex_);
            restarts_++;
        }
    }
}

bool Decoder::session(std::string& why) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        sandbox_ = Json();
        hello_version_.clear();
    }
    sandbox_partial_.clear();
    log_partial_.clear();
    Program program;
    std::string error;
    restart_requested_.store(false);
    if (!resolve_(config_.module, program, error)) {
        why = error;
        return false;
    }
    // Given back however the session ends.
    struct Lease {
        std::function<void()> release;
        ~Lease() { if (release) release(); }
    } lease{program.release};
    const Json& settings = program.settings.is_object() ? program.settings : config_.settings;
    Subprocess::Options options;
    if (program.launcher.empty()) {
        why = "the decoder needs the receiver's sandbox launcher";
        return false;
    }
    options.path = program.launcher;
    options.arguments = {"--sandbox-exec-report", program.executable, "--fernsdr-module", "2"};
    options.environment = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "FERNSDR_MODULE_API=2"};
    options.streams = {Subprocess::Stream::ToChild, Subprocess::Stream::Null, Subprocess::Stream::FromChild,
                       Subprocess::Stream::FromChild, Subprocess::Stream::ToChild, Subprocess::Stream::FromChild};
    process_ = std::make_unique<Subprocess>();
    if (!process_->start(options, error)) {
        why = error;
        return false;
    }
    set_state("starting", "");

    // Whatever piled up while no decoder ran is stale: start from now, with
    // each channel counting from 0 again as the contract says.
    for (const auto& tap : taps_) tap->take_frames();
    pending_.clear();
    writing_.clear();
    written_ = 0;
    pending_samples_ = 0;
    base_index_.assign(taps_.size(), 0);
    based_.assign(taps_.size(), false);
    lost_since_.assign(taps_.size(), false);
    per_slot_.clear();
    newest_second_ = INT64_MIN;
    std::string events;
    bool hello = false;
    bool ready = false;
    bool opened = false;
    const auto started = std::chrono::steady_clock::now();
    bool ok = true;
    while (!stopping_.load()) {
        if (restart_requested_.load()) {
            why = "restarting for a change to the module";
            ok = false;
            break;
        }
        pollfd fds[4] = {{process_->fd(2), POLLIN, 0}, {process_->fd(3), POLLIN, 0},
                         {process_->fd(4), static_cast<short>(ready && (!writing_.empty() || !pending_.empty()) ? POLLOUT : 0), 0},
                         {process_->fd(5), POLLIN, 0}};
        ::poll(fds, 4, 50);
        read_log(process_->fd(2));
        if (!read_sandbox(why) || (sandbox_.is_object() && !read_events(process_->fd(3), events, ready, why))) {
            ok = false;
            break;
        }
        if (!hello && !hello_version_.empty()) hello = true;
        if (hello && !opened) {
            Json open = Json::make_object();
            open.set("type", "open");
            Json list = Json::make_array();
            for (size_t i = 0; i < channels_.size(); i++) {
                Json channel = Json::make_object();
                channel.set("id", channels_[i].id);
                channel.set("band", channels_[i].band);
                channel.set("mode", channels_[i].mode);
                channel.set("dial", channels_[i].dial_hz);
                channel.set("rate", taps_[i]->rate());
                channel.set("offset", channels_[i].offset_hz);
                channel.set("width", channels_[i].width_hz);
                channel.set("format", "cf32");
                list.push_back(channel);
            }
            open.set("channels", list);
            open.set("settings", settings);
            const std::string line = open.serialize() + "\n";
            if (process_->write_to(0, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
                why = "the module did not take its open command";
                ok = false;
                break;
            }
            opened = true;
        }
        const auto waited = std::chrono::steady_clock::now() - started;
        if (!hello && waited > std::chrono::seconds(5)) {
            why = "the module said no hello within 5 seconds";
            ok = false;
            break;
        }
        if (!ready && waited > std::chrono::seconds(20)) {
            why = "the module was not ready within 15 seconds of open";
            ok = false;
            break;
        }
        if (ready) {
            queue_frames();
            if (!write_frames(process_->fd(4), why)) {
                ok = false;
                break;
            }
        } else {
            for (const auto& tap : taps_) tap->take_frames();
        }
        Subprocess::Exit exit;
        if (process_->poll_exit(exit)) {
            read_log(process_->fd(2));
            if (read_sandbox(why) && sandbox_.is_object()) read_events(process_->fd(3), events, ready, why);
            if (why.empty()) why = "the module ended: " + exit.describe();
            ok = false;
            break;
        }
    }

    if (process_->started()) {
        const std::string stop = "{\"type\":\"stop\"}\n";
        process_->write_to(0, stop.data(), stop.size());
        process_->terminate({0, 4}, std::chrono::milliseconds(2000), std::chrono::milliseconds(2000),
                            [this] { read_log(process_->fd(2)); });
    }
    process_.reset();
    return ok;
}

bool Decoder::read_sandbox(std::string& why) {
    const int fd = process_->fd(5);
    if (fd < 0) return true;
    char chunk[2048];
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    if (got > 0) sandbox_partial_.append(chunk, static_cast<size_t>(got));
    const size_t end = sandbox_partial_.find('\n');
    if (end == std::string::npos && got > 0 && sandbox_partial_.size() < sizeof(chunk)) return true;
    Json report;
    const bool valid = end != std::string::npos && end + 1 == sandbox_partial_.size() &&
        sandbox_partial_.size() < sizeof(chunk) && Json::parse(sandbox_partial_.substr(0, end), report) &&
        report.is_object() && report["seccomp"].is_bool() && report["files_closed"].is_bool() &&
        report["scoped"].is_bool() && report["landlock_abi"].is_number() && report["problem"].is_string();
    process_->close_fd(5);
    if (!valid) {
        why = "the sandbox launcher did not report confinement";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        sandbox_ = report;
    }
    if (!report["seccomp"].boolean()) {
        why = report["problem"].string().empty() ? "the sandbox launcher could not apply seccomp" : report["problem"].string();
        return false;
    }
    return true;
}

bool Decoder::read_events(int fd, std::string& buffer, bool& ready, std::string& why) {
    if (fd < 0) return true;
    char chunk[16384];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got > 0) {
            buffer.append(chunk, static_cast<size_t>(got));
            size_t start = 0;
            for (;;) {
                const size_t end = buffer.find('\n', start);
                if (end == std::string::npos) break;
                Json event;
                if (Json::parse(buffer.substr(start, end - start), event) && event.is_object()) {
                    handle_event(event, ready, why);
                    if (!why.empty()) return false;
                }
                start = end + 1;
            }
            buffer.erase(0, start);
            if (buffer.size() > kMaxLine) {
                why = "the module sent an event line longer than 64 KiB";
                return false;
            }
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        return true;  // nothing more for now, or the end
    }
}

void Decoder::handle_event(const Json& event, bool& ready, std::string& why) {
    const std::string type = event["type"].string();
    if (type == "hello") {
        if (event["api"].number() != 2 || event["kind"].string() != "decoder") {
            why = "the module is not a decoder speaking API 2";
            return;
        }
        std::string version = plain(event["version"].string(), 32);
        std::lock_guard<std::mutex> lock(status_mutex_);
        hello_version_ = version.empty() ? "?" : version;
        return;
    }
    if (type == "ready") {
        ready = true;
        set_state("running", "");
        LOG_INFO("decoder", "%s: %s %s is decoding %zu channel(s)", config_.id.c_str(), config_.module.c_str(),
                 hello_version_.c_str(), channels_.size());
        return;
    }
    if (type == "decode") {
        Decode decode;
        std::string problem;
        if (!parse_decode(event, channels_, decode, problem)) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            if (rejected_++ == 0 || last_rejection_ != problem) {
                LOG_WARN("decoder", "%s: dropped a decode that %s", config_.id.c_str(), problem.c_str());
            }
            last_rejection_ = problem;
            return;
        }
        const size_t index = static_cast<size_t>(std::find_if(channels_.begin(), channels_.end(),
            [&](const DecodeChannel& c) { return c.id == decode.channel; }) - channels_.begin());
        // Counted by the second, not the millisecond: a decoder that moved
        // each decode's time by a millisecond would otherwise get a new slot,
        // and a new entry, every time. The ten minutes kept are measured from
        // the newest second seen, never from the decode at hand, so times
        // that step backwards cannot keep old entries alive either; and the
        // table has a ceiling whatever the times say.
        const int64_t second = decode.time_ms / 1000;
        const char* refused = nullptr;
        if (newest_second_ != INT64_MIN && second < newest_second_ - 600) {
            refused = "a time more than ten minutes behind its newest";
        } else {
            if (second > newest_second_) {
                newest_second_ = second;
                for (auto it = per_slot_.begin(); it != per_slot_.end();) {
                    if (it->first.second < newest_second_ - 600) it = per_slot_.erase(it);
                    else ++it;
                }
            }
            const auto key = std::make_pair(index, second);
            if (per_slot_.size() >= kSlotsKept && per_slot_.find(key) == per_slot_.end()) {
                refused = "too many slots at once";
            } else if (++per_slot_[key] > kDecodesPerSlot) {
                refused = "more than 200 decodes in one slot";
            }
        }
        if (refused) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            rejected_++;
            last_rejection_ = refused;
            return;
        }
        decode.decoder = config_.id;
        store_.add(std::move(decode), now_ms());
        std::lock_guard<std::mutex> lock(status_mutex_);
        decodes_[index]++;
        return;
    }
    if (type == "stats") {
        // Only numbers, per channel id the decoder was given.
        Json kept = Json::make_array();
        const Json& list = event["channels"];
        for (size_t i = 0; i < list.size() && i < channels_.size(); i++) {
            Json entry = Json::make_object();
            entry.set("id", plain(list[i]["id"].string(), 48));
            for (const char* key : {"slots", "decodes", "late", "cpu_ms"}) entry.set(key, list[i][key].number());
            kept.push_back(entry);
        }
        std::lock_guard<std::mutex> lock(status_mutex_);
        module_stats_ = kept;
        return;
    }
    if (type == "error") {
        const std::string message = plain(event["message"].string(), 400);
        set_state(ready ? "running" : "starting", message);
        LOG_WARN("decoder", "%s: %s", config_.id.c_str(), message.c_str());
    }
}

void Decoder::read_log(int fd) {
    if (fd < 0) return;
    char chunk[4096];
    std::string& partial = log_partial_;
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        partial.append(chunk, static_cast<size_t>(got));
        size_t end;
        while ((end = partial.find('\n')) != std::string::npos) {
            const std::string line = plain(partial.substr(0, end), 1024);
            partial.erase(0, end + 1);
            std::lock_guard<std::mutex> lock(status_mutex_);
            log_.push_back(line);
            if (log_.size() > 200) log_.pop_front();
        }
        if (partial.size() > 4096) partial.clear();
    }
}

void Decoder::queue_frames() {
    for (size_t i = 0; i < taps_.size(); i++) {
        for (auto& frame : taps_[i]->take_frames()) {
            if (!based_[i]) {
                base_index_[i] = frame.index;
                based_[i] = true;
                // The first frame of a session starts the count; there is no
                // earlier clock to have been set again.
                frame.flags = 0;
            }
            frame.index -= base_index_[i];
            if (lost_since_[i]) {
                frame.flags |= DecoderTap::kGap;
                lost_since_[i] = false;
            }
            pending_samples_ += frame.samples.size();
            pending_.push_back(std::move(frame));
        }
    }
    while (pending_samples_ > max_pending_samples_ && !pending_.empty()) {
        const TapFrame& dropped = pending_.front();
        const size_t channel = dropped.channel;
        pending_samples_ -= dropped.samples.size();
        pending_.pop_front();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            frames_dropped_[channel]++;
        }
        // The next frame of that channel, queued or still to come, says so.
        auto next = std::find_if(pending_.begin(), pending_.end(), [&](const TapFrame& f) { return f.channel == channel; });
        if (next != pending_.end()) next->flags |= DecoderTap::kGap;
        else lost_since_[channel] = true;
    }
}

bool Decoder::write_frames(int fd, std::string& why) {
    if (fd < 0) return true;
    for (;;) {
        if (writing_.empty()) {
            if (pending_.empty()) return true;
            TapFrame frame = std::move(pending_.front());
            pending_.pop_front();
            pending_samples_ -= frame.samples.size();
            writing_.reserve(kFrameHeader + frame.samples.size() * 8);
            writing_.append("FDR1", 4);
            put16(writing_, frame.channel);
            put16(writing_, frame.flags);
            put32(writing_, static_cast<uint32_t>(frame.samples.size()));
            put32(writing_, 0);
            put64(writing_, frame.index);
            put64(writing_, static_cast<uint64_t>(frame.utc_us));
            // std::complex<float> is two floats, real then imaginary, and every
            // platform the receiver builds for is little-endian: cf32 as is.
            writing_.append(reinterpret_cast<const char*>(frame.samples.data()), frame.samples.size() * sizeof(cfloat));
            written_ = 0;
            std::lock_guard<std::mutex> lock(status_mutex_);
            frames_sent_[frame.channel]++;
        }
        const ssize_t put = process_->write_to(4, writing_.data() + written_, writing_.size() - written_);
        if (put > 0) {
            written_ += static_cast<size_t>(put);
            if (written_ == writing_.size()) {
                writing_.clear();
                written_ = 0;
            }
            continue;
        }
        if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
        why = put < 0 && errno == EPIPE ? "the module stopped reading its samples" : "cannot write samples to the module";
        return false;
    }
}

Json Decoder::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    Json out = Json::make_object();
    out.set("id", config_.id);
    out.set("module", config_.module);
    out.set("version", hello_version_);
    out.set("state", state_);
    out.set("message", message_);
    out.set("sandbox", sandbox_);
    out.set("restarts", restarts_);
    out.set("rejected", static_cast<double>(rejected_));
    if (!last_rejection_.empty()) out.set("last_rejection", last_rejection_);
    Json list = Json::make_array();
    for (size_t i = 0; i < channels_.size(); i++) {
        Json channel = Json::make_object();
        channel.set("id", channels_[i].id);
        channel.set("band", channels_[i].band);
        channel.set("mode", channels_[i].mode);
        channel.set("dial", channels_[i].dial_hz);
        channel.set("frames_sent", static_cast<double>(i < frames_sent_.size() ? frames_sent_[i] : 0));
        channel.set("frames_dropped", static_cast<double>(i < frames_dropped_.size() ? frames_dropped_[i] : 0));
        channel.set("decodes", static_cast<double>(i < decodes_.size() ? decodes_[i] : 0));
        list.push_back(channel);
    }
    out.set("channels", list);
    out.set("module_stats", module_stats_);
    Json log = Json::make_array();
    for (const auto& line : log_) log.push_back(line);
    out.set("log", log);
    return out;
}

}  // namespace fernsdr
