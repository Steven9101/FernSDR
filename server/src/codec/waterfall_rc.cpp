#include "waterfall_rc.h"

#include <cstdlib>

#include "waterfall_codec.h"

namespace fernsdr {
namespace wfc {

namespace {

constexpr uint16_t kHalf = 1024;
constexpr int kProbBits = 11;
constexpr int kAdapt = 5;
constexpr uint32_t kTop = 1u << 24;
// Magnitudes past this many unary steps go out as plain bits.
constexpr int kUnary = 12;

int bucket(int v) {
    v = std::abs(v);
    return v == 0 ? 0 : v == 1 ? 1 : v <= 3 ? 2 : 3;
}

// LZMA's range encoder. Its first output byte is always zero and is left out.
class RangeEncoder {
public:
    explicit RangeEncoder(std::vector<uint8_t>& out) : out_(out) {}

    void bit(uint16_t& p, int b) {
        const uint32_t bound = (range_ >> kProbBits) * p;
        if (!b) {
            range_ = bound;
            p = static_cast<uint16_t>(p + (((1u << kProbBits) - p) >> kAdapt));
        } else {
            low_ += bound;
            range_ -= bound;
            p = static_cast<uint16_t>(p - (p >> kAdapt));
        }
        while (range_ < kTop) { range_ <<= 8; shift_low(); }
    }

    void direct(uint32_t value, int bits) {
        while (bits--) {
            range_ >>= 1;
            if ((value >> bits) & 1) low_ += range_;
            while (range_ < kTop) { range_ <<= 8; shift_low(); }
        }
    }

    void finish() {
        for (int i = 0; i < 5; i++) shift_low();
    }

private:
    void shift_low() {
        if (static_cast<uint32_t>(low_) < 0xFF000000u || (low_ >> 32) != 0) {
            uint8_t temp = cache_;
            do {
                const uint8_t byte = static_cast<uint8_t>(temp + static_cast<uint8_t>(low_ >> 32));
                if (started_) out_.push_back(byte);
                started_ = true;
                temp = 0xFF;
            } while (--cache_size_ != 0);
            cache_ = static_cast<uint8_t>(low_ >> 24);
        }
        cache_size_++;
        low_ = (low_ & 0x00FFFFFFu) << 8;
    }

    std::vector<uint8_t>& out_;
    uint64_t low_ = 0;
    uint32_t range_ = 0xFFFFFFFFu;
    uint8_t cache_ = 0;
    uint64_t cache_size_ = 1;
    bool started_ = false;
};

class RangeDecoder {
public:
    RangeDecoder(const uint8_t* data, size_t size) : data_(data), size_(size) {
        for (int i = 0; i < 4; i++) code_ = (code_ << 8) | next();
    }

    int bit(uint16_t& p) {
        const uint32_t bound = (range_ >> kProbBits) * p;
        int b;
        if (code_ < bound) {
            range_ = bound;
            p = static_cast<uint16_t>(p + (((1u << kProbBits) - p) >> kAdapt));
            b = 0;
        } else {
            code_ -= bound;
            range_ -= bound;
            p = static_cast<uint16_t>(p - (p >> kAdapt));
            b = 1;
        }
        while (range_ < kTop) { range_ <<= 8; code_ = (code_ << 8) | next(); }
        return b;
    }

    uint32_t direct(int bits) {
        uint32_t value = 0;
        while (bits--) {
            range_ >>= 1;
            uint32_t b = 0;
            if (code_ >= range_) { code_ -= range_; b = 1; }
            value = (value << 1) | b;
            while (range_ < kTop) { range_ <<= 8; code_ = (code_ << 8) | next(); }
        }
        return value;
    }

    // More than the finishing bytes read past the end: the payload was cut.
    bool overrun() const { return past_ > 4; }

private:
    uint32_t next() {
        if (pos_ < size_) return data_[pos_++];
        past_++;
        return 0;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    size_t past_ = 0;
    uint32_t range_ = 0xFFFFFFFFu;
    uint32_t code_ = 0;
};

// The contexts of bin i, shared by encoder and decoder. `prev` is null on a
// key row.
struct Context {
    int above_bucket, direction, last_bucket, last_sign;
};

Context context_of(const int* prev, size_t i, int left, int last) {
    Context c{};
    if (prev) {
        const int above = prev[i];
        c.above_bucket = bucket(above - left);
        c.direction = above > left ? 1 : above < left ? 2 : 0;
    } else {
        c.above_bucket = 4;
        c.direction = 3;
    }
    c.last_bucket = bucket(last);
    c.last_sign = last > 0 ? 1 : last < 0 ? 2 : 0;
    return c;
}

int agreement(const int* prev, size_t i, int left, int residual) {
    if (!prev || prev[i] == left) return 0;
    return (prev[i] > left) == (residual > 0) ? 1 : 2;
}

}  // namespace

void RangedModel::reset() {
    for (auto& a : zero) for (auto& b : a) for (auto& p : b) p = kHalf;
    for (auto& a : sign) for (auto& b : a) for (auto& p : b) p = kHalf;
    for (auto& a : magnitude) for (auto& b : a) for (auto& p : b) p = kHalf;
}

void RangedLineEncoder::reset() {
    previous_.clear();
    since_key_ = 0;
}

const std::vector<uint8_t>& RangedLineEncoder::encode(const float* db, size_t width, bool force_key, int step_db) {
    frame_.clear();
    if (width < 1 || width > 4096 || (step_db != 1 && step_db != 2)) { reset(); return frame_; }
    current_.resize(width);
    for (size_t i = 0; i < width; i++) current_[i] = quantise_db(db[i], step_db);

    const bool key = force_key || previous_.size() != width || step_db != step_db_ || since_key_ >= kKeyEvery;
    if (key) { model_.reset(); since_key_ = 0; }
    step_db_ = step_db;
    last_key_ = key;
    const int* prev = key ? nullptr : previous_.data();
    const int seed = kIntraSeedQ / step_db;

    frame_.push_back(key ? kRangedKey : 0);
    RangeEncoder rc(frame_);
    int last = 0;
    for (size_t i = 0; i < width; i++) {
        const int left = i ? current_[i - 1] : (prev ? prev[0] : seed);
        const Context c = context_of(prev, i, left, last);
        const int residual = current_[i] - left;
        rc.bit(model_.zero[c.above_bucket][c.last_bucket][c.direction], residual != 0);
        if (residual != 0) {
            rc.bit(model_.sign[c.above_bucket][c.direction][c.last_sign], residual < 0);
            const int magnitude = std::abs(residual) - 1;
            const int agree = agreement(prev, i, left, residual);
            int j = 0;
            for (; j < kUnary; j++) {
                const bool stop = magnitude == j;
                rc.bit(model_.magnitude[j < 4 ? j : 4][c.above_bucket][agree], !stop);
                if (stop) break;
            }
            if (j == kUnary) {
                // Exp-Golomb of the rest, as plain bits: the width, then the
                // bits below the leading one.
                const uint32_t n = static_cast<uint32_t>(magnitude - kUnary + 1);
                int bits = 0;
                while ((n >> bits) > 1) bits++;
                rc.direct(static_cast<uint32_t>(bits), 4);
                rc.direct(n & ((1u << bits) - 1), bits);
            }
        }
        last = residual;
    }
    rc.finish();
    previous_ = current_;
    since_key_++;
    return frame_;
}

void RangedLineDecoder::reset() {
    previous_.clear();
    ready_ = false;
}

bool RangedLineDecoder::decode(const uint8_t* data, size_t size, size_t width, float* out, int step_db) {
    if (size < 1 || width < 1 || width > 4096 || (step_db != 1 && step_db != 2) || (data[0] & ~kRangedKey)) {
        reset();
        return false;
    }
    const bool key = data[0] & kRangedKey;
    if (!key && (!ready_ || previous_.size() != width || step_db != step_db_)) { reset(); return false; }
    if (key) model_.reset();
    const int* prev = key ? nullptr : previous_.data();
    const int low = kMinLevelQ / step_db, high = kMaxLevelQ / step_db, seed = kIntraSeedQ / step_db;

    current_.resize(width);
    RangeDecoder rc(data + 1, size - 1);
    int last = 0;
    for (size_t i = 0; i < width; i++) {
        const int left = i ? current_[i - 1] : (prev ? prev[0] : seed);
        const Context c = context_of(prev, i, left, last);
        int residual = 0;
        if (rc.bit(model_.zero[c.above_bucket][c.last_bucket][c.direction])) {
            const bool negative = rc.bit(model_.sign[c.above_bucket][c.direction][c.last_sign]);
            // The agreement context needs the sign, which is known by now.
            const int agree = agreement(prev, i, left, negative ? -1 : 1);
            int magnitude = 0;
            while (magnitude < kUnary && rc.bit(model_.magnitude[magnitude < 4 ? magnitude : 4][c.above_bucket][agree])) {
                magnitude++;
            }
            if (magnitude == kUnary) {
                const int bits = static_cast<int>(rc.direct(4));
                if (bits > 9) { reset(); return false; }
                magnitude += static_cast<int>(((1u << bits) | rc.direct(bits)) - 1);
            }
            residual = negative ? -(magnitude + 1) : magnitude + 1;
        }
        const int value = left + residual;
        if (value < low || value > high || rc.overrun()) { reset(); return false; }
        current_[i] = value;
        last = residual;
    }
    for (size_t i = 0; i < width; i++) out[i] = static_cast<float>(current_[i] * step_db);
    previous_ = current_;
    step_db_ = step_db;
    ready_ = true;
    return true;
}

}  // namespace wfc
}  // namespace fernsdr
