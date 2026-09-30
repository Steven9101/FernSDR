// One connected user: parses control messages, owns a Listener, and pushes
// encoded frames out.
//
// Every setting a client sends is clamped before it reaches the DSP, and
// anything that had to be changed comes back as a notice - a client that asks
// for a 40 kHz passband should be told why it did not get one, not silently
// given something else.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../util/json.h"
#include "listener.h"
#include "radio.h"
#include "chat.h"
#include "settings.h"
#include "stream_delivery.h"

namespace fernsdr {

class Session {
public:
    Session(uint64_t id, Radio& radio);
    ~Session();

    uint64_t id() const { return id_; }

    /**
     * Where this listener is connecting from. Set by the application, which is
     * the only thing that sees the socket; the session needs it because a chat
     * mute is by address.
     */
    void set_address(std::string address) { address_ = std::move(address); }

    // Output is queued rather than pushed through a callback into the
    // transport.  An earlier version handed the session a lambda capturing its
    // Connection; when a disconnect went unnoticed the session outlived the
    // connection and wrote into freed memory.  Queues make that impossible:
    // the transport pulls, and only ever into a connection it knows is alive.
    void collect(std::vector<std::string>& texts, std::vector<std::vector<uint8_t>>& binaries);

    // Sends the welcome message and attaches to the default band.
    void begin();
    // Handles one control message.  Malformed input is answered with an
    // error, never a disconnect: a buggy client should not lose its audio.
    void handle_text(const std::string& text);
    // Periodic: meters and stream statistics.
    void tick();
    // The listener's band went off the air by its hours: on to the band now
    // on the same input, if there is one, and a note either way.
    void follow_schedule();
    void account_control_bytes(size_t bytes) { if (listener_) listener_->account_control_bytes(bytes); }
    void request_waterfall_keyframe() { if (listener_) listener_->request_waterfall_keyframe(); }
    void note_media_expiry() { if (listener_) listener_->note_media_expiry(); }
    StreamDelivery& delivery() { return delivery_; }
    void set_transport_backlog(size_t bytes, int queue_delay_ms = 0) {
        transport_backlog_ = bytes;
        if (listener_) listener_->set_transport_backlog(bytes, queue_delay_ms);
    }

    void detach();

    const std::string& band_id() const { return band_id_; }

    // What the admin panel shows about one listener. Deliberately does not
    // include anything that would let an operator read a listener's audio or
    // identify them beyond the address the connection came from.
    struct Summary {
        uint64_t id = 0;
        std::string band;
        double frequency_hz = 0.0;
        std::string mode;
        double bandwidth_hz = 0.0;
        int audio_bitrate = 0;
        int waterfall_bitrate = 0;
        int64_t connected_seconds = 0;
    };
    Summary summarise() const;

    // A listener's inactivity against the site's limit (0 for none). Queues
    // the "still listening?" warning once, kInactivityWarningMs before the
    // limit; Expired means the app should close the connection.
    enum class Inactivity { None, Expired };
    static constexpr int64_t kInactivityWarningMs = 60000;
    Inactivity check_inactivity(int64_t limit_ms, int64_t now_ms);

    /** Takes the oldest chat line this session posted and the app has not yet broadcast. */
    bool take_chat(ChatMessage& out) {
        if (pending_chats_.empty()) return false;
        out = std::move(pending_chats_.front());
        pending_chats_.erase(pending_chats_.begin());
        return true;
    }

private:
    void attach_to(Band* band);
    void handle_tune(const Json& message);
    void handle_viewport(const Json& message);
    void handle_audio(const Json& message);
    void handle_dsp(const Json& message);
    void handle_chat(const Json& message);

    void queue_text(const std::string& text);
    void send_welcome();
    // Says the state has changed. The state itself goes out at once after a
    // quiet spell, and during a burst of changes (a slider or a passband being
    // dragged) at most every kStateSpacingMs, always the latest: one reply to
    // each of thirty changes a second was ten kilobytes a second of control
    // traffic, which the bandwidth budget took from the waterfall until it
    // stopped.
    void send_state(const std::string& note = "");
    void flush_state();
    void send_error(const std::string& message);
    void apply_channel(const std::string& note_prefix);

    Json describe_band(const Band& band) const;

    uint64_t id_;

    std::string address_;
    Radio& radio_;
    Band* band_ = nullptr;
    std::string band_id_;
    std::shared_ptr<Listener> listener_;

    int64_t connected_at_ms_ = 0;
    std::string chat_name_;
    // Chat lines this session posted since the last tick; the application
    // picks them up and fans them out, because a session may not reach into
    // other sessions. More than one can arrive within a tick, and each was
    // accepted into the history, so each is delivered. The chat's rate limit
    // keeps this short.
    std::vector<ChatMessage> pending_chats_;
    // Whether the listener has picked a bitrate, as opposed to inheriting the
    // band's default.
    bool bitrate_chosen_ = false;
    double tune_request_id_ = 0;
    double viewport_request_id_ = 0;
    double dsp_request_id_ = 0;
    ChannelSettings channel_;
    ViewportSettings viewport_;
    StreamDelivery delivery_;

    std::vector<std::string> pending_texts_;
    std::vector<uint8_t> pending_meter_;
    bool binary_meter_ = false;
    // The page reads the CTCSS tone in the binary meter's NFM flag; a page
    // that does not would throw the whole reading away.
    bool ctcss_meter_ = false;
    static constexpr int64_t kStateSpacingMs = 100;
    bool state_due_ = false;
    std::string state_note_;
    int64_t last_state_ms_ = -1000000;
    int64_t last_meter_ms_ = 0;
    int64_t last_activity_ms_ = 0;
    bool inactivity_warned_ = false;
    bool inactivity_expired_ = false;
    void note_activity();
    int64_t slow_meter_until_ms_ = 0;
    // RDS goes out when it changes: at once for another station, otherwise
    // at most once a second, since a station's radiotext comes in pieces
    // and nobody reads faster than that.
    uint64_t rds_sent_sequence_ = 0;
    uint64_t rds_sent_station_ = 0;
    int64_t last_rds_ms_ = -1000000;
    int64_t last_history_ms_ = -1000000;
    void send_rds(const RdsState& rds);
    size_t transport_backlog_ = 0;
    uint8_t last_audio_generation_ = 0xFF;
    double last_reported_rate_ = 0.0;
};

}  // namespace fernsdr
