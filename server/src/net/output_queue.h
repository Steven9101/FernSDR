#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "websocket.h"

namespace fernsdr {

// One contiguous write buffer, with boundaries for binary messages that the
// application may discard. HTTP and WebSocket control bytes remain in FIFO
// order. Once any byte of a frame has left, its remainder is immutable.
class OutputQueue {
public:
    void append(const uint8_t* data, size_t size);
    // A `bulk` binary frame gives way: a later binary frame that is not bulk
    // goes ahead of the unsent bulk frames at the end of the queue, so on a
    // link that cannot keep up, audio is not held behind waterfall rows. It
    // never moves ahead of text, control or HTTP bytes, of a frame already
    // started, or of an earlier frame that is not bulk: each kind keeps its
    // own order, and a message that describes what follows stays in front.
    void frame(ws::Opcode opcode, const uint8_t* payload, size_t size, int64_t now_ms = 0, bool bulk = false);
    void close(uint16_t code, const std::string& reason);

    const uint8_t* data() const { return bytes_.data() + offset_; }
    size_t size() const { return bytes_.size() - offset_; }
    void consume(size_t count);
    bool has_expired_binary(int64_t now_ms, int64_t minimum_age_ms) const;

    using KeepBinary = std::function<bool(uint8_t* payload, size_t size, int64_t age_ms)>;
    // The callback may change a complete unsent payload, but cannot append to
    // this queue. Return false to remove that entire frame. Young frames are
    // visited after an old one so the application can repair dependencies.
    size_t prune_binary(int64_t now_ms, int64_t minimum_age_ms, const KeepBinary& keep);

private:
    struct Binary {
        uint64_t start, payload, end;
        int64_t queued_ms;
        bool bulk;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Binary> binary_;
    size_t first_binary_ = 0;
    size_t offset_ = 0;
    // Frame positions stay absolute when a consumed prefix is compacted.
    // Rebasing a partial frame's start to zero would make it look unsent.
    // Use 64 bits even on 32-bit hosts: a queue that never quite drains can
    // transmit more than 4 GiB during an ordinary receiver uptime.
    uint64_t base_ = 0;
};

}  // namespace fernsdr
