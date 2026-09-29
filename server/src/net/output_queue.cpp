#include "output_queue.h"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace fernsdr {

void OutputQueue::append(const uint8_t* data, size_t size) {
    if (size) bytes_.insert(bytes_.end(), data, data + size);
}

void OutputQueue::frame(ws::Opcode opcode, const uint8_t* payload, size_t size, int64_t now_ms, bool bulk) {
    const size_t before = bytes_.size();
    const uint64_t start = base_ + before;
    ws::encode_frame(opcode, payload, size, bytes_);
    if (opcode != ws::Opcode::Binary) return;
    const uint64_t length = bytes_.size() - before;
    // Where it may go: ahead of the unbroken run of unsent bulk frames that
    // ends the queue, and nowhere further.
    size_t index = binary_.size();
    uint64_t at = start;
    if (!bulk) {
        const uint64_t sent = base_ + offset_;
        while (index > first_binary_) {
            const Binary& last = binary_[index - 1];
            if (!last.bulk || last.start < sent || last.end != at) break;
            at = last.start;
            index--;
        }
    }
    if (at != start) {
        std::rotate(bytes_.begin() + static_cast<ptrdiff_t>(at - base_),
                    bytes_.begin() + static_cast<ptrdiff_t>(before), bytes_.end());
        for (size_t i = index; i < binary_.size(); i++) {
            binary_[i].start += length;
            binary_[i].payload += length;
            binary_[i].end += length;
        }
    }
    binary_.insert(binary_.begin() + static_cast<ptrdiff_t>(index),
                   Binary{at, at + length - size, at + length, now_ms, bulk});
}

void OutputQueue::close(uint16_t code, const std::string& reason) {
    ws::encode_close(code, reason, bytes_);
}

void OutputQueue::consume(size_t count) {
    assert(count <= size());
    offset_ += count;
    while (first_binary_ < binary_.size() && binary_[first_binary_].end <= base_ + offset_) first_binary_++;
    if (offset_ == bytes_.size()) {
        bytes_.clear();
        binary_.clear();
        first_binary_ = offset_ = base_ = 0;
    } else if (offset_ > 64 * 1024) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<ptrdiff_t>(offset_));
        base_ += offset_;
        offset_ = 0;
        binary_.erase(binary_.begin(), binary_.begin() + static_cast<ptrdiff_t>(first_binary_));
        first_binary_ = 0;
    }
}

bool OutputQueue::has_expired_binary(int64_t now_ms, int64_t minimum_age_ms) const {
    // Each kind is in the order it was queued, but a frame that is not bulk
    // may sit ahead of older bulk ones: the oldest unsent frame is the first
    // unsent one of either kind.
    const uint64_t sent = base_ + offset_;
    bool seen[2] = {false, false};
    for (size_t i = first_binary_; i < binary_.size() && !(seen[0] && seen[1]); i++) {
        const Binary& entry = binary_[i];
        if (entry.start < sent || seen[entry.bulk]) continue;
        seen[entry.bulk] = true;
        if (now_ms - entry.queued_ms >= minimum_age_ms) return true;
    }
    return false;
}

size_t OutputQueue::prune_binary(int64_t now_ms, int64_t minimum_age_ms, const KeepBinary& keep) {
    if (!has_expired_binary(now_ms, minimum_age_ms)) return 0;

    size_t read = offset_, write = offset_, retained = 0, dropped = 0;
    const auto copy_to_front = [&](size_t end) {
        if (end != read && write != read) std::memmove(bytes_.data() + write, bytes_.data() + read, end - read);
        write += end - read;
        read = end;
    };
    for (size_t i = first_binary_; i < binary_.size(); i++) {
        Binary entry = binary_[i];
        const bool started = entry.start < base_ + offset_;
        const size_t start = started ? offset_ : static_cast<size_t>(entry.start - base_);
        const size_t end = static_cast<size_t>(entry.end - base_);
        copy_to_front(start);
        const size_t removed = read - write;
        if (started || keep(bytes_.data() + static_cast<size_t>(entry.payload - base_),
                            static_cast<size_t>(entry.end - entry.payload),
                            std::max<int64_t>(0, now_ms - entry.queued_ms))) {
            copy_to_front(end);
            entry.start -= removed;
            entry.payload -= removed;
            entry.end -= removed;
            binary_[retained++] = entry;
        } else {
            read = end;
            dropped++;
        }
    }
    copy_to_front(bytes_.size());
    bytes_.resize(write);
    binary_.resize(retained);
    first_binary_ = 0;
    if (size() == 0) consume(0);
    return dropped;
}

}  // namespace fernsdr
