// A fixed number of equal rows of bytes, the oldest dropped when a new one
// arrives. Allocated once; a receiver runs for months and this is on the
// band thread's path.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fernsdr {

class RowRing {
public:
    RowRing(size_t width, size_t capacity) : width_(width), capacity_(capacity), rows_(width * capacity, 0) {}

    size_t width() const { return width_; }
    size_t size() const { return count_; }

    /** Copies `width()` bytes from `row` in as the newest row. */
    void push(const uint8_t* row) {
        if (capacity_ == 0) return;
        std::copy(row, row + width_, rows_.begin() + static_cast<std::ptrdiff_t>(next_ * width_));
        next_ = (next_ + 1) % capacity_;
        count_ = std::min(count_ + 1, capacity_);
    }

    /** Every row held, oldest first, into `out`; returns how many. */
    size_t copy_oldest_first(std::vector<uint8_t>& out) const {
        out.clear();
        out.reserve(count_ * width_);
        const size_t oldest = (next_ + capacity_ - count_) % (capacity_ ? capacity_ : 1);
        for (size_t n = 0; n < count_; n++) {
            const auto row = rows_.begin() + static_cast<std::ptrdiff_t>(((oldest + n) % capacity_) * width_);
            out.insert(out.end(), row, row + static_cast<std::ptrdiff_t>(width_));
        }
        return count_;
    }

private:
    size_t width_;
    size_t capacity_;
    std::vector<uint8_t> rows_;
    size_t next_ = 0;
    size_t count_ = 0;
};

}  // namespace fernsdr
