// SHA-512 (FIPS 180-4), for Ed25519. Written like Sha256 in password.h and
// checked against the standard's examples in test_ed25519.cpp.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace fernsdr {

class Sha512 {
public:
    Sha512() { reset(); }

    void reset() {
        static const uint64_t initial[8] = {
            0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
            0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
        };
        std::memcpy(state_, initial, sizeof(state_));
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

    void update(const std::string& text) { update(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }

    void finish(uint8_t digest[64]) {
        // The length is 128 bits; a message of 2^61 bytes or more is not a
        // thing this program hashes, so the high half is zero.
        const uint64_t bits = length_ * 8;
        const uint8_t one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while (buffered_ != 112) update(&zero, 1);
        uint8_t tail[16] = {};
        for (int i = 0; i < 8; i++) tail[8 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(tail, 16);
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) digest[i * 8 + j] = static_cast<uint8_t>(state_[i] >> (56 - 8 * j));
        }
    }

private:
    static uint64_t rotr(uint64_t v, int n) { return (v >> n) | (v << (64 - n)); }

    void transform(const uint8_t block[128]) {
        static const uint64_t k[80] = {
            0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
            0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
            0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
            0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
            0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
            0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
            0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
            0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
            0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
            0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
            0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
            0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
            0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
            0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
            0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
            0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
            0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
            0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
            0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
            0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
            0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
            0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
            0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
            0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
            0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
            0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
            0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
        };
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | block[i * 8 + j];
        }
        for (int i = 16; i < 80; i++) {
            const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 80; i++) {
            const uint64_t t1 = h + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint64_t t2 = (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    uint64_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[128];
    size_t buffered_ = 0;
};

}  // namespace fernsdr
