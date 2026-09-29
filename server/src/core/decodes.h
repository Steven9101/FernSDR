// What decoder modules heard, kept for the pages that show it: checked field
// by field on the way in, bounded in count and age, and numbered so a page
// can ask for what it has not seen yet.
#pragma once

#include "../util/json.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace fernsdr {

struct Decode {
    uint64_t sequence = 0;
    std::string decoder;   // the [decoder:<id>] it came from
    std::string channel;
    std::string band;
    std::string mode;
    int64_t time_ms = 0;   // UTC start of its slot
    double dial_hz = 0.0;
    double audio_hz = 0.0; // above the dial
    double snr = 0.0;
    double dt = 0.0;
    std::string message;
    std::string call;
    std::string grid;
    std::string report;
    std::string quality;   // bp, osd or low
    int64_t received_ms = 0;

    Json to_json() const;
};

// A decoder's `decode` event, checked against the channel it names. Returns
// false, and why, for anything the contract does not allow; see
// docs/MODULES.md, "Decodes".
struct DecodeChannel {
    std::string id;
    std::string band;
    std::string mode;
    double dial_hz = 0.0;
    double offset_hz = 0.0;
    double width_hz = 0.0;
};
bool parse_decode(const Json& event, const std::vector<DecodeChannel>& channels, Decode& out, std::string& why);

class DecodeStore {
public:
    explicit DecodeStore(size_t capacity = 20000, int64_t max_age_ms = 24LL * 3600 * 1000);

    // Keeps a decode and numbers it; `now_ms` also ages out the oldest.
    uint64_t add(Decode decode, int64_t now_ms);
    // Decodes numbered after `after`, oldest first, at most `limit`.
    std::vector<Decode> since(uint64_t after, size_t limit) const;
    // The same, keeping only those `keep` passes. `through` is the last number
    // looked at, passed or not, so a page that asks again after it neither
    // misses a decode nor rereads the ones it was not shown.
    using Filter = std::function<bool(const Decode&)>;
    std::vector<Decode> since(uint64_t after, size_t limit, const Filter& keep, uint64_t& through) const;
    // The newest `limit` that `keep` passes, oldest first: what a page opening
    // shows before it follows along with since().
    std::vector<Decode> latest(size_t limit, const Filter& keep, uint64_t& through) const;
    uint64_t last_sequence() const;
    // Chosen when the store is made, so a page can tell a restarted receiver,
    // whose numbers start again at 1, from one that has had no news.
    const std::string& epoch() const { return epoch_; }
    size_t size() const;

    // Called with every decode kept, outside the store's lock.
    void set_added_callback(std::function<void(const Decode&)> callback);

private:
    mutable std::mutex mutex_;
    std::deque<Decode> decodes_;
    size_t capacity_;
    int64_t max_age_ms_;
    uint64_t next_ = 1;
    std::string epoch_;
    std::function<void(const Decode&)> added_;
};

}  // namespace fernsdr
