#include "decodes.h"

#include "directory.h"
#include "../util/password.h"

#include <algorithm>
#include <cmath>

namespace fernsdr {

namespace {

bool printable(const std::string& text, size_t limit) {
    return text.size() <= limit && std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}

bool callsign_like(const std::string& text) {
    return !text.empty() && text.size() <= 12 &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/'; });
}

}  // namespace

Json Decode::to_json() const {
    Json out = Json::make_object();
    out.set("seq", static_cast<double>(sequence));
    out.set("decoder", decoder);
    out.set("channel", channel);
    out.set("band", band);
    out.set("mode", mode);
    out.set("time", static_cast<double>(time_ms));
    out.set("dial", dial_hz);
    out.set("freq", audio_hz);
    out.set("snr", snr);
    out.set("dt", dt);
    out.set("message", message);
    if (!call.empty()) out.set("call", call);
    if (!grid.empty()) out.set("grid", grid);
    if (!report.empty()) out.set("report", report);
    out.set("quality", quality);
    return out;
}

bool parse_decode(const Json& event, const std::vector<DecodeChannel>& channels, Decode& out, std::string& why) {
    const std::string channel = event["channel"].string();
    const auto found = std::find_if(channels.begin(), channels.end(), [&](const DecodeChannel& c) { return c.id == channel; });
    if (found == channels.end()) {
        why = "names a channel it was not given";
        return false;
    }
    const double time = event["time"].number(-1.0);
    const double freq = event["freq"].number(-1.0);
    const double snr = event["snr"].number(1000.0);
    const double dt = event["dt"].number(1000.0);
    if (!(time > 0.0) || time > 4e12 || std::floor(time) != time) {
        why = "has no valid time";
        return false;
    }
    if (!(freq >= found->offset_hz - found->width_hz / 2.0 && freq <= found->offset_hz + found->width_hz / 2.0)) {
        why = "has a frequency outside its channel";
        return false;
    }
    if (!(snr >= -60.0 && snr <= 60.0) || !(dt >= -5.0 && dt <= 5.0)) {
        why = "has an snr or dt out of range";
        return false;
    }
    const std::string message = event["message"].string();
    if (message.empty() || !printable(message, 64)) {
        why = "has no message, or one that is not plain text of at most 64 characters";
        return false;
    }
    const std::string call = event["call"].string();
    if (event.has("call") && !callsign_like(call)) {
        why = "has a call that is not a callsign";
        return false;
    }
    const std::string grid = event["grid"].string();
    if (event.has("grid") && !((grid.size() == 4 || grid.size() == 6) && valid_grid_locator(grid))) {
        why = "has a grid that is not a locator";
        return false;
    }
    const std::string report = event["report"].string();
    if (event.has("report") && !printable(report, 8)) {
        why = "has a report longer than 8 characters";
        return false;
    }
    // Copied, and the default spelled out here: string()'s fallback is
    // returned by reference, and a temporary one would be gone by now.
    const std::string quality = event.has("quality") ? event["quality"].string() : std::string("bp");
    if (quality != "bp" && quality != "osd" && quality != "low") {
        why = "has an unknown quality";
        return false;
    }
    out.channel = found->id;
    out.band = found->band;
    out.mode = found->mode;
    out.dial_hz = found->dial_hz;
    out.time_ms = static_cast<int64_t>(time);
    out.audio_hz = std::round(freq * 10.0) / 10.0;
    out.snr = std::round(snr);
    out.dt = std::round(dt * 10.0) / 10.0;
    out.message = message;
    out.call = event.has("call") ? call : "";
    out.grid = event.has("grid") ? grid : "";
    out.report = event.has("report") ? report : "";
    out.quality = quality;
    return true;
}

DecodeStore::DecodeStore(size_t capacity, int64_t max_age_ms)
    : capacity_(capacity), max_age_ms_(max_age_ms), epoch_(random_hex(8)) {}

uint64_t DecodeStore::add(Decode decode, int64_t now_ms) {
    std::function<void(const Decode&)> added;
    Decode kept;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decode.sequence = next_++;
        decode.received_ms = now_ms;
        decodes_.push_back(std::move(decode));
        while (decodes_.size() > capacity_ ||
               (!decodes_.empty() && now_ms - decodes_.front().received_ms > max_age_ms_)) {
            decodes_.pop_front();
        }
        kept = decodes_.back();
        added = added_;
    }
    if (added) added(kept);
    return kept.sequence;
}

std::vector<Decode> DecodeStore::since(uint64_t after, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Decode> out;
    // Numbered in order, so the first newer one is found by bisection.
    auto first = std::upper_bound(decodes_.begin(), decodes_.end(), after,
                                  [](uint64_t value, const Decode& d) { return value < d.sequence; });
    for (auto it = first; it != decodes_.end() && out.size() < limit; ++it) out.push_back(*it);
    return out;
}

std::vector<Decode> DecodeStore::since(uint64_t after, size_t limit, const Filter& keep, uint64_t& through) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Decode> out;
    // Behind `after` only when the page read a store before this one, whose
    // numbers were higher; the epoch tells it so, and this says where to
    // start again.
    through = next_ - 1;
    auto it = std::upper_bound(decodes_.begin(), decodes_.end(), after,
                               [](uint64_t value, const Decode& d) { return value < d.sequence; });
    for (; it != decodes_.end(); ++it) {
        if (!keep(*it)) continue;
        if (out.size() == limit) {
            through = it->sequence - 1;
            break;
        }
        out.push_back(*it);
    }
    return out;
}

std::vector<Decode> DecodeStore::latest(size_t limit, const Filter& keep, uint64_t& through) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Decode> out;
    through = next_ - 1;
    for (auto it = decodes_.rbegin(); it != decodes_.rend() && out.size() < limit; ++it) {
        if (keep(*it)) out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

uint64_t DecodeStore::last_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_ - 1;
}

size_t DecodeStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decodes_.size();
}

void DecodeStore::set_added_callback(std::function<void(const Decode&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    added_ = std::move(callback);
}

}  // namespace fernsdr
