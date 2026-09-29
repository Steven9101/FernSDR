// Password hashing for the admin panel: SHA-256, HMAC-SHA256, PBKDF2 and a
// constant-time compare, in about two hundred lines.
//
// Written here for the same reason as everything else in this tree: linking
// OpenSSL to hash one password would turn a server that builds anywhere into
// one that needs a dependency resolved first. What is NOT done here is
// invention - SHA-256 is FIPS 180-4 and PBKDF2 is RFC 8018, both implemented
// to the letter and checked against the published test vectors in
// tests/test_password.cpp.
//
// PBKDF2 rather than a plain hash because an admin password is guessable and
// a single SHA-256 can be tried billions of times a second on a GPU. PBKDF2
// with a large iteration count makes each guess cost real time. It is not
// Argon2 - nothing here is memory-hard - but it is the strongest thing that
// fits in a self-contained header, and it is used behind a login rate limiter
// rather than as the only line of defence.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fernsdr {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
        length_ = 0;
        buffered_ = 0;
    }

    void update(const uint8_t* data, size_t length) {
        length_ += length;
        while (length > 0) {
            const size_t take = std::min(sizeof(buffer_) - buffered_, length);
            std::memcpy(buffer_ + buffered_, data, take);
            buffered_ += take;
            data += take;
            length -= take;
            if (buffered_ == sizeof(buffer_)) {
                transform(buffer_);
                buffered_ = 0;
            }
        }
    }

    void update(const std::string& text) {
        update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    void finish(uint8_t digest[32]) {
        const uint64_t bits = length_ * 8;
        const uint8_t one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while (buffered_ != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; i++) tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(tail, 8);
        for (int i = 0; i < 8; i++) {
            digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
    }

private:
    static uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

    void transform(const uint8_t block[64]) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + choose + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t buffered_ = 0;
};

inline void hmac_sha256(const uint8_t* key, size_t key_length, const uint8_t* message,
                        size_t message_length, uint8_t out[32]) {
    uint8_t block[64] = {0};
    if (key_length > 64) {
        Sha256 hash;
        hash.update(key, key_length);
        hash.finish(block);
    } else {
        std::memcpy(block, key, key_length);
    }

    uint8_t inner_pad[64], outer_pad[64];
    for (int i = 0; i < 64; i++) {
        inner_pad[i] = static_cast<uint8_t>(block[i] ^ 0x36);
        outer_pad[i] = static_cast<uint8_t>(block[i] ^ 0x5c);
    }

    uint8_t inner[32];
    Sha256 hash;
    hash.update(inner_pad, 64);
    hash.update(message, message_length);
    hash.finish(inner);

    Sha256 outer;
    outer.update(outer_pad, 64);
    outer.update(inner, 32);
    outer.finish(out);
}

/** PBKDF2-HMAC-SHA256, RFC 8018. `out` may be any length. */
inline void pbkdf2_sha256(const std::string& password, const std::string& salt, int iterations,
                          uint8_t* out, size_t out_length) {
    const uint8_t* key = reinterpret_cast<const uint8_t*>(password.data());
    uint32_t block_index = 1;
    size_t produced = 0;

    while (produced < out_length) {
        std::vector<uint8_t> message(salt.begin(), salt.end());
        message.push_back(static_cast<uint8_t>(block_index >> 24));
        message.push_back(static_cast<uint8_t>(block_index >> 16));
        message.push_back(static_cast<uint8_t>(block_index >> 8));
        message.push_back(static_cast<uint8_t>(block_index));

        uint8_t current[32];
        hmac_sha256(key, password.size(), message.data(), message.size(), current);
        uint8_t accumulator[32];
        std::memcpy(accumulator, current, 32);

        for (int iteration = 1; iteration < iterations; iteration++) {
            hmac_sha256(key, password.size(), current, 32, current);
            for (int i = 0; i < 32; i++) accumulator[i] ^= current[i];
        }

        const size_t take = std::min<size_t>(32, out_length - produced);
        std::memcpy(out + produced, accumulator, take);
        produced += take;
        block_index++;
    }
}

/**
 * Compares without leaking where the difference is.
 *
 * A byte-by-byte compare that returns early tells an attacker, through timing,
 * how much of a guess was right - which turns an infeasible search into a
 * feasible one, one byte at a time.
 */
inline bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); i++) {
        difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return difference == 0;
}

inline std::string to_hex(const uint8_t* data, size_t length) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

/** Random bytes from the kernel, as hex. Used for salts and session tokens. */
std::string random_hex(size_t bytes);

/**
 * A stored password hash: "pbkdf2$<iterations>$<salt-hex>$<derived-hex>".
 *
 * Self-describing so the iteration count can be raised later without
 * invalidating existing hashes.
 */
// 600,000 is OWASP's current figure for PBKDF2-HMAC-SHA256. The browser does
// the work at sign-in, where it takes a fraction of a second.
std::string hash_password(const std::string& password, int iterations = 600000);
bool verify_password(const std::string& password, const std::string& stored);

/**
 * Splits a stored hash into its parts.
 *
 * The challenge-response login needs the salt and iteration count to tell the
 * browser how to derive the same key, and the key itself to check the proof
 * against - none of which `verify_password` can give it, because that takes a
 * password this scheme deliberately never receives.
 */
bool parse_password_hash(const std::string& stored, int& iterations, std::string& salt,
                         std::string& derived_hex);

/** Decodes lower-case hex. Returns false on odd length or a non-hex digit. */
inline bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int value = 0;
        for (int half = 0; half < 2; half++) {
            const char c = hex[i + half];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = value * 16 + digit;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

}  // namespace fernsdr
