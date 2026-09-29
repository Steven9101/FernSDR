// SHA-1 and base64, needed only for the WebSocket handshake.  Both are tiny,
// and pulling in OpenSSL for 30 lines of hashing would undercut the point of
// a server that builds anywhere with nothing installed.
//
// SHA-1 is used here purely as the fixed, non-cryptographic transformation
// RFC 6455 specifies for Sec-WebSocket-Accept.  Nothing security-bearing
// depends on it.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace fernsdr {

inline void sha1(const uint8_t* data, size_t length, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    const uint64_t bit_length = static_cast<uint64_t>(length) * 8;
    const size_t padded = ((length + 8) / 64 + 1) * 64;

    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint8_t block[64];
        for (size_t i = 0; i < 64; i++) {
            const size_t index = chunk + i;
            if (index < length) block[i] = data[index];
            else if (index == length) block[i] = 0x80;
            else if (index >= padded - 8) block[i] = static_cast<uint8_t>(bit_length >> (8 * (padded - 1 - index)));
            else block[i] = 0;
        }

        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    for (int i = 0; i < 5; i++) {
        digest[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

inline std::string base64_encode(const uint8_t* data, size_t length) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    for (size_t i = 0; i < length; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < length ? data[i + 1] : 0;
        const uint32_t c = i + 2 < length ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out += table[(triple >> 18) & 0x3F];
        out += table[(triple >> 12) & 0x3F];
        out += i + 1 < length ? table[(triple >> 6) & 0x3F] : '=';
        out += i + 2 < length ? table[triple & 0x3F] : '=';
    }
    return out;
}

// False on anything but base64 with its padding; whitespace is not allowed.
inline bool base64_decode(const std::string& text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    out.reserve(text.size() / 4 * 3);
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=') : 0;
        if (pad == 1 && text[i + 2] == '=') return false;
        int v[4];
        for (int k = 0; k < 4; k++) {
            v[k] = k >= 4 - pad ? 0 : value(text[i + k]);
            if (v[k] < 0) return false;
        }
        const uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                                (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out += static_cast<char>((triple >> 16) & 0xFF);
        if (pad < 2) out += static_cast<char>((triple >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(triple & 0xFF);
    }
    return true;
}

}  // namespace fernsdr
