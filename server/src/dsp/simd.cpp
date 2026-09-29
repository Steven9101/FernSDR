#include "simd.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "fft_split.h"

namespace fernsdr::simd {

namespace {

// Written once on GCC vector types of `Lanes` floats and forced inline into
// entry points that carry the matching target attribute, as fft_kernels.h
// does: each entry compiles its own copy for its own registers, and nothing
// needing AVX2 is reachable except through the dispatch below. Vectors pass
// by reference, since by value a vector wider than the baseline's changes
// the calling convention.
#define FERNSDR_SIMD_INLINE __attribute__((always_inline)) inline

template <size_t Lanes>
struct Lane {
    typedef float V __attribute__((vector_size(Lanes * sizeof(float)), may_alias));
};

template <class V>
FERNSDR_SIMD_INLINE void load(V& v, const float* p) {
    std::memcpy(&v, p, sizeof v);
}

template <class V>
FERNSDR_SIMD_INLINE void store(float* p, const V& v) {
    std::memcpy(p, &v, sizeof v);
}

// atan on [0, 1] as x times a polynomial in x squared, minimax to 1e-5 rad
// (Rajan, Wang, Inkol and Joyal's odd polynomial, refitted to degree 11).
constexpr float kA1 = 0.99997726f;
constexpr float kA3 = -0.33262347f;
constexpr float kA5 = 0.19354346f;
constexpr float kA7 = -0.11643287f;
constexpr float kA9 = 0.05265332f;
constexpr float kA11 = -0.01172120f;
constexpr float kHalfPi = 1.57079633f;
constexpr float kPi = 3.14159265f;

// atan2 on whole vectors, without branches: the octant is folded in by
// selects. The same arithmetic as fast_atan2 below, lane for lane.
template <class V>
FERNSDR_SIMD_INLINE void atan2_lanes(V& out, const V& y, const V& x) {
    const V zero = V{} + 0.0f;
    const V ax = x < zero ? -x : x;
    const V ay = y < zero ? -y : y;
    const V wide = ax > ay ? ax : ay;
    const V narrow = ax > ay ? ay : ax;
    // Both zero gives a ratio of zero rather than a NaN.
    const V a = narrow / (wide + 1e-30f);
    const V s = a * a;
    V r = ((((kA11 * s + kA9) * s + kA7) * s + kA5) * s + kA3) * s + kA1;
    r *= a;
    r = ay > ax ? kHalfPi - r : r;
    r = x < zero ? kPi - r : r;
    out = y < zero ? -r : r;
}

template <size_t Lanes>
FERNSDR_SIMD_INLINE float dot_lanes(const float* a, const float* b, size_t n) {
    typedef typename Lane<Lanes>::V V;
    // Two sums in flight, so each addition does not wait for the last.
    V s0 = V{} + 0.0f, s1 = V{} + 0.0f;
    size_t i = 0;
    for (; i + 2 * Lanes <= n; i += 2 * Lanes) {
        V x0, y0, x1, y1;
        load(x0, a + i);
        load(y0, b + i);
        load(x1, a + i + Lanes);
        load(y1, b + i + Lanes);
        s0 += x0 * y0;
        s1 += x1 * y1;
    }
    s0 += s1;
    float lanes[Lanes];
    store(lanes, s0);
    float sum = 0.0f;
    for (size_t k = 0; k < Lanes; k++) sum += lanes[k];
    for (; i < n; i++) sum += a[i] * b[i];
    return sum;
}

template <size_t Lanes>
FERNSDR_SIMD_INLINE void dot2_lanes(const float* a, const float* b0, const float* b1, size_t n, float* out0,
                                    float* out1) {
    typedef typename Lane<Lanes>::V V;
    V s0 = V{} + 0.0f, s1 = V{} + 0.0f;
    size_t i = 0;
    for (; i + Lanes <= n; i += Lanes) {
        V x, y0, y1;
        load(x, a + i);
        load(y0, b0 + i);
        load(y1, b1 + i);
        s0 += x * y0;
        s1 += x * y1;
    }
    float lanes0[Lanes], lanes1[Lanes];
    store(lanes0, s0);
    store(lanes1, s1);
    float sum0 = 0.0f, sum1 = 0.0f;
    for (size_t k = 0; k < Lanes; k++) {
        sum0 += lanes0[k];
        sum1 += lanes1[k];
    }
    for (; i < n; i++) {
        sum0 += a[i] * b0[i];
        sum1 += a[i] * b1[i];
    }
    *out0 = sum0;
    *out1 = sum1;
}

// The phase step between consecutive samples, `Lanes` at a time: the
// product with the previous sample's conjugate, then its angle. Samples are
// taken apart into real and imaginary vectors a block at a time on the stack.
template <size_t Lanes>
FERNSDR_SIMD_INLINE void phase_steps_lanes(const cfloat* in, size_t n, cfloat previous, float* out) {
    typedef typename Lane<Lanes>::V V;
    constexpr size_t kBlock = 256;
    float yr[kBlock], xr[kBlock];
    size_t done = 0;
    while (done < n) {
        const size_t count = n - done < kBlock ? n - done : kBlock;
        const cfloat* s = in + done;
        for (size_t i = 0; i < count; i++) {
            const cfloat before = i == 0 ? previous : s[i - 1];
            // s * conj(before)
            xr[i] = s[i].real() * before.real() + s[i].imag() * before.imag();
            yr[i] = s[i].imag() * before.real() - s[i].real() * before.imag();
        }
        previous = s[count - 1];
        size_t i = 0;
        for (; i + Lanes <= count; i += Lanes) {
            V y, x, r;
            load(y, yr + i);
            load(x, xr + i);
            atan2_lanes(r, y, x);
            store(out + done + i, r);
        }
        for (; i < count; i++) out[done + i] = fast_atan2(yr[i], xr[i]);
        done += count;
    }
}

// log10 on vectors: x = 2^e m with m in [sqrt(1/2), sqrt(2)), then
// ln m = 2 atanh(s) with s = (m - 1) / (m + 1), |s| < 0.172, where four terms
// of the series leave an error under 2e-8.
template <size_t Lanes>
FERNSDR_SIMD_INLINE void power_to_db_lanes(const float* power, size_t n, float scale, float offset_db, float* out) {
    typedef typename Lane<Lanes>::V V;
    typedef int32_t I __attribute__((vector_size(Lanes * sizeof(int32_t)), may_alias));
    constexpr float kTenOverLn10 = 4.34294482f;  // 10 / ln 10
    constexpr float kLn2 = 0.693147181f;
    size_t i = 0;
    for (; i + Lanes <= n; i += Lanes) {
        V x;
        load(x, power + i);
        x = x * scale + 1e-30f;
        I bits;
        std::memcpy(&bits, &x, sizeof bits);
        I exponent = ((bits >> 23) & 0xff) - 127;
        I mantissa_bits = (bits & 0x007fffff) | 0x3f800000;
        V m;
        std::memcpy(&m, &mantissa_bits, sizeof m);
        // Into [sqrt(1/2), sqrt(2)): halve the larger mantissas.
        const auto big = m > 1.41421356f;
        m = big ? m * 0.5f : m;
        exponent = big ? exponent + 1 : exponent;
        const V e = __builtin_convertvector(exponent, V);
        const V s = (m - 1.0f) / (m + 1.0f);
        const V s2 = s * s;
        const V ln_m = 2.0f * s * (1.0f + s2 * (1.0f / 3.0f + s2 * (1.0f / 5.0f + s2 * (1.0f / 7.0f))));
        const V db = (e * kLn2 + ln_m) * kTenOverLn10 - offset_db;
        store(out + i, db);
    }
    for (; i < n; i++) out[i] = 10.0f * std::log10(power[i] * scale + 1e-30f) - offset_db;
}

float dot_scalar(const float* a, const float* b, size_t n) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

void dot2_scalar(const float* a, const float* b0, const float* b1, size_t n, float* out0, float* out1) {
    *out0 = dot_scalar(a, b0, n);
    *out1 = dot_scalar(a, b1, n);
}

void power_to_db_scalar(const float* power, size_t n, float scale, float offset_db, float* out) {
    for (size_t i = 0; i < n; i++) out[i] = 10.0f * std::log10(power[i] * scale + 1e-30f) - offset_db;
}

void phase_steps_scalar(const cfloat* in, size_t n, cfloat previous, float* out) {
    for (size_t i = 0; i < n; i++) {
        const cfloat product = in[i] * std::conj(previous);
        previous = in[i];
        out[i] = fast_atan2(product.imag(), product.real());
    }
}

#if (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
#define FERNSDR_SIMD_FOUR 1
__attribute__((noinline)) float dot_four(const float* a, const float* b, size_t n) { return dot_lanes<4>(a, b, n); }
__attribute__((noinline)) void dot2_four(const float* a, const float* b0, const float* b1, size_t n, float* out0,
                                         float* out1) {
    dot2_lanes<4>(a, b0, b1, n, out0, out1);
}
__attribute__((noinline)) void phase_steps_four(const cfloat* in, size_t n, cfloat previous, float* out) {
    phase_steps_lanes<4>(in, n, previous, out);
}
__attribute__((noinline)) void power_to_db_four(const float* power, size_t n, float scale, float offset_db, float* out) {
    power_to_db_lanes<4>(power, n, scale, offset_db, out);
}
#endif

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__) && defined(__SSE2__) && \
    !defined(FERNSDR_SCALAR) && !defined(FERNSDR_NO_AVX2)
#define FERNSDR_SIMD_AVX2 1
__attribute__((target("avx2,fma"), noinline)) float dot_avx2(const float* a, const float* b, size_t n) {
    return dot_lanes<8>(a, b, n);
}
__attribute__((target("avx2,fma"), noinline)) void dot2_avx2(const float* a, const float* b0, const float* b1,
                                                              size_t n, float* out0, float* out1) {
    dot2_lanes<8>(a, b0, b1, n, out0, out1);
}
__attribute__((target("avx2,fma"), noinline)) void phase_steps_avx2(const cfloat* in, size_t n, cfloat previous,
                                                                     float* out) {
    phase_steps_lanes<8>(in, n, previous, out);
}
__attribute__((target("avx2,fma"), noinline)) void power_to_db_avx2(const float* power, size_t n, float scale,
                                                                    float offset_db, float* out) {
    power_to_db_lanes<8>(power, n, scale, offset_db, out);
}
#endif

struct Kernels {
    float (*dot)(const float*, const float*, size_t) = dot_scalar;
    void (*dot2)(const float*, const float*, const float*, size_t, float*, float*) = dot2_scalar;
    void (*phase_steps)(const cfloat*, size_t, cfloat, float*) = phase_steps_scalar;
    void (*power_to_db)(const float*, size_t, float, float, float*) = power_to_db_scalar;
    const char* name = "scalar";
};

// Follows the transforms' choice, so FERNSDR_FFT_ISA lowers both together.
// AVX-512 uses the eight-lane kernels: these loops are short, and sixteen
// lanes measured no faster on them.
Kernels choose() {
    Kernels k;
    const char* isa = FftSplit::instruction_set();
#if defined(FERNSDR_SIMD_AVX2)
    if (std::strcmp(isa, "avx2") == 0 || std::strcmp(isa, "avx512") == 0) {
        k.dot = dot_avx2;
        k.dot2 = dot2_avx2;
        k.phase_steps = phase_steps_avx2;
        k.power_to_db = power_to_db_avx2;
        k.name = "avx2";
        return k;
    }
#endif
#if defined(FERNSDR_SIMD_FOUR)
    if (std::strcmp(isa, "scalar") != 0) {
        k.dot = dot_four;
        k.dot2 = dot2_four;
        k.phase_steps = phase_steps_four;
        k.power_to_db = power_to_db_four;
        k.name = isa;
    }
#endif
    (void)isa;
    return k;
}

const Kernels& kernels() {
    static const Kernels chosen = choose();
    return chosen;
}

}  // namespace

float fast_atan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float wide = ax > ay ? ax : ay;
    const float narrow = ax > ay ? ay : ax;
    const float a = narrow / (wide + 1e-30f);
    const float s = a * a;
    float r = ((((kA11 * s + kA9) * s + kA7) * s + kA5) * s + kA3) * s + kA1;
    r *= a;
    if (ay > ax) r = kHalfPi - r;
    if (x < 0.0f) r = kPi - r;
    return y < 0.0f ? -r : r;
}

float dot(const float* a, const float* b, size_t n) { return kernels().dot(a, b, n); }

void dot2(const float* a, const float* b0, const float* b1, size_t n, float* out0, float* out1) {
    kernels().dot2(a, b0, b1, n, out0, out1);
}

void phase_steps(const cfloat* in, size_t n, cfloat previous, float* out) {
    if (n == 0) return;
    kernels().phase_steps(in, n, previous, out);
}

void power_to_db(const float* power, size_t n, float scale, float offset_db, float* out) {
    kernels().power_to_db(power, n, scale, offset_db, out);
}

const char* instruction_set() { return kernels().name; }

}  // namespace fernsdr::simd
