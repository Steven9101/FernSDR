// One decoder module at work: its channels on the bands, the process in its
// sandbox, the frames going to it and the decodes coming back. See
// docs/MODULES.md, "Decoders".
#pragma once

#include "decodes.h"
#include "decoder_tap.h"
#include "../util/json.h"
#include "../util/subprocess.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fernsdr {

class Band;

struct DecoderChannelConfig {
    std::string band;
    double dial_hz = 0.0;
    std::string mode = "ft8";
    double offset_hz = 2000.0;
    double width_hz = 4000.0;
    // "<band>-<mode>-<dial in kHz>": what the decoder and the pages call it.
    std::string id() const;
};

struct DecoderConfig {
    std::string id;          // the section's name, [decoder:<id>]
    std::string module;      // the module's id
    Json settings = Json::make_object();
    std::vector<DecoderChannelConfig> channels;
    // Whether listeners see what it decodes. Off unless the operator says so:
    // the decodes name other stations and where they are, and publishing
    // them is the operator's decision rather than a default.
    bool listed = false;
    // Whether its decodes are reported to PSK Reporter (report =
    // pskreporter); off unless the operator says so. See SpotReporter.
    bool report = false;
};

class Decoder {
public:
    // The executable and this program's own path, whose sandbox launcher
    // reports confinement before executing the module.
    struct Program {
        std::string executable;
        std::string launcher;
        // The settings as the module's manifest types them; when null, the
        // config's own (tests give them ready typed).
        Json settings;
        // Called when the session ends, to give back the module store's
        // lease on the version that ran.
        std::function<void()> release;
    };
    using Resolve = std::function<bool(const std::string& module, Program& out, std::string& error)>;
    using FindBand = std::function<Band*(const std::string& id)>;

    Decoder(DecoderConfig config, Resolve resolve, FindBand find_band, DecodeStore& store);
    ~Decoder();

    const DecoderConfig& config() const { return config_; }
    // Creates the channels on their bands and starts the process thread.
    bool start(std::string& error);
    void stop();
    // Ends the running session, so the next one starts the module afresh:
    // after an update or a change of settings.
    void restart();

    // For the admin panel: state, the module's last words, per channel what
    // went to it and what came back.
    Json status() const;

    // Delays before restarting a module that ended; the last repeats.
    static const std::vector<std::chrono::milliseconds>& restart_delays();

private:
    void run();
    bool session(std::string& why);
    bool read_sandbox(std::string& why);
    bool read_events(int fd, std::string& buffer, bool& ready, std::string& why);
    void handle_event(const Json& event, bool& ready, std::string& why);
    void read_log(int fd);
    void queue_frames();
    bool write_frames(int fd, std::string& why);
    void set_state(const std::string& state, const std::string& message);
    int64_t now_ms() const;

    DecoderConfig config_;
    Resolve resolve_;
    FindBand find_band_;
    DecodeStore& store_;
    std::vector<DecodeChannel> channels_;
    std::vector<std::shared_ptr<DecoderTap>> taps_;
    std::vector<Band*> bands_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> restart_requested_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::unique_ptr<Subprocess> process_;

    // Frames waiting for fd 4, and how much of the first has gone.
    std::deque<TapFrame> pending_;
    std::string writing_;
    size_t written_ = 0;
    std::vector<uint64_t> base_index_;
    std::vector<bool> based_;
    std::vector<bool> lost_since_;
    size_t pending_samples_ = 0;
    size_t max_pending_samples_ = 0;
    // Decodes a channel reported in one second of slot time, for the limit
    // of 200, over the ten minutes behind the newest second reported.
    std::map<std::pair<size_t, int64_t>, int> per_slot_;
    int64_t newest_second_ = INT64_MIN;

    mutable std::mutex status_mutex_;
    std::string state_ = "stopped";
    std::string message_;
    std::deque<std::string> log_;
    int restarts_ = 0;
    uint64_t rejected_ = 0;
    std::string last_rejection_;
    Json module_stats_ = Json::make_object();
    std::vector<uint64_t> frames_sent_;
    std::vector<uint64_t> frames_dropped_;
    std::vector<uint64_t> decodes_;
    // Written by the session thread under status_mutex_, which also reads it
    // without the lock; status() reads it under the lock.
    std::string hello_version_;
    std::string log_partial_;
    Json sandbox_;
    std::string sandbox_partial_;
};

}  // namespace fernsdr
