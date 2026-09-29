// MSB-first bit packing plus the entropy codes shared by the audio and
// waterfall codecs.  The exact same code is mirrored in web/src/dsp/bitio.ts;
// any change here must be made there too (tests/test_bitio.cpp pins the
// bitstream so accidental divergence is caught).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace fernsdr {

// Unary run length at which Rice coding gives up and stores the value raw.
static constexpr uint32_t kRiceEscape = 24;

inline uint32_t zigzag_encode(int32_t v) {
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}
inline int32_t zigzag_decode(uint32_t u) {
    return static_cast<int32_t>(u >> 1) ^ -static_cast<int32_t>(u & 1);
}

// Number of bits a Rice code would occupy, without emitting anything.  The
// rate-control loops call this a lot, so it stays branch-light.
inline uint32_t rice_cost(uint32_t value, uint32_t k) {
    uint32_t q = value >> k;
    if (q >= kRiceEscape) return kRiceEscape + 1 + 32;
    return q + 1 + k;
}

class BitWriter {
public:
    explicit BitWriter(size_t reserve_bytes = 256) { buf_.reserve(reserve_bytes); }

    void put_bit(uint32_t bit) { put_bits(bit & 1u, 1); }

    // Writes the low `count` bits of `value`, most significant first. Up to
    // 32 bits go into the 64-bit accumulator at once, and leave it as a word
    // once 32 are pending; the codecs write one of these per coefficient.
    // Worked on in locals: a byte stored into the buffer may alias anything,
    // and with the accumulator in the object it was stored and reloaded
    // around every byte.
    void put_bits(uint32_t value, uint32_t count) {
        if (count == 0) return;
        const uint64_t masked = count >= 32 ? value : value & ((1u << count) - 1u);
        uint64_t acc = (acc_ << count) | masked;
        uint32_t nbits = nbits_ + count;
        if (nbits >= 32) {
            nbits -= 32;
            put_word(static_cast<uint32_t>(acc >> nbits));
            acc &= (uint64_t{1} << nbits) - 1;
        }
        acc_ = acc;
        nbits_ = nbits;
    }

    void put_rice(uint32_t value, uint32_t k) {
        uint32_t q = value >> k;
        if (q >= kRiceEscape) {
            put_bits((1u << kRiceEscape) - 1, kRiceEscape);
            put_bit(0);
            put_bits(value, 32);
            return;
        }
        // q ones, the terminating zero and the k low bits are one field of
        // q + 1 + k bits, written at once when it fits a single call.
        const uint32_t unary = ((1u << q) - 1) << 1;
        if (q + 1 + k <= 32) {
            put_bits((unary << k) | (value & ((uint64_t{1} << k) - 1)), q + 1 + k);
            return;
        }
        put_bits(unary, q + 1);
        put_bits(value, k);
    }

    void put_signed_rice(int32_t value, uint32_t k) { put_rice(zigzag_encode(value), k); }

    // put_signed_rice() for `count` values with one parameter, the bits
    // exactly those of calling it for each in turn. The codecs write a run
    // like this per band or per waterfall row, and keeping the accumulator
    // in registers for the whole run, instead of storing it back to the
    // object after every value, is most of what writing one costs.
    void put_signed_rice_run(const int32_t* values, size_t count, uint32_t k) {
        uint64_t acc = acc_;
        uint32_t nbits = nbits_;
        const uint64_t low_mask = (uint64_t{1} << k) - 1;
        for (size_t i = 0; i < count; i++) {
            const uint32_t value = zigzag_encode(values[i]);
            const uint32_t q = value >> k;
            const uint32_t length = q + 1 + k;
            if (q >= kRiceEscape || length > 32) {
                // The escape, or a field too long for one call: rare, and
                // left to the general path.
                acc_ = acc;
                nbits_ = nbits;
                put_rice(value, k);
                acc = acc_;
                nbits = nbits_;
                continue;
            }
            const uint64_t unary = ((uint64_t{1} << q) - 1) << 1;
            acc = (acc << length) | (unary << k) | (value & low_mask);
            nbits += length;
            if (nbits >= 32) {
                nbits -= 32;
                put_word(static_cast<uint32_t>(acc >> nbits));
                acc &= (uint64_t{1} << nbits) - 1;
            }
        }
        acc_ = acc;
        nbits_ = nbits;
    }

    // Order-0 Exp-Golomb: used where the value range is unknown up front
    // (scale-factor deltas, header fields).
    void put_exp_golomb(uint32_t value) {
        uint32_t v = value + 1;
        uint32_t nbits = 0;
        while ((v >> nbits) > 1u) nbits++;
        for (uint32_t i = 0; i < nbits; i++) put_bit(0);
        put_bits(v, nbits + 1);
    }

    void put_signed_exp_golomb(int32_t value) { put_exp_golomb(zigzag_encode(value)); }

    // Pads the final partial byte with zeros and returns the finished buffer.
    const std::vector<uint8_t>& finish() {
        if (nbits_ % 8) put_bits(0, 8 - nbits_ % 8);
        while (nbits_ >= 8) {
            nbits_ -= 8;
            buf_.push_back(static_cast<uint8_t>(acc_ >> nbits_));
        }
        acc_ = 0;
        return buf_;
    }

    size_t bits_written() const { return buf_.size() * 8 + nbits_; }
    void reset() { buf_.clear(); acc_ = 0; nbits_ = 0; }

    // Residuals as the waterfall codec writes them with zero runs: each
    // maximal run of zeros as a 0 bit and the run's length less one in
    // order-0 Exp-Golomb, every other value as a 1 bit and its signed Rice
    // code. The bits are exactly those of put_bit, put_exp_golomb and
    // put_signed_rice in turn; each code goes in as one field, with the
    // accumulator in registers for the whole row.
    void put_signed_rice_with_zero_runs(const int32_t* values, size_t count, uint32_t k) {
        uint64_t acc = acc_;
        uint32_t nbits = nbits_;
        const auto field = [&](uint64_t bits, uint32_t length) {
            acc = (acc << length) | bits;
            nbits += length;
            if (nbits >= 32) {
                nbits -= 32;
                put_word(static_cast<uint32_t>(acc >> nbits));
                acc &= (uint64_t{1} << nbits) - 1;
            }
        };
        const uint64_t low_mask = (uint64_t{1} << k) - 1;
        for (size_t i = 0; i < count;) {
            if (values[i] == 0) {
                size_t run = 1;
                while (i + run < count && values[i + run] == 0) run++;
                i += run;
                // The 0 bit, floor(log2(run)) zeros and run itself: run in a
                // field of twice floor(log2(run)) plus two bits.
                uint32_t magnitude = 0;
                while ((run >> magnitude) > 1) magnitude++;
                if (2 * magnitude + 2 <= 32) {
                    field(run, 2 * magnitude + 2);
                } else {
                    acc_ = acc;
                    nbits_ = nbits;
                    put_bit(0);
                    put_exp_golomb(static_cast<uint32_t>(run - 1));
                    acc = acc_;
                    nbits = nbits_;
                }
                continue;
            }
            const uint32_t value = zigzag_encode(values[i++]);
            const uint32_t q = value >> k;
            if (q >= kRiceEscape || q + 2 + k > 32) {
                acc_ = acc;
                nbits_ = nbits;
                put_bit(1);
                put_rice(value, k);
                acc = acc_;
                nbits = nbits_;
                continue;
            }
            // The 1 bit and the quotient's q ones are q + 1 ones, then the
            // terminating zero and the k low bits.
            const uint64_t ones = ((uint64_t{1} << (q + 1)) - 1) << 1;
            field((ones << k) | (value & low_mask), q + 2 + k);
        }
        acc_ = acc;
        nbits_ = nbits;
    }

private:
    // Four bytes, most significant first. Pushed one at a time: the fast
    // path of push_back is inline, where inserting a range called out to the
    // library for every word.
    void put_word(uint32_t word) {
        buf_.push_back(static_cast<uint8_t>(word >> 24));
        buf_.push_back(static_cast<uint8_t>(word >> 16));
        buf_.push_back(static_cast<uint8_t>(word >> 8));
        buf_.push_back(static_cast<uint8_t>(word));
    }

    std::vector<uint8_t> buf_;
    uint64_t acc_ = 0;   // fewer than 32 pending bits between calls
    uint32_t nbits_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Reads past the end yield zero bits and latch the overrun flag, so a
    // truncated or corrupt frame degrades instead of reading out of bounds.
    uint32_t get_bit() {
        if (pos_ >= size_ * 8) { overrun_ = true; return 0; }
        uint32_t byte = data_[pos_ >> 3];
        uint32_t bit = (byte >> (7 - (pos_ & 7))) & 1u;
        pos_++;
        return bit;
    }

    uint32_t get_bits(uint32_t count) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < count; i++) v = (v << 1) | get_bit();
        return v;
    }

    uint32_t get_rice(uint32_t k) {
        uint32_t q = 0;
        while (get_bit()) {
            if (++q >= kRiceEscape) {
                get_bit();  // consume the terminating zero the escape still writes
                return get_bits(32);
            }
            if (overrun_) return 0;
        }
        return (q << k) | (k ? get_bits(k) : 0u);
    }

    int32_t get_signed_rice(uint32_t k) { return zigzag_decode(get_rice(k)); }

    uint32_t get_exp_golomb() {
        uint32_t nbits = 0;
        while (!get_bit()) {
            if (++nbits > 32 || overrun_) { overrun_ = true; return 0; }
        }
        // The terminating 1 is the leading bit of the value.
        uint32_t v = 1;
        for (uint32_t i = 0; i < nbits; i++) v = (v << 1) | get_bit();
        return v - 1;
    }

    int32_t get_signed_exp_golomb() { return zigzag_decode(get_exp_golomb()); }

    bool overrun() const { return overrun_; }
    size_t bits_read() const { return pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool overrun_ = false;
};

}  // namespace fernsdr
