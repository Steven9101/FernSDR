#include "ed25519.h"

#include <cstring>

#include "sha512.h"

// Written from RFC 8032. A field element mod p = 2^255 - 19 is kept as
// sixteen 16-bit limbs, low first, each in a signed 64-bit integer, the
// representation TweetNaCl made well known: sums and differences need no
// carrying, and a product of two such elements accumulates in 64 bits without
// overflowing. Points are in extended coordinates (X:Y:Z:T, x = X/Z, y = Y/Z,
// xy = T/Z), where one formula adds any two points, a point to itself
// included. Scalar multiplication goes through all 256 bits with the same
// operations whatever the bits are, so signing takes the same time for every
// key; verifying handles only public data.
namespace fernsdr {

namespace {

struct Fe {
    int64_t v[16] = {};
};

Fe small(int64_t value) {
    Fe r;
    r.v[0] = value;
    return r;
}

// Moves what lies above 16 bits in each limb into the next. What leaves the
// top limb is worth 2^256 each, which is 38 mod p, so it comes back in at the
// bottom. The shift has to be arithmetic, floor division for negative limbs:
// C++17 leaves that to the compiler, and GCC and Clang both do it.
static_assert((int64_t{-1} >> 1) == -1, "carry() needs an arithmetic right shift");

void carry(Fe& a) {
    for (int i = 0; i < 16; i++) {
        const int64_t c = a.v[i] >> 16;
        a.v[i] -= c * 65536;
        if (i < 15) a.v[i + 1] += c;
        else a.v[0] += 38 * c;
    }
}

Fe add(const Fe& a, const Fe& b) {
    Fe r;
    for (int i = 0; i < 16; i++) r.v[i] = a.v[i] + b.v[i];
    return r;
}

Fe sub(const Fe& a, const Fe& b) {
    Fe r;
    for (int i = 0; i < 16; i++) r.v[i] = a.v[i] - b.v[i];
    return r;
}

Fe mul(const Fe& a, const Fe& b) {
    int64_t t[31] = {};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a.v[i] * b.v[j];
    }
    // Limb 16 and up are worth 2^256 and more: 38 times as much, 16 limbs down.
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    Fe r;
    for (int i = 0; i < 16; i++) r.v[i] = t[i];
    carry(r);
    carry(r);
    return r;
}

Fe square(const Fe& a) { return mul(a, a); }

// Takes b where `take` is 1 and keeps a where it is 0, without a branch.
void select(Fe& a, const Fe& b, int64_t take) {
    const int64_t mask = -take;
    for (int i = 0; i < 16; i++) a.v[i] ^= mask & (a.v[i] ^ b.v[i]);
}

// The unique representative in [0, p), as 32 little-endian bytes.
void pack(uint8_t out[32], const Fe& in) {
    Fe a = in;
    carry(a);
    carry(a);
    carry(a);
    // Now below 2^256 with 16-bit limbs, so under 3p: subtracting p twice,
    // each time only when that does not go negative, leaves it under p.
    for (int round = 0; round < 2; round++) {
        Fe m;
        m.v[0] = a.v[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m.v[i] = a.v[i] - 0xffff - ((m.v[i - 1] >> 16) & 1);
            m.v[i - 1] &= 0xffff;
        }
        m.v[15] = a.v[15] - 0x7fff - ((m.v[14] >> 16) & 1);
        const int64_t borrow = (m.v[15] >> 16) & 1;
        m.v[14] &= 0xffff;
        select(a, m, 1 - borrow);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = static_cast<uint8_t>(a.v[i] & 0xff);
        out[2 * i + 1] = static_cast<uint8_t>(a.v[i] >> 8);
    }
}

// The low 255 bits of 32 little-endian bytes; the top bit is left to the caller.
Fe unpack(const uint8_t in[32]) {
    Fe r;
    for (int i = 0; i < 16; i++) r.v[i] = in[2 * i] + (static_cast<int64_t>(in[2 * i + 1]) << 8);
    r.v[15] &= 0x7fff;
    return r;
}

bool equal(const Fe& a, const Fe& b) {
    uint8_t x[32], y[32];
    pack(x, a);
    pack(y, b);
    return std::memcmp(x, y, 32) == 0;
}

int parity(const Fe& a) {
    uint8_t x[32];
    pack(x, a);
    return x[0] & 1;
}

bool is_zero(const Fe& a) {
    uint8_t x[32];
    pack(x, a);
    uint8_t any = 0;
    for (const uint8_t b : x) any |= b;
    return any == 0;
}

// a^(p - 2) = 1/a. p - 2 = 2^255 - 21 has every bit set from 254 down but
// bits 4 and 2.
Fe invert(const Fe& a) {
    Fe c = a;
    for (int bit = 253; bit >= 0; bit--) {
        c = square(c);
        if (bit != 2 && bit != 4) c = mul(c, a);
    }
    return c;
}

// a^((p - 5) / 8), for square roots. (p - 5) / 8 = 2^252 - 3 has every bit set
// from 251 down but bit 1.
Fe pow_p58(const Fe& a) {
    Fe c = a;
    for (int bit = 250; bit >= 0; bit--) {
        c = square(c);
        if (bit != 1) c = mul(c, a);
    }
    return c;
}

// The curve's constants, worked out once rather than written out in limbs:
// d = -121665 / 121666, and the square root of -1, 2^((p - 1) / 4), whose
// exponent 2^253 - 5 has every bit set from 252 down but bit 2.
struct Constants {
    Fe d, d2, sqrt_m1;
    Constants() {
        d = mul(small(-121665), invert(small(121666)));
        d2 = add(d, d);
        const Fe two = small(2);
        sqrt_m1 = two;
        for (int bit = 251; bit >= 0; bit--) {
            sqrt_m1 = square(sqrt_m1);
            if (bit != 2) sqrt_m1 = mul(sqrt_m1, two);
        }
    }
};

const Constants& constants() {
    static const Constants c;
    return c;
}

struct Point {
    Fe x, y, z, t;
};

Point neutral() {
    Point p;
    p.y = small(1);
    p.z = small(1);
    return p;
}

// a + b on -x^2 + y^2 = 1 + d x^2 y^2 (RFC 8032, 5.1.4).
Point add(const Point& p, const Point& q) {
    const Fe a = mul(sub(p.y, p.x), sub(q.y, q.x));
    const Fe b = mul(add(p.y, p.x), add(q.y, q.x));
    const Fe c = mul(mul(p.t, q.t), constants().d2);
    const Fe z = mul(p.z, q.z);
    const Fe d = add(z, z);
    const Fe e = sub(b, a), f = sub(d, c), g = add(d, c), h = add(b, a);
    Point r;
    r.x = mul(e, f);
    r.y = mul(g, h);
    r.t = mul(e, h);
    r.z = mul(f, g);
    return r;
}

void swap_if(Point& p, Point& q, int64_t condition) {
    Fe* a[4] = {&p.x, &p.y, &p.z, &p.t};
    Fe* b[4] = {&q.x, &q.y, &q.z, &q.t};
    for (int i = 0; i < 4; i++) {
        const Fe keep = *a[i];
        select(*a[i], *b[i], condition);
        select(*b[i], keep, condition);
    }
}

// [scalar]q, the scalar as 32 little-endian bytes: every bit costs the same
// doubling and addition, the ladder only swapping which is which.
Point multiply(const Point& point, const uint8_t scalar[32]) {
    Point p = neutral(), q = point;
    for (int bit = 255; bit >= 0; bit--) {
        const int64_t b = (scalar[bit / 8] >> (bit & 7)) & 1;
        swap_if(p, q, b);
        q = add(q, p);
        p = add(p, p);
        swap_if(p, q, b);
    }
    return p;
}

void encode(uint8_t out[32], const Point& p) {
    const Fe zi = invert(p.z);
    const Fe x = mul(p.x, zi), y = mul(p.y, zi);
    pack(out, y);
    out[31] ^= static_cast<uint8_t>(parity(x) << 7);
}

// RFC 8032, 5.1.3, refusing a y of p or more instead of reducing it.
bool decode(Point& out, const uint8_t in[32]) {
    const Fe y = unpack(in);
    uint8_t canonical[32];
    pack(canonical, y);
    uint8_t given[32];
    std::memcpy(given, in, 32);
    given[31] &= 0x7f;
    if (std::memcmp(canonical, given, 32) != 0) return false;

    const Constants& k = constants();
    const Fe one = small(1);
    const Fe y2 = square(y);
    const Fe u = sub(y2, one);
    const Fe v = add(mul(k.d, y2), one);
    const Fe v3 = mul(square(v), v);
    const Fe v7 = mul(square(v3), v);
    Fe x = mul(mul(u, v3), pow_p58(mul(u, v7)));
    const Fe vx2 = mul(v, square(x));
    if (!equal(vx2, u)) {
        if (!equal(vx2, sub(Fe{}, u))) return false;
        x = mul(x, k.sqrt_m1);
    }
    const int sign = in[31] >> 7;
    if (is_zero(x) && sign) return false;
    if (parity(x) != sign) x = sub(Fe{}, x);
    out.x = x;
    out.y = y;
    out.z = one;
    out.t = mul(x, y);
    return true;
}

// The base point: y = 4/5 and the even x.
const Point& base() {
    static const Point b = [] {
        uint8_t bytes[32];
        pack(bytes, mul(small(4), invert(small(5))));
        Point p;
        decode(p, bytes);
        return p;
    }();
    return b;
}

// The group order L = 2^252 + 27742317777372353535851937790883648493, in
// 32-bit limbs, low first.
const uint32_t kOrder[8] = {0x5cf5d3ed, 0x5812631a, 0xa2f79cd6, 0x14def9de, 0, 0, 0, 0x10000000};

// x mod L for x as 64 little-endian bytes, bit by bit from the top: shift one
// bit into the remainder and take L off whenever that does not go below
// zero. Branch-free, so the nonce a signature is built on does not show in
// how long it takes. About 14 000 operations, three times per signature;
// the scalar multiplications cost a hundred times more.
void reduce(uint8_t out[32], const uint8_t in[64]) {
    uint32_t r[9] = {};
    for (int bit = 511; bit >= 0; bit--) {
        uint32_t carry = (in[bit / 8] >> (bit & 7)) & 1u;
        for (int i = 0; i < 9; i++) {
            const uint32_t next = r[i] >> 31;
            r[i] = (r[i] << 1) | carry;
            carry = next;
        }
        uint32_t t[9];
        uint64_t borrow = 0;
        for (int i = 0; i < 9; i++) {
            const uint64_t l = i < 8 ? kOrder[i] : 0;
            const uint64_t d = static_cast<uint64_t>(r[i]) - l - borrow;
            t[i] = static_cast<uint32_t>(d);
            borrow = (d >> 63) & 1;
        }
        // All ones when r was under L and stays, none when t replaces it.
        const uint32_t keep = 0u - static_cast<uint32_t>(borrow);
        for (int i = 0; i < 9; i++) r[i] = (r[i] & keep) | (t[i] & ~keep);
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<uint8_t>(r[i] >> (8 * j));
    }
}

// (k * a + r) mod L, all three as 32 little-endian bytes.
void multiply_add(uint8_t out[32], const uint8_t k[32], const uint8_t a[32], const uint8_t r[32]) {
    uint32_t x[8], y[8], z[8];
    for (int i = 0; i < 8; i++) {
        x[i] = y[i] = z[i] = 0;
        for (int j = 0; j < 4; j++) {
            x[i] |= static_cast<uint32_t>(k[4 * i + j]) << (8 * j);
            y[i] |= static_cast<uint32_t>(a[4 * i + j]) << (8 * j);
            z[i] |= static_cast<uint32_t>(r[4 * i + j]) << (8 * j);
        }
    }
    uint32_t product[16] = {};
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            const uint64_t t = product[i + j] + static_cast<uint64_t>(x[i]) * y[j] + carry;
            product[i + j] = static_cast<uint32_t>(t);
            carry = t >> 32;
        }
        product[i + 8] = static_cast<uint32_t>(carry);
    }
    uint64_t carry = 0;
    for (int i = 0; i < 16; i++) {
        const uint64_t t = product[i] + (i < 8 ? static_cast<uint64_t>(z[i]) : 0) + carry;
        product[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
    uint8_t wide[64];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 4; j++) wide[4 * i + j] = static_cast<uint8_t>(product[i] >> (8 * j));
    }
    reduce(out, wide);
}

bool below_order(const uint8_t s[32]) {
    for (int i = 7; i >= 0; i--) {
        uint32_t limb = 0;
        for (int j = 3; j >= 0; j--) limb = (limb << 8) | s[4 * i + j];
        if (limb < kOrder[i]) return true;
        if (limb > kOrder[i]) return false;
    }
    return false;  // equal to L
}

// k = SHA-512(R || A || message) mod L, which signer and verifier both work out.
void challenge(uint8_t k[32], const uint8_t r[32], const uint8_t public_key[32], const uint8_t* message,
               size_t length) {
    uint8_t wide[64];
    Sha512 sha;
    sha.update(r, 32);
    sha.update(public_key, 32);
    sha.update(message, length);
    sha.finish(wide);
    reduce(k, wide);
}

void secret_scalar(const uint8_t seed[32], uint8_t scalar[32], uint8_t prefix[32]) {
    uint8_t h[64];
    Sha512 sha;
    sha.update(seed, 32);
    sha.finish(h);
    std::memcpy(scalar, h, 32);
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
    std::memcpy(prefix, h + 32, 32);
}

// A public key decoded, and refused when it is of small order (eight times it
// is the neutral point). Such a key takes every message: with the neutral
// point itself as the key, R = [S]B verifies for any S. RFC 8032 lets these
// keys through; nobody who meant to sign anything has one.
bool usable_key(Point& a, const uint8_t public_key[32]) {
    if (!decode(a, public_key)) return false;
    Point eight = a;
    for (int i = 0; i < 3; i++) eight = add(eight, eight);
    return !is_zero(eight.x);
}

}  // namespace

void ed25519_public_key(const uint8_t seed[32], uint8_t public_key[32]) {
    uint8_t scalar[32], prefix[32];
    secret_scalar(seed, scalar, prefix);
    encode(public_key, multiply(base(), scalar));
}

void ed25519_sign(const uint8_t seed[32], const uint8_t* message, size_t length, uint8_t signature[64]) {
    uint8_t scalar[32], prefix[32], public_key[32];
    secret_scalar(seed, scalar, prefix);
    encode(public_key, multiply(base(), scalar));
    uint8_t wide[64], r[32];
    Sha512 sha;
    sha.update(prefix, 32);
    sha.update(message, length);
    sha.finish(wide);
    reduce(r, wide);
    encode(signature, multiply(base(), r));
    uint8_t k[32];
    challenge(k, signature, public_key, message, length);
    multiply_add(signature + 32, k, scalar, r);
}

bool ed25519_key_is_valid(const uint8_t public_key[32]) {
    Point a;
    return usable_key(a, public_key);
}

bool ed25519_verify(const uint8_t public_key[32], const uint8_t* message, size_t length,
                    const uint8_t signature[64]) {
    if (!below_order(signature + 32)) return false;
    Point a;
    if (!usable_key(a, public_key)) return false;
    uint8_t k[32];
    challenge(k, signature, public_key, message, length);
    // [S]B - [k]A has to come out as R, byte for byte: R's own encoding is
    // never reduced, so a non-canonical R cannot match.
    a.x = sub(Fe{}, a.x);
    a.t = sub(Fe{}, a.t);
    uint8_t check[32];
    encode(check, add(multiply(base(), signature + 32), multiply(a, k)));
    return std::memcmp(check, signature, 32) == 0;
}

}  // namespace fernsdr
