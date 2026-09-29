// RDS as a station sends it, built here independently of the decoder, for
// the decoder's tests and the receiver's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rds_signal {

// The check word, computed here independently of the decoder, by long
// division of data x^10 by the generator 0x5b9.
inline uint16_t check(uint16_t data) {
    uint32_t value = static_cast<uint32_t>(data) << 10;
    for (int bit = 25; bit >= 10; bit--) {
        if (value & (1u << bit)) value ^= 0x5b9u << (bit - 10);
    }
    return static_cast<uint16_t>(value);
}

constexpr uint16_t kOffsetA = 0x0fc, kOffsetB = 0x198, kOffsetC = 0x168, kOffsetCPrime = 0x350, kOffsetD = 0x1b4;

inline void append_block(std::vector<int>& bits, uint16_t data, uint16_t offset) {
    const uint32_t block = static_cast<uint32_t>(data) << 10 | (check(data) ^ offset);
    for (int i = 25; i >= 0; i--) bits.push_back((block >> i) & 1);
}

// Groups for a station: PS in 0A, radiotext in 2A (or 2B), PI, PTY, TP.
inline std::vector<int> groups(uint16_t pi, int pty, const std::string& ps, const std::string& rt, bool rt_b = false,
                               int rt_flag = 0) {
    std::vector<int> bits;
    const uint16_t common = static_cast<uint16_t>(1 << 10 | pty << 5);
    for (int s = 0; s < 4; s++) {
        append_block(bits, pi, kOffsetA);
        append_block(bits, static_cast<uint16_t>(0 << 12 | common | s), kOffsetB);
        append_block(bits, 0xe0cd, kOffsetC);  // AF codes, not read
        append_block(bits, static_cast<uint16_t>(static_cast<uint8_t>(ps[2 * s]) << 8 | static_cast<uint8_t>(ps[2 * s + 1])), kOffsetD);
    }
    std::string text = rt + "\r";
    const int per = rt_b ? 2 : 4;
    while (text.size() % per) text += ' ';
    for (size_t s = 0; s * per < text.size(); s++) {
        const auto c = [&](size_t k) { return static_cast<uint8_t>(text[s * per + k]); };
        append_block(bits, pi, kOffsetA);
        const uint16_t b = static_cast<uint16_t>(2 << 12 | (rt_b ? 1 << 11 : 0) | common | rt_flag << 4 | s);
        append_block(bits, b, kOffsetB);
        if (rt_b) {
            append_block(bits, pi, kOffsetCPrime);
            append_block(bits, static_cast<uint16_t>(c(0) << 8 | c(1)), kOffsetD);
        } else {
            append_block(bits, static_cast<uint16_t>(c(0) << 8 | c(1)), kOffsetC);
            append_block(bits, static_cast<uint16_t>(c(2) << 8 | c(3)), kOffsetD);
        }
    }
    return bits;
}

// Differential coding, then each bit as a chip and its opposite: the
// biphase symbols the 57 kHz carrier is keyed with, at 2375 a second.
inline std::vector<int> rds_chips(const std::vector<int>& bits) {
    std::vector<int> chips;
    int d = 0;
    for (const int b : bits) {
        d ^= b;
        chips.push_back(d ? 1 : -1);
        chips.push_back(d ? -1 : 1);
    }
    return chips;
}

}  // namespace rds_signal
