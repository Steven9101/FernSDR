#include "chat.h"
#include "../net/server.h"
#include "../util/utf8.h"

#include <algorithm>

namespace fernsdr {

namespace {

// Six messages in ten seconds is conversational; anything faster is a script.
constexpr int kBurst = 6;
// An IPv6 /64 is one household, and a /48 is free from a tunnel broker: a
// script moving from /64 to /64 in its /48 meets this, and the whole chat
// meets the last, so that no number of addresses floods every listener.
constexpr int kWideBurst = 24;
constexpr int kEveryoneBurst = 60;
constexpr int64_t kWindowMs = 10000;

}  // namespace

namespace {

// The code point of the UTF-8 character at the start of `text`, already
// known to be valid and `bytes` long.
uint32_t code_point(std::string_view text, size_t bytes) {
    const auto b = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(text[i])); };
    if (bytes == 1) return b(0);
    if (bytes == 2) return ((b(0) & 0x1F) << 6) | (b(1) & 0x3F);
    if (bytes == 3) return ((b(0) & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    return ((b(0) & 0x07) << 18) | ((b(1) & 0x3F) << 12) | ((b(2) & 0x3F) << 6) | (b(3) & 0x3F);
}

// Characters that show nothing and change how the rest is shown: direction
// overrides and isolates, which can turn the line around, zero-width ones,
// which make a name look like another's, line separators and the C1
// controls. A chat line is plain text; none of them belongs in it.
bool invisible(uint32_t c) {
    return (c >= 0x80 && c <= 0x9F) || c == 0xAD || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF;
}

}  // namespace

std::string clean_chat_text(const std::string& value, size_t limit) {
    if (!valid_utf8(value)) return "";
    std::string out;
    out.reserve(std::min(value.size(), limit));
    for (size_t at = 0; at < value.size();) {
        const size_t bytes = utf8_character_bytes(std::string_view(value).substr(at));
        const auto c = static_cast<unsigned char>(value[at]);
        at += bytes;
        if (out.size() >= limit) break;
        // Newlines included: a message that spans ten lines is a way to push
        // everyone else's messages off the screen.
        if (c < 0x20 || c == 0x7F) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        if (bytes > 1 && invisible(code_point(std::string_view(value).substr(at - bytes), bytes))) continue;
        // Truncating a multibyte character would poison every recipient's
        // WebSocket text stream, including subsequent chat-history replies.
        if (out.size() + bytes > limit) break;
        out.append(value, at - bytes, bytes);
    }
    const size_t begin = out.find_first_not_of(' ');
    if (begin == std::string::npos) return "";
    const size_t end = out.find_last_not_of(' ');
    return out.substr(begin, end - begin + 1);
}

bool ChatRoom::post(uint64_t session, const std::string& address, const std::string& name,
                    const std::string& text, int64_t now_ms, ChatMessage& out,
                    std::string& error) {
    const std::string clean_text = clean_chat_text(text, kMaxTextLength);
    if (clean_text.empty()) {
        error = "empty message";
        return false;
    }
    std::string clean_name = clean_chat_text(name, kMaxNameLength);
    if (clean_name.empty()) clean_name = "anonymous";

    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        error = "the chat is off on this receiver";
        return false;
    }
    // Checked before the rate limit, so a muted address is told why rather
    // than being told it is typing too fast.
    const auto mute = mutes_.find(network_key(address));
    if (mute != mutes_.end() && (mute->second == 0 || mute->second > now_ms)) {
        error = "the operator has muted this address here";
        return false;
    }

    // Rates whose window has passed say nothing any more; dropped here so the
    // map holds only the addresses that posted in the last ten seconds.
    for (auto it = rates_.begin(); it != rates_.end();) {
        it = now_ms - it->second.window_started_ms > kWindowMs ? rates_.erase(it) : std::next(it);
    }
    const std::string wide = wide_network_key(address);
    std::vector<std::pair<Rate*, int>> limits = {{&rates_["n " + network_key(address)], kBurst},
                                                 {&rates_["*"], kEveryoneBurst}};
    if (!wide.empty()) limits.push_back({&rates_["w " + wide], kWideBurst});
    for (auto& [rate, burst] : limits) {
        if (now_ms - rate->window_started_ms > kWindowMs) {
            rate->window_started_ms = now_ms;
            rate->count = 0;
        }
        if (rate->count >= burst) {
            error = "you are sending faster than anyone can read; wait a moment";
            return false;
        }
    }
    for (auto& limit : limits) limit.first->count++;

    out.id = next_id_++;
    out.name = clean_name;
    out.text = clean_text;
    out.at_ms = now_ms;
    messages_.push_back(out);
    if (messages_.size() > kHistory) messages_.pop_front();
    history_message_.clear();
    return true;
}

std::vector<ChatMessage> ChatRoom::history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return {};
    return {messages_.begin(), messages_.end()};
}

std::string ChatRoom::history_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_message_.empty()) {
        Json out = Json::make_object();
        out.set("type", "chat-history");
        Json list = Json::make_array();
        if (enabled_) {
            for (const ChatMessage& entry : messages_) {
                Json item = Json::make_object();
                item.set("id", static_cast<double>(entry.id));
                item.set("name", entry.name);
                item.set("text", entry.text);
                item.set("at", static_cast<double>(entry.at_ms));
                list.push_back(item);
            }
        }
        out.set("messages", list);
        history_message_ = out.serialize();
    }
    return history_message_;
}

void ChatRoom::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled) messages_.clear();
    enabled_ = enabled;
    history_message_.clear();
}

bool ChatRoom::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void ChatRoom::mute(const std::string& address, int minutes, int64_t now_ms) {
    if (address.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // By network: an IPv6 host picks a new address in its /64 at will.
    mutes_[network_key(address)] = minutes > 0 ? now_ms + static_cast<int64_t>(minutes) * 60000 : 0;
}

void ChatRoom::unmute(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    mutes_.erase(network_key(address));
}

bool ChatRoom::muted(const std::string& address, int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = mutes_.find(network_key(address));
    return found != mutes_.end() && (found->second == 0 || found->second > now_ms);
}

std::vector<ChatMute> ChatRoom::mutes(int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatMute> out;
    for (const auto& [address, until] : mutes_) {
        if (until != 0 && until <= now_ms) continue;
        out.push_back({address, until});
    }
    return out;
}

Json ChatRoom::mutes_json(int64_t now_ms) const {
    Json list = Json::make_array();
    for (const ChatMute& entry : mutes(now_ms)) {
        Json item = Json::make_object();
        item.set("address", entry.address);
        item.set("until", static_cast<double>(entry.until_ms));
        list.push_back(item);
    }
    return list;
}

void ChatRoom::load_mutes(const Json& list) {
    if (!list.is_array()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    mutes_.clear();
    for (size_t i = 0; i < list.size(); i++) {
        const Json& item = list[i];
        if (!item.is_object() || !item.has("address")) continue;
        const std::string address = item["address"].string();
        if (address.empty() || address.size() > 64) continue;
        mutes_[network_key(address)] = static_cast<int64_t>(item["until"].number(0));
    }
}

}  // namespace fernsdr
