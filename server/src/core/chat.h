// The chat widget's server side.
//
// A receiver's chat is where listeners tell each other what they are hearing,
// and it is worth about two hundred lines. It is not a general messaging
// system: no accounts, no history beyond what fits in memory, no moderation
// beyond the operator's ability to disconnect somebody.
//
// What it does take seriously is the ways a public chat box goes wrong.
// Messages are rate-limited per connection, because otherwise one script fills
// every listener's screen. Nothing here interprets markup: a message is text,
// it is stored as text, and the client puts it in a text node. The operator's
// own listeners are not a trusted input.
//
// And the operator can mute an address. Disconnecting somebody was the only
// tool there was, and it lasts exactly as long as it takes them to reload the
// page. A mute is by address, has an expiry, survives a restart, and says so
// to the person muted: a silent one only teaches them to shout louder.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../util/json.h"

namespace fernsdr {

/** An address the operator has silenced, and until when. */
struct ChatMute {
    std::string address;
    /** Epoch milliseconds. Zero means until the operator lifts it. */
    int64_t until_ms = 0;
};

struct ChatMessage {
    uint64_t id = 0;
    std::string name;
    std::string text;
    int64_t at_ms = 0;
};

class ChatRoom {
public:
    // Enough to fill a screen on arrival without being a log.
    static constexpr size_t kHistory = 80;
    static constexpr size_t kMaxNameLength = 24;
    static constexpr size_t kMaxTextLength = 400;

    /**
     * Accepts a message from a listener.
     *
     * Returns false with `error` set when it was refused - too fast, empty, or
     * nothing but control characters. `out` is the message as everyone else
     * will see it.
     */
    bool post(uint64_t session, const std::string& address, const std::string& name,
              const std::string& text, int64_t now_ms, ChatMessage& out, std::string& error);

    /**
     * Silences an address. `minutes` of zero means until it is lifted.
     *
     * By address rather than by session, because a session ends the moment the
     * page is reloaded and the point of this is that it does not.
     */
    void mute(const std::string& address, int minutes, int64_t now_ms);
    void unmute(const std::string& address);
    bool muted(const std::string& address, int64_t now_ms) const;
    /** The mutes still in force, expired ones dropped. */
    std::vector<ChatMute> mutes(int64_t now_ms) const;

    /** The mute list as it is stored between runs, and read back. */
    Json mutes_json(int64_t now_ms) const;
    void load_mutes(const Json& list);

    std::vector<ChatMessage> history() const;

    /**
     * Turns the chat on or off. Off, every post is refused with a reason and
     * the history is empty; turning it off also forgets what was said, so
     * nothing reappears when it is turned back on.
     */
    void set_enabled(bool enabled);
    bool enabled() const;

private:
    struct Rate {
        int64_t window_started_ms = 0;
        int count = 0;
    };

    mutable std::mutex mutex_;
    bool enabled_ = true;
    std::deque<ChatMessage> messages_;
    // By network_key() of the address rather than by session: a session ends
    // with every reload, and sixteen of them from one address each had a rate
    // of their own.
    std::map<std::string, Rate> rates_;
    std::map<std::string, int64_t> mutes_;
    uint64_t next_id_ = 1;
};

/** Strips control characters and trims; returns "" when nothing is left. */
std::string clean_chat_text(const std::string& value, size_t limit);

}  // namespace fernsdr
