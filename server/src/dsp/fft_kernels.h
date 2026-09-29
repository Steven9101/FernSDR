// Vector kernels for FftSplit. Internal to fft_split.cpp.
//
// Everything here is written once, on GCC vector types of a given number of
// float lanes, and instantiated per instruction set inside entry points that
// carry the matching target attribute. The functions are forced inline so
// that each entry point compiles its own copy for its own registers, and no
// code needing AVX2 is reachable from anywhere else. Intrinsics would tie a
// function to one instruction set, so shuffles use the compiler's generic
// vector shuffle (see shuffle below).
//
// The transform is a four-step one. A length N = N1 * N2 is viewed as N1
// rows of N2 samples. Pass A transforms the columns: lanes consecutive
// columns at once, every lane an independent transform of length N1, so each
// butterfly is plain vector arithmetic with no shuffle at all. It multiplies
// by the twiddle W_N^(column * row) and stores the rows back. Pass B
// transposes tiles of lanes x lanes, transforms the rows the same way, and
// its stores are already the natural output order, lanes bins apart by N1.
// Long transforms first split into sixteen four-step ones (Outer16), and the
// longest stream through radix-4 passes (Streaming). The first pass of each
// reads its input through a small reader (ArrayStreams and its siblings), so
// a band's front end is windowed, split or packed as it is read.
#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace fernsdr {
namespace fft_kernels {

#define FERNSDR_FFT_INLINE __attribute__((always_inline)) inline

// may_alias: the vectors are read from and written to float arrays.
template <size_t Lanes>
struct Lane {
    typedef float V __attribute__((vector_size(Lanes * sizeof(float)), may_alias));
};

// Vectors pass by reference: by value, a vector wider than the baseline's
// changes the calling convention, which GCC warns about even for functions
// that are only ever inlined.
template <class V>
FERNSDR_FFT_INLINE void load(V& v, const float* p) {
    std::memcpy(&v, p, sizeof v);
}

template <class V>
FERNSDR_FFT_INLINE void store(float* p, const V& v) {
    std::memcpy(p, &v, sizeof v);
}

// Lane k of `out` is lane I[k] of a followed by b. GCC before 12 has only
// __builtin_shuffle, which takes the lanes as an integer vector.
template <int... I, class V>
FERNSDR_FFT_INLINE void shuffle(V& out, const V& a, const V& b) {
#if defined(__clang__)
    out = __builtin_shufflevector(a, b, I...);
#else
    typedef int Order __attribute__((vector_size(sizeof(V))));
    out = __builtin_shuffle(a, b, Order{I...});
#endif
}

// Four-point transform in place, natural order.
template <class V>
FERNSDR_FFT_INLINE void fft4(V& r0, V& i0, V& r1, V& i1, V& r2, V& i2, V& r3, V& i3) {
    const V s0r = r0 + r2, s0i = i0 + i2, d0r = r0 - r2, d0i = i0 - i2;
    const V s1r = r1 + r3, s1i = i1 + i3, d1r = r1 - r3, d1i = i1 - i3;
    r0 = s0r + s1r;
    i0 = s0i + s1i;
    r2 = s0r - s1r;
    i2 = s0i - s1i;
    // Bin 1 is d0 - i*d1 and bin 3 is d0 + i*d1.
    r1 = d0r + d1i;
    i1 = d0i - d1r;
    r3 = d0r - d1i;
    i3 = d0i + d1r;
}

constexpr float kHalfSqrt2 = 0.707106781186547524f;
constexpr float kCos1_16 = 0.923879532511286756f;  // cos(pi/8)
constexpr float kSin1_16 = 0.382683432365089772f;  // sin(pi/8)

// Eight points from two four-point halves.
template <class V>
FERNSDR_FFT_INLINE void fft8(V* r, V* i) {
    V er0 = r[0], ei0 = i[0], er1 = r[2], ei1 = i[2], er2 = r[4], ei2 = i[4], er3 = r[6], ei3 = i[6];
    V or0 = r[1], oi0 = i[1], or1 = r[3], oi1 = i[3], or2 = r[5], oi2 = i[5], or3 = r[7], oi3 = i[7];
    fft4(er0, ei0, er1, ei1, er2, ei2, er3, ei3);
    fft4(or0, oi0, or1, oi1, or2, oi2, or3, oi3);
    const V t1r = (or1 + oi1) * kHalfSqrt2, t1i = (oi1 - or1) * kHalfSqrt2;
    const V t2r = oi2, t2i = -or2;
    const V t3r = (oi3 - or3) * kHalfSqrt2, t3i = -(or3 + oi3) * kHalfSqrt2;
    r[0] = er0 + or0; i[0] = ei0 + oi0; r[4] = er0 - or0; i[4] = ei0 - oi0;
    r[1] = er1 + t1r; i[1] = ei1 + t1i; r[5] = er1 - t1r; i[5] = ei1 - t1i;
    r[2] = er2 + t2r; i[2] = ei2 + t2i; r[6] = er2 - t2r; i[6] = ei2 - t2i;
    r[3] = er3 + t3r; i[3] = ei3 + t3i; r[7] = er3 - t3r; i[7] = ei3 - t3i;
}

// Sixteen points from two eight-point halves.
template <class V>
FERNSDR_FFT_INLINE void fft16(V* r, V* i) {
    V er[8], ei[8], orr[8], oi[8];
    for (int k = 0; k < 8; k++) {
        er[k] = r[2 * k];
        ei[k] = i[2 * k];
        orr[k] = r[2 * k + 1];
        oi[k] = i[2 * k + 1];
    }
    fft8(er, ei);
    fft8(orr, oi);
    // W_16^k = cos(pi k/8) - i sin(pi k/8) on the odd half. Bins 0 and 4 are
    // 1 and -i, written out: without -ffast-math a product with 0 or 1 stays.
    const float wr[8] = {1.0f, kCos1_16, kHalfSqrt2, kSin1_16, 0.0f, -kSin1_16, -kHalfSqrt2, -kCos1_16};
    const float wi[8] = {0.0f, -kSin1_16, -kHalfSqrt2, -kCos1_16, -1.0f, -kCos1_16, -kHalfSqrt2, -kSin1_16};
    for (int k = 0; k < 8; k++) {
        V tr, ti;
        if (k == 0) {
            tr = orr[0];
            ti = oi[0];
        } else if (k == 4) {
            tr = oi[4];
            ti = -orr[4];
        } else {
            tr = orr[k] * wr[k] - oi[k] * wi[k];
            ti = orr[k] * wi[k] + oi[k] * wr[k];
        }
        r[k] = er[k] + tr;
        i[k] = ei[k] + ti;
        r[k + 8] = er[k] - tr;
        i[k + 8] = ei[k] - ti;
    }
}

// A vertical transform longer than a codelet runs as radix-8 levels. At a
// level of length len = 8 * b, element b * r + c, r < 8, joins the eight-point
// transform of column c, which is multiplied by W_len^(c * k) and written
// back in place as element b * k + c; each run of b elements is then a
// transform of its own, down to a codelet of at most 16 points. The codelet
// for run s writes bin j to s's base-8 digits reversed, plus j times the
// number of runs.
struct VerticalPlan {
    size_t m = 1;
    size_t levels = 0;             // radix-8 levels before the codelets
    size_t last = 1;               // codelet length, m / 8^levels, at most 16
    const float* twiddle_re = nullptr;  // level L at offset[L]: W_len^(c * k), [c][k], k < 8
    const float* twiddle_im = nullptr;
    const size_t* offset = nullptr;
    const unsigned* reverse = nullptr;  // m / last entries
    // W_256^(c k), [c][k] for c, k < 16: a 256-point transform anywhere in
    // the plan runs as one radix-16 level and sixteen codelets (vertical_to).
    const float* w256_re = nullptr;
    const float* w256_im = nullptr;
};

template <class V>
FERNSDR_FFT_INLINE void codelet(size_t len, V* r, V* i) {
    switch (len) {
    case 2: {
        const V ar = r[0], ai = i[0];
        r[0] = ar + r[1]; i[0] = ai + i[1];
        r[1] = ar - r[1]; i[1] = ai - i[1];
        break;
    }
    case 4: fft4(r[0], i[0], r[1], i[1], r[2], i[2], r[3], i[3]); break;
    case 8: fft8(r, i); break;
    case 16: fft16(r, i); break;
    default: break;
    }
}

// The codelets of one vertical transform, each writing its bins to their
// natural places in `out`.
template <class V, size_t Len>
FERNSDR_FFT_INLINE void finish(const VerticalPlan& p, const V* re, const V* im, V* out_re, V* out_im) {
    const size_t count = p.m / Len;
    for (size_t s = 0; s < count; s++) {
        V r[Len], i[Len];
        for (size_t j = 0; j < Len; j++) {
            r[j] = re[s * Len + j];
            i[j] = im[s * Len + j];
        }
        codelet<V>(Len, r, i);
        const size_t base = p.reverse[s];
        for (size_t j = 0; j < Len; j++) {
            out_re[base + j * count] = r[j];
            out_im[base + j * count] = i[j];
        }
    }
}

// Transform of length p.m on arrays of vectors, every lane independent.
// `re`/`im` are overwritten; the result goes to `out_re`/`out_im`.
template <class V>
FERNSDR_FFT_INLINE void vertical(const VerticalPlan& p, V* re, V* im, V* out_re, V* out_im) {
    size_t len = p.m;
    for (size_t level = 0; level < p.levels; level++) {
        const size_t b = len / 8;
        const float* w_re = p.twiddle_re + p.offset[level];
        const float* w_im = p.twiddle_im + p.offset[level];
        for (size_t base = 0; base < p.m; base += len) {
            V* r = re + base;
            V* i = im + base;
            for (size_t c = 0; c < b; c++) {
                V xr[8], xi[8];
                for (size_t k = 0; k < 8; k++) {
                    xr[k] = r[c + b * k];
                    xi[k] = i[c + b * k];
                }
                fft8(xr, xi);
                r[c] = xr[0];
                i[c] = xi[0];
                for (size_t k = 1; k < 8; k++) {
                    const float wr = w_re[c * 8 + k], wi = w_im[c * 8 + k];
                    r[c + b * k] = xr[k] * wr - xi[k] * wi;
                    i[c + b * k] = xr[k] * wi + xi[k] * wr;
                }
            }
        }
        len = b;
    }
    switch (p.last) {
    case 1: finish<V, 1>(p, re, im, out_re, out_im); break;
    case 2: finish<V, 2>(p, re, im, out_re, out_im); break;
    case 4: finish<V, 4>(p, re, im, out_re, out_im); break;
    case 8: finish<V, 8>(p, re, im, out_re, out_im); break;
    default: finish<V, 16>(p, re, im, out_re, out_im); break;
    }
}

// Transposes rows of Lanes vectors in place: row r, lane c becomes row c,
// lane r.
template <class V>
FERNSDR_FFT_INLINE void transpose4(V* v) {
    V t0, t1, t2, t3;
    shuffle<0, 4, 1, 5>(t0, v[0], v[1]);
    shuffle<2, 6, 3, 7>(t1, v[0], v[1]);
    shuffle<0, 4, 1, 5>(t2, v[2], v[3]);
    shuffle<2, 6, 3, 7>(t3, v[2], v[3]);
    shuffle<0, 1, 4, 5>(v[0], t0, t2);
    shuffle<2, 3, 6, 7>(v[1], t0, t2);
    shuffle<0, 1, 4, 5>(v[2], t1, t3);
    shuffle<2, 3, 6, 7>(v[3], t1, t3);
}

template <class V>
FERNSDR_FFT_INLINE void transpose8(V* v) {
    V t[8], s[8];
    for (int p = 0; p < 4; p++) {
        shuffle<0, 8, 1, 9, 4, 12, 5, 13>(t[2 * p], v[2 * p], v[2 * p + 1]);
        shuffle<2, 10, 3, 11, 6, 14, 7, 15>(t[2 * p + 1], v[2 * p], v[2 * p + 1]);
    }
    for (int h = 0; h < 2; h++) {
        const V* q = t + 4 * h;
        shuffle<0, 1, 8, 9, 4, 5, 12, 13>(s[4 * h + 0], q[0], q[2]);
        shuffle<2, 3, 10, 11, 6, 7, 14, 15>(s[4 * h + 1], q[0], q[2]);
        shuffle<0, 1, 8, 9, 4, 5, 12, 13>(s[4 * h + 2], q[1], q[3]);
        shuffle<2, 3, 10, 11, 6, 7, 14, 15>(s[4 * h + 3], q[1], q[3]);
    }
    for (int c = 0; c < 4; c++) {
        shuffle<0, 1, 2, 3, 8, 9, 10, 11>(v[c], s[c], s[c + 4]);
        shuffle<4, 5, 6, 7, 12, 13, 14, 15>(v[c + 4], s[c], s[c + 4]);
    }
}

template <class V>
FERNSDR_FFT_INLINE void transpose16(V* v) {
    V t[16], s[16];
    for (int p = 0; p < 8; p++) {
        shuffle<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13, 29>(t[2 * p], v[2 * p], v[2 * p + 1]);
        shuffle<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(t[2 * p + 1], v[2 * p], v[2 * p + 1]);
    }
    for (int q = 0; q < 4; q++) {
        const V* u = t + 4 * q;
        shuffle<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(s[4 * q + 0], u[0], u[2]);
        shuffle<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(s[4 * q + 1], u[0], u[2]);
        shuffle<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(s[4 * q + 2], u[1], u[3]);
        shuffle<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(s[4 * q + 3], u[1], u[3]);
    }
    // Each 128-bit quarter of s[4q + c] now holds column c of rows 4q to
    // 4q + 3; gather the quarters.
    for (int c = 0; c < 4; c++) {
        V x0, x1, y0, y1;
        shuffle<0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26, 27>(x0, s[c], s[4 + c]);
        shuffle<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31>(x1, s[c], s[4 + c]);
        shuffle<0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26, 27>(y0, s[8 + c], s[12 + c]);
        shuffle<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31>(y1, s[8 + c], s[12 + c]);
        shuffle<0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23>(v[c], x0, y0);
        shuffle<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31>(v[8 + c], x0, y0);
        shuffle<0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23>(v[4 + c], x1, y1);
        shuffle<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31>(v[12 + c], x1, y1);
    }
}

template <size_t Lanes, class V>
FERNSDR_FFT_INLINE void transpose(V* v) {
    if constexpr (Lanes == 4) transpose4(v);
    else if constexpr (Lanes == 8) transpose8(v);
    else if constexpr (Lanes == 16) transpose16(v);
}

// Where a transform's first pass reads its vectors: element e and the
// Lanes after it, as split real and imaginary parts.
struct ArrayStreams {
    const float* re;
    const float* im;
    template <class V>
    FERNSDR_FFT_INLINE void get(size_t e, V& r, V& i) const {
        load(r, re + e);
        load(i, im + e);
    }
    // Four cache lines of each array from element e (see outer16_pass).
    FERNSDR_FFT_INLINE void prefetch(size_t e) const {
        for (size_t q = 0; q < 64; q += 16) {
            __builtin_prefetch(re + e + q);
            __builtin_prefetch(im + e + q);
        }
    }
};

// Where a transform's last pass puts its vectors: bins e onwards.
struct ArrayOut {
    float* re;
    float* im;
    template <class V>
    FERNSDR_FFT_INLINE void put(size_t e, const V& r, const V& i) const {
        store(re + e, r);
        store(im + e, i);
    }
};

template <size_t Lanes, class V>
FERNSDR_FFT_INLINE void reverse_lanes(V& v) {
    if constexpr (Lanes == 4) shuffle<3, 2, 1, 0>(v, v, v);
    else if constexpr (Lanes == 8) shuffle<7, 6, 5, 4, 3, 2, 1, 0>(v, v, v);
    else shuffle<15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0>(v, v, v);
}

// The even and odd floats of two consecutive vectors.
template <size_t Lanes, class V>
FERNSDR_FFT_INLINE void deinterleave(V& even, V& odd, const V& low, const V& high) {
    if constexpr (Lanes == 4) {
        shuffle<0, 2, 4, 6>(even, low, high);
        shuffle<1, 3, 5, 7>(odd, low, high);
    } else if constexpr (Lanes == 8) {
        shuffle<0, 2, 4, 6, 8, 10, 12, 14>(even, low, high);
        shuffle<1, 3, 5, 7, 9, 11, 13, 15>(odd, low, high);
    } else {
        static_assert(Lanes == 16, "vectors are four, eight or sixteen lanes wide");
        shuffle<0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30>(even, low, high);
        shuffle<1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31>(odd, low, high);
    }
}

// A complex signal as a receiver's front end has it: interleaved (re, im)
// pairs, the older half of the transform's block in one buffer and the newer
// in another, weighted by `window` as they are read. Reading it like this
// replaces the passes that would slide a history, split the pairs and window
// them before the transform.
struct Windowed {
    const float* older;   // elements 0 .. half - 1, as pairs
    const float* newer;   // elements half .. 2 half - 1
    const float* window;  // one weight per element
    size_t half;
};

template <size_t Lanes>
struct WindowedStreams {
    const Windowed& w;
    template <class V>
    FERNSDR_FFT_INLINE void get(size_t e, V& r, V& i) const {
        const float* s = e < w.half ? w.older + 2 * e : w.newer + 2 * (e - w.half);
        V low, high, weight;
        load(low, s);
        load(high, s + Lanes);
        deinterleave<Lanes>(r, i, low, high);
        load(weight, w.window + e);
        r = r * weight;
        i = i * weight;
    }
    FERNSDR_FFT_INLINE void prefetch(size_t e) const {
        const float* s = e < w.half ? w.older + 2 * e : w.newer + 2 * (e - w.half);
        for (size_t q = 0; q < 128; q += 16) __builtin_prefetch(s + q);
        for (size_t q = 0; q < 64; q += 16) __builtin_prefetch(w.window + e + q);
    }
};

// What one plan hands the kernels.
struct Layout {
    size_t n1;             // column transform length (rows)
    size_t n2;             // row transform length (columns)
    const float* pass_re;  // W_N^(column * row), blocks of lanes columns
    const float* pass_im;
    VerticalPlan columns;  // length n1
    VerticalPlan rows;     // length n2
};

// A vertical transform of compile-time length M, written with a compile-time
// stride: the radix-8 levels of `vertical`, but with every loop bound,
// stride and output place a constant, and the codelets writing straight to
// their natural places instead of through the digit-reversal table. The
// level twiddles are the plan's (VerticalPlan, level at offset[level]).
template <class V, size_t M, size_t Stride>
FERNSDR_FFT_INLINE void vertical_to(const VerticalPlan& p, size_t level, V* re, V* im, V* out_re, V* out_im) {
    if constexpr (M == 256) {
        // Radix 16 twice instead of 8, 8 and 4: one pass over the vectors
        // fewer and about half the twiddle products. Element 16 r + c joins
        // the sixteen-point transform of column c; its bin k, times
        // W_256^(c k), becomes element 16 k + c, and run k is then a codelet
        // whose bin j is bin k + 16 j of the whole.
        for (size_t c = 0; c < 16; c++) {
            V xr[16], xi[16];
            for (size_t r = 0; r < 16; r++) {
                xr[r] = re[16 * r + c];
                xi[r] = im[16 * r + c];
            }
            fft16(xr, xi);
            re[c] = xr[0];
            im[c] = xi[0];
            for (size_t k = 1; k < 16; k++) {
                const float wr = p.w256_re[c * 16 + k], wi = p.w256_im[c * 16 + k];
                re[16 * k + c] = xr[k] * wr - xi[k] * wi;
                im[16 * k + c] = xr[k] * wi + xi[k] * wr;
            }
        }
        for (size_t k = 0; k < 16; k++)
            vertical_to<V, 16, Stride * 16>(p, level + 1, re + 16 * k, im + 16 * k, out_re + k * Stride,
                                            out_im + k * Stride);
    } else if constexpr (M <= 16) {
        V r[M], i[M];
        for (size_t j = 0; j < M; j++) {
            r[j] = re[j];
            i[j] = im[j];
        }
        codelet<V>(M, r, i);
        for (size_t j = 0; j < M; j++) {
            out_re[j * Stride] = r[j];
            out_im[j * Stride] = i[j];
        }
    } else {
        constexpr size_t kB = M / 8;
        const float* w_re = p.twiddle_re + p.offset[level];
        const float* w_im = p.twiddle_im + p.offset[level];
        for (size_t c = 0; c < kB; c++) {
            V xr[8], xi[8];
            for (size_t k = 0; k < 8; k++) {
                xr[k] = re[c + kB * k];
                xi[k] = im[c + kB * k];
            }
            fft8(xr, xi);
            re[c] = xr[0];
            im[c] = xi[0];
            for (size_t k = 1; k < 8; k++) {
                const float wr = w_re[c * 8 + k], wi = w_im[c * 8 + k];
                re[c + kB * k] = xr[k] * wr - xi[k] * wi;
                im[c + kB * k] = xr[k] * wi + xi[k] * wr;
            }
        }
        for (size_t k1 = 0; k1 < 8; k1++)
            vertical_to<V, kB, Stride * 8>(p, level + 1, re + k1 * kB, im + k1 * kB, out_re + k1 * Stride,
                                          out_im + k1 * Stride);
    }
}

// The four steps with both lengths fixed at compile time, for the lengths
// every listener and most bands use. Lengths up to 16 are codelets that stay
// in registers, longer ones go through `block` (4 * max(N1, N2) vectors,
// aligned for V); only the rows between the passes and those buffers touch
// memory.
template <size_t Lanes, size_t N1, size_t N2, class In, class Out>
FERNSDR_FFT_INLINE void four_step_fixed(const Layout& p, const In& in, const Out& out, float* z_re, float* z_im,
                                        float* block) {
    static_assert(N1 >= Lanes && N2 >= Lanes, "a pass needs a whole vector of rows or columns");
    using V = typename Lane<Lanes>::V;
    constexpr size_t kMost = N1 > N2 ? N1 : N2;
    V* b_re = reinterpret_cast<V*>(block);
    V* b_im = b_re + kMost;
    V* s_re = b_im + kMost;
    V* s_im = s_re + kMost;

    for (size_t column = 0; column < N2; column += Lanes) {
        const float* w_re = p.pass_re + column * N1;
        const float* w_im = p.pass_im + column * N1;
        if constexpr (N1 <= 16) {
            V r[N1], i[N1];
            for (size_t row = 0; row < N1; row++) in.get(row * N2 + column, r[row], i[row]);
            codelet<V>(N1, r, i);
            store(z_re + column, r[0]);
            store(z_im + column, i[0]);
            for (size_t row = 1; row < N1; row++) {
                V wr, wi;
                load(wr, w_re + row * Lanes);
                load(wi, w_im + row * Lanes);
                store(z_re + row * N2 + column, r[row] * wr - i[row] * wi);
                store(z_im + row * N2 + column, r[row] * wi + i[row] * wr);
            }
        } else {
            for (size_t row = 0; row < N1; row++) in.get(row * N2 + column, b_re[row], b_im[row]);
            vertical_to<V, N1, 1>(p.columns, 0, b_re, b_im, s_re, s_im);
            store(z_re + column, s_re[0]);
            store(z_im + column, s_im[0]);
            for (size_t row = 1; row < N1; row++) {
                V wr, wi;
                load(wr, w_re + row * Lanes);
                load(wi, w_im + row * Lanes);
                store(z_re + row * N2 + column, s_re[row] * wr - s_im[row] * wi);
                store(z_im + row * N2 + column, s_re[row] * wi + s_im[row] * wr);
            }
        }
    }
    for (size_t first = 0; first < N1; first += Lanes) {
        if constexpr (N2 <= 16) {
            V r[N2], i[N2];
            for (size_t tile = 0; tile < N2; tile += Lanes) {
                for (size_t row = 0; row < Lanes; row++) {
                    load(r[tile + row], z_re + (first + row) * N2 + tile);
                    load(i[tile + row], z_im + (first + row) * N2 + tile);
                }
                transpose<Lanes>(r + tile);
                transpose<Lanes>(i + tile);
            }
            codelet<V>(N2, r, i);
            for (size_t k2 = 0; k2 < N2; k2++) out.put(first + N1 * k2, r[k2], i[k2]);
        } else {
            for (size_t tile = 0; tile < N2; tile += Lanes) {
                for (size_t row = 0; row < Lanes; row++) {
                    load(b_re[tile + row], z_re + (first + row) * N2 + tile);
                    load(b_im[tile + row], z_im + (first + row) * N2 + tile);
                }
                transpose<Lanes>(b_re + tile);
                transpose<Lanes>(b_im + tile);
            }
            vertical_to<V, N2, 1>(p.rows, 0, b_re, b_im, s_re, s_im);
            for (size_t k2 = 0; k2 < N2; k2++) out.put(first + N1 * k2, s_re[k2], s_im[k2]);
        }
    }
}

// Picks the fixed form when the plan's lengths have one. `in` gives pass A
// its vectors (see ArrayStreams) and `out` takes pass B's (see ArrayOut); the
// transform in place passes the same arrays to both, which is safe because
// pass A reads everything before pass B writes.
template <size_t Lanes, class In, class Out>
FERNSDR_FFT_INLINE void four_step_any(const Layout& p, const In& in, const Out& out, float* z_re, float* z_im,
                                      float* block) {
    const size_t n1 = p.n1, n2 = p.n2;
    if constexpr (Lanes <= 4) {
        if (n1 == 4 && n2 == 4) return four_step_fixed<Lanes, 4, 4>(p, in, out, z_re, z_im, block);
        if (n1 == 4 && n2 == 8) return four_step_fixed<Lanes, 4, 8>(p, in, out, z_re, z_im, block);
    }
    if constexpr (Lanes <= 8) {
        if (n1 == 8 && n2 == 8) return four_step_fixed<Lanes, 8, 8>(p, in, out, z_re, z_im, block);
        if (n1 == 8 && n2 == 16) return four_step_fixed<Lanes, 8, 16>(p, in, out, z_re, z_im, block);
    }
    if (n1 == 16 && n2 == 16) return four_step_fixed<Lanes, 16, 16>(p, in, out, z_re, z_im, block);
    if (n1 == 16 && n2 == 32) return four_step_fixed<Lanes, 16, 32>(p, in, out, z_re, z_im, block);
    if (n1 == 16 && n2 == 64) return four_step_fixed<Lanes, 16, 64>(p, in, out, z_re, z_im, block);
    if (n1 == 16 && n2 == 128) return four_step_fixed<Lanes, 16, 128>(p, in, out, z_re, z_im, block);
    if (n1 == 16 && n2 == 256) return four_step_fixed<Lanes, 16, 256>(p, in, out, z_re, z_im, block);
    if constexpr (Lanes <= 8) {
        if (n1 == 16 && n2 == 512) return four_step_fixed<Lanes, 16, 512>(p, in, out, z_re, z_im, block);
    } else {
        if (n1 == 64 && n2 == 128) return four_step_fixed<Lanes, 64, 128>(p, in, out, z_re, z_im, block);
    }
    if (n1 == 128 && n2 == 128) return four_step_fixed<Lanes, 128, 128>(p, in, out, z_re, z_im, block);
    if constexpr (Lanes == 16) {
        if (n1 == 128 && n2 == 256) return four_step_fixed<Lanes, 128, 256>(p, in, out, z_re, z_im, block);
        if (n1 == 256 && n2 == 256) return four_step_fixed<Lanes, 256, 256>(p, in, out, z_re, z_im, block);
    }
    // The planner makes four-step plans only for the lengths above, and the
    // tests run every length up to 2^20: a new one missing here stops them.
    std::abort();
}

// The longest transforms, above the long plans' range (Outer16 below),
// stream instead: Stockham autosort passes over the whole
// arrays, radix 4 with the twiddles after each butterfly. Every pass reads and
// writes the arrays in long runs that the hardware prefetches, which beats
// the four steps' strided column reads once the arrays outgrow the caches.
// Stage s holds W_2l^j for its half-length l.
struct Streaming {
    size_t n;
    size_t stages;
    const float* twiddle_re;
    const float* twiddle_im;
    const size_t* offset;
};

// A real signal's samples as the complex input of a real transform:
// element m is (w[2m] s[2m], w[2m+1] s[2m+1]). The first `half` samples are
// in `older`, the rest in `newer`; no window when `window` is null.
struct Packed {
    const float* older;
    const float* newer;
    const float* window;
    size_t half;
};

// Four packed elements from m, a multiple of four.
FERNSDR_FFT_INLINE void load_packed(const Packed& p, size_t m, Lane<4>::V& re, Lane<4>::V& im) {
    using V = Lane<4>::V;
    const size_t sample = 2 * m;
    const float* s = sample < p.half ? p.older + sample : p.newer + (sample - p.half);
    V low, high;
    load(low, s);
    load(high, s + 4);
    shuffle<0, 2, 4, 6>(re, low, high);
    shuffle<1, 3, 5, 7>(im, low, high);
    if (p.window) {
        V window_low, window_high, even, odd;
        load(window_low, p.window + sample);
        load(window_high, p.window + sample + 4);
        shuffle<0, 2, 4, 6>(even, window_low, window_high);
        shuffle<1, 3, 5, 7>(odd, window_low, window_high);
        re = re * even;
        im = im * odd;
    }
}

// The first two stages at once, vectorised across j since each butterfly
// there has a run of one; the four results of four j land transposed.
// `input(m, re, im)` loads elements m to m + 3.
template <class Input>
FERNSDR_FFT_INLINE void streaming_first(const Input& input, float* out_re, float* out_im, const float* phase_re,
                                        const float* phase_im, const float* twice_re, const float* twice_im,
                                        size_t quarter) {
    using V = Lane<4>::V;
    for (size_t j = 0; j < quarter; j += 4) {
        V ar, ai, br, bi, cr, ci, dr, di, wr, wi, xr, xi;
        input(j, ar, ai);
        input(j + quarter, br, bi);
        input(j + 2 * quarter, cr, ci);
        input(j + 3 * quarter, dr, di);
        load(wr, phase_re + j); load(wi, phase_im + j);
        load(xr, twice_re + j); load(xi, twice_im + j);
        const V s0r = ar + cr, s0i = ai + ci, s1r = br + dr, s1i = bi + di;
        const V d0r = ar - cr, d0i = ai - ci, d1r = br - dr, d1i = bi - di;
        const V o0r = d0r + d1i, o0i = d0i - d1r, o1r = d0r - d1i, o1i = d0i + d1r;
        const V yr = wr * xr - wi * xi, yi = wr * xi + wi * xr;
        const V er = s0r - s1r, ei = s0i - s1i;
        V out[4] = {s0r + s1r, o0r * wr - o0i * wi, er * xr - ei * xi, o1r * yr - o1i * yi};
        transpose4(out);
        for (size_t k = 0; k < 4; k++) store(out_re + 4 * j + 4 * k, out[k]);
        V outi[4] = {s0i + s1i, o0r * wi + o0i * wr, er * xi + ei * xr, o1r * yi + o1i * yr};
        transpose4(outi);
        for (size_t k = 0; k < 4; k++) store(out_im + 4 * j + 4 * k, outi[k]);
    }
}

template <size_t Lanes>
FERNSDR_FFT_INLINE void streaming_fused(const float* ar, const float* ai, float* o_re, float* o_im, size_t quarter,
                                        size_t run, float wr, float wi, float xr, float xi, float yr, float yi) {
    using V = typename Lane<Lanes>::V;
    for (size_t k = 0; k < run; k += Lanes) {
        V a, b, c, d, e, f, g, h;
        load(a, ar + k); load(b, ai + k);
        load(c, ar + k + quarter * run); load(d, ai + k + quarter * run);
        load(e, ar + k + 2 * quarter * run); load(f, ai + k + 2 * quarter * run);
        load(g, ar + k + 3 * quarter * run); load(h, ai + k + 3 * quarter * run);
        const V sr0 = a + e, si0 = b + f, sr1 = c + g, si1 = d + h;
        const V dr0 = a - e, di0 = b - f, dr1 = c - g, di1 = d - h;
        const V tr0 = dr0 + di1, ti0 = di0 - dr1, tr1 = dr0 - di1, ti1 = di0 + dr1;
        const V er = sr0 - sr1, ei = si0 - si1;
        store(o_re + k, sr0 + sr1);
        store(o_im + k, si0 + si1);
        store(o_re + k + run, tr0 * wr - ti0 * wi);
        store(o_im + k + run, tr0 * wi + ti0 * wr);
        store(o_re + k + 2 * run, er * xr - ei * xi);
        store(o_im + k + 2 * run, er * xi + ei * xr);
        store(o_re + k + 3 * run, tr1 * yr - ti1 * yi);
        store(o_im + k + 3 * run, tr1 * yi + ti1 * yr);
    }
}

// Forward, in place, `scratch_*` of n floats. With vectors, runs of four or
// more use four lanes and runs of eight or more Lanes; Lanes of one is the
// plain arithmetic for targets without them. With `packed`, the first pass
// reads a real signal's samples (see Packed) instead of `re`/`im`, which
// saves the pass that would pack them; `re`/`im` still end up holding the
// transform. Callers pass `packed` only where a first pass below reads it.
template <size_t Lanes>
FERNSDR_FFT_INLINE void streaming(const Streaming& p, float* re, float* im, float* scratch_re, float* scratch_im,
                                  const Packed* packed = nullptr) {
    using V4 = Lane<4>::V;
    const size_t n = p.n;
    float* src_re = re;
    float* src_im = im;
    float* dst_re = scratch_re;
    float* dst_im = scratch_im;
    size_t l = n / 2, run = 1, stage = 0;
    const auto from_arrays = [&](size_t m, V4& r, V4& i) {
        load(r, src_re + m);
        load(i, src_im + m);
    };
    const auto from_packed = [&](size_t m, V4& r, V4& i) { load_packed(*packed, m, r, i); };
    // Only when the pass count then ends back in the caller's arrays;
    // otherwise the copy back costs what the combined pass saves.
    if (Lanes >= 4 && n >= 16 && (p.stages % 4 == 0 || p.stages % 4 == 3)) {
        if (packed)
            streaming_first(from_packed, dst_re, dst_im, p.twiddle_re, p.twiddle_im, p.twiddle_re + p.offset[1],
                            p.twiddle_im + p.offset[1], n / 4);
        else
            streaming_first(from_arrays, dst_re, dst_im, p.twiddle_re, p.twiddle_im, p.twiddle_re + p.offset[1],
                            p.twiddle_im + p.offset[1], n / 4);
        packed = nullptr;
        src_re = dst_re;
        src_im = dst_im;
        dst_re = re;
        dst_im = im;
        run = 4;
        l = n / 8;
        stage = 2;
    }
    while (l >= 1) {
        const float* w_re = p.twiddle_re + p.offset[stage];
        const float* w_im = p.twiddle_im + p.offset[stage];
        const bool fused = run >= 4 && l >= 2;
        if (fused) {
            const size_t quarter = l / 2;
            const float* x_re = p.twiddle_re + p.offset[stage + 1];
            const float* x_im = p.twiddle_im + p.offset[stage + 1];
            for (size_t j = 0; j < quarter; j++) {
                const float wr = w_re[j], wi = w_im[j], xr = x_re[j], xi = x_im[j];
                const float yr = wr * xr - wi * xi, yi = wr * xi + wi * xr;
                if (Lanes >= 8 && run >= 8)
                    streaming_fused<Lanes>(src_re + j * run, src_im + j * run, dst_re + 4 * j * run,
                                           dst_im + 4 * j * run, quarter, run, wr, wi, xr, xi, yr, yi);
                else if (Lanes >= 4)
                    streaming_fused<4>(src_re + j * run, src_im + j * run, dst_re + 4 * j * run,
                                       dst_im + 4 * j * run, quarter, run, wr, wi, xr, xi, yr, yi);
                else
                    streaming_fused<1>(src_re + j * run, src_im + j * run, dst_re + 4 * j * run,
                                       dst_im + 4 * j * run, quarter, run, wr, wi, xr, xi, yr, yi);
            }
        } else if (Lanes >= 4 && run == 1 && l >= 4) {
            // Vectorised across j; each pair of results interleaves.
            using V = Lane<4>::V;
            for (size_t j = 0; j < l; j += 4) {
                V ur, ui, vr, vi, wr, wi;
                if (packed) {
                    from_packed(j, ur, ui);
                    from_packed(j + l, vr, vi);
                } else {
                    from_arrays(j, ur, ui);
                    from_arrays(j + l, vr, vi);
                }
                load(wr, w_re + j); load(wi, w_im + j);
                const V dr = ur - vr, di = ui - vi, sr = ur + vr, si = ui + vi;
                const V er = dr * wr - di * wi, ei = dr * wi + di * wr;
                V low, high;
                shuffle<0, 4, 1, 5>(low, sr, er);
                shuffle<2, 6, 3, 7>(high, sr, er);
                store(dst_re + 2 * j, low);
                store(dst_re + 2 * j + 4, high);
                shuffle<0, 4, 1, 5>(low, si, ei);
                shuffle<2, 6, 3, 7>(high, si, ei);
                store(dst_im + 2 * j, low);
                store(dst_im + 2 * j + 4, high);
            }
            packed = nullptr;
        } else {
            for (size_t j = 0; j < l; j++) {
                const float wr = w_re[j], wi = w_im[j];
                const float* a_re = src_re + j * run;
                const float* a_im = src_im + j * run;
                const float* b_re = src_re + (j + l) * run;
                const float* b_im = src_im + (j + l) * run;
                float* o_re = dst_re + 2 * j * run;
                float* o_im = dst_im + 2 * j * run;
                for (size_t k = 0; k < run; k++) {
                    const float ur = a_re[k], ui = a_im[k], vr = b_re[k], vi = b_im[k];
                    o_re[k] = ur + vr;
                    o_im[k] = ui + vi;
                    const float dr = ur - vr, di = ui - vi;
                    o_re[k + run] = dr * wr - di * wi;
                    o_im[k + run] = dr * wi + di * wr;
                }
            }
        }
        float* t = src_re; src_re = dst_re; dst_re = t;
        t = src_im; src_im = dst_im; dst_im = t;
        stage += fused ? 2 : 1;
        run *= fused ? 4 : 2;
        l /= fused ? 4 : 2;
    }
    if (src_re != re) {
        std::memcpy(re, src_re, n * sizeof(float));
        std::memcpy(im, src_im, n * sizeof(float));
    }
}

// Lanes packed elements from m, a multiple of Lanes; four go through
// load_packed.
template <size_t Lanes>
FERNSDR_FFT_INLINE void load_packed_lanes(const Packed& p, size_t m, typename Lane<Lanes>::V& re,
                                          typename Lane<Lanes>::V& im) {
    if constexpr (Lanes == 4) {
        load_packed(p, m, re, im);
    } else if constexpr (Lanes == 16) {
        using V = typename Lane<16>::V;
        const size_t sample = 2 * m;
        const float* s = sample < p.half ? p.older + sample : p.newer + (sample - p.half);
        V low, high;
        load(low, s);
        load(high, s + 16);
        shuffle<0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30>(re, low, high);
        shuffle<1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31>(im, low, high);
        if (p.window) {
            V window_low, window_high, even, odd;
            load(window_low, p.window + sample);
            load(window_high, p.window + sample + 16);
            shuffle<0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30>(even, window_low, window_high);
            shuffle<1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31>(odd, window_low, window_high);
            re = re * even;
            im = im * odd;
        }
    } else {
        static_assert(Lanes == 8, "packed loads come four, eight or sixteen lanes wide");
        using V = typename Lane<8>::V;
        const size_t sample = 2 * m;
        const float* s = sample < p.half ? p.older + sample : p.newer + (sample - p.half);
        V low, high;
        load(low, s);
        load(high, s + 8);
        shuffle<0, 2, 4, 6, 8, 10, 12, 14>(re, low, high);
        shuffle<1, 3, 5, 7, 9, 11, 13, 15>(im, low, high);
        if (p.window) {
            V window_low, window_high, even, odd;
            load(window_low, p.window + sample);
            load(window_high, p.window + sample + 8);
            shuffle<0, 2, 4, 6, 8, 10, 12, 14>(even, window_low, window_high);
            shuffle<1, 3, 5, 7, 9, 11, 13, 15>(odd, window_low, window_high);
            re = re * even;
            im = im * odd;
        }
    }
}

// Long transforms: one radix-16 pass over the whole array, then sixteen
// transforms of a sixteenth of the length.
//
// Streaming reads and writes both arrays at every pass, ten passes at half a
// million points, and once the arrays outgrow L2 that traffic is what the
// transform costs. This pass does the first four stages in one go and leaves
// sixteen independent blocks, each small enough to finish inside L2.
// Decimation in frequency: element j of block k is
// W_n^(j k) sum_r x[j + m r] W_16^(r k), m = n / 16, and the transform of
// block k holds bin 16 q + k of the whole at its element q.
//
// Blocks sit `stride` floats apart rather than m. m is a large power of two,
// and sixteen streams that far apart map to the same cache sets, which is
// what defeated four steps at these lengths before; the padding makes every
// block start in a different set. The input streams are the caller's and
// cannot be padded, so the pass copies a run of each into a small buffer
// before it combines them, and prefetches the runs it will need next.
struct Outer16 {
    size_t n = 0;
    size_t m = 0;       // n / 16
    size_t stride = 0;  // floats from one block to the next
    // W_n^(j k) for j = g * Lanes + lane, as W_n^(g Lanes k) * W_n^(lane k):
    // `coarse` at [k - 1][g], `fine` at [k - 1][lane]. Exact tables for every
    // j would be almost as large as the data they multiply.
    const float* coarse_re = nullptr;
    const float* coarse_im = nullptr;
    const float* fine_re = nullptr;
    const float* fine_im = nullptr;
};

// Elements of each input stream a chunk copies: four cache lines per array.
constexpr size_t kOuterRun = 64;

template <size_t Lanes>
struct PackedStreams {
    const Packed& packed;
    template <class V>
    FERNSDR_FFT_INLINE void get(size_t e, V& r, V& i) const {
        load_packed_lanes<Lanes>(packed, e, r, i);
    }
    FERNSDR_FFT_INLINE void prefetch(size_t e) const {
        const size_t sample = 2 * e;
        const float* s = sample < packed.half ? packed.older + sample : packed.newer + (sample - packed.half);
        for (size_t q = 0; q < 2 * kOuterRun; q += 16) {
            __builtin_prefetch(s + q);
            if (packed.window) __builtin_prefetch(packed.window + sample + q);
        }
    }
};

// A real signal's packed samples under the sine window a channelizer uses,
// w[s] = sin(pi (s + 1/2) / N), computed instead of read. Sample s of stream
// r, element j is 2 (r m + j) + {0, 1}, and 2 m / N = 1/16, so its weight is
// sin(theta_j + r pi / 16) = sin(theta_j) cos(r pi / 16) + cos(theta_j)
// sin(r pi / 16): four tables of m values replace a window of N, which at a
// million points is 4 MB the pass would otherwise read for every block.
struct SineTables {
    const float* sin_even;  // sin(pi (2 j + 1/2) / N), j < m
    const float* cos_even;
    const float* sin_odd;   // sin(pi (2 j + 3/2) / N)
    const float* cos_odd;
    const float* sin_r;     // sin(r pi / 16), r < 16
    const float* cos_r;
    size_t m_log;           // log2 m
};

template <size_t Lanes>
struct SinePackedStreams {
    const Packed& packed;  // its window is ignored
    const SineTables& t;
    template <class V>
    FERNSDR_FFT_INLINE void get(size_t e, V& r, V& i) const {
        const Packed bare{packed.older, packed.newer, nullptr, packed.half};
        load_packed_lanes<Lanes>(bare, e, r, i);
        const size_t stream = e >> t.m_log, j = e & ((size_t{1} << t.m_log) - 1);
        const float c = t.cos_r[stream], s = t.sin_r[stream];
        V se, ce, so, co;
        load(se, t.sin_even + j);
        load(ce, t.cos_even + j);
        load(so, t.sin_odd + j);
        load(co, t.cos_odd + j);
        r = r * (se * c + ce * s);
        i = i * (so * c + co * s);
    }
    FERNSDR_FFT_INLINE void prefetch(size_t e) const {
        const size_t sample = 2 * e;
        const float* s = sample < packed.half ? packed.older + sample : packed.newer + (sample - packed.half);
        for (size_t q = 0; q < 2 * kOuterRun; q += 16) __builtin_prefetch(s + q);
    }
};

// The radix-16 pass, from `input` into the blocks at y_re/y_im.
template <size_t Lanes, class Input>
FERNSDR_FFT_INLINE void outer16_pass(const Outer16& p, const Input& input, float* y_re, float* y_im) {
    using V = typename Lane<Lanes>::V;
    alignas(64) float stage_re[16][kOuterRun];
    alignas(64) float stage_im[16][kOuterRun];
    const size_t m = p.m, groups = m / Lanes;
    for (size_t j0 = 0; j0 < m; j0 += kOuterRun) {
        const bool ahead = j0 + 2 * kOuterRun < m;
        for (size_t r = 0; r < 16; r++) {
            if (ahead) input.prefetch(r * m + j0 + 2 * kOuterRun);
            for (size_t q = 0; q < kOuterRun; q += Lanes) {
                V a, b;
                input.get(r * m + j0 + q, a, b);
                store(stage_re[r] + q, a);
                store(stage_im[r] + q, b);
            }
        }
        for (size_t q = 0; q < kOuterRun; q += Lanes) {
            V xr[16], xi[16];
            for (size_t r = 0; r < 16; r++) {
                load(xr[r], stage_re[r] + q);
                load(xi[r], stage_im[r] + q);
            }
            fft16(xr, xi);
            const size_t j = j0 + q, g = j / Lanes;
            store(y_re + j, xr[0]);
            store(y_im + j, xi[0]);
            for (size_t k = 1; k < 16; k++) {
                V fr, fi;
                load(fr, p.fine_re + (k - 1) * Lanes);
                load(fi, p.fine_im + (k - 1) * Lanes);
                const float cr = p.coarse_re[(k - 1) * groups + g], ci = p.coarse_im[(k - 1) * groups + g];
                const V wr = fr * cr - fi * ci, wi = fr * ci + fi * cr;
                store(y_re + k * p.stride + j, xr[k] * wr - xi[k] * wi);
                store(y_im + k * p.stride + j, xr[k] * wi + xi[k] * wr);
            }
        }
    }
}

// Block order to natural order: bin 16 q + k is element q of block k.
template <size_t Lanes, class Out>
FERNSDR_FFT_INLINE void unpermute16(const Outer16& p, const float* y_re, const float* y_im, const Out& out) {
    using V = typename Lane<Lanes>::V;
    for (size_t q0 = 0; q0 < p.m; q0 += Lanes) {
        V vr[16], vi[16];
        for (size_t k = 0; k < 16; k++) {
            load(vr[k], y_re + k * p.stride + q0);
            load(vi[k], y_im + k * p.stride + q0);
        }
        // Tile t holds blocks t .. t + Lanes - 1; afterwards its row `lane`
        // holds those blocks' bins for q = q0 + lane.
        for (size_t t = 0; t < 16; t += Lanes) {
            transpose<Lanes>(vr + t);
            transpose<Lanes>(vi + t);
        }
        for (size_t lane = 0; lane < Lanes; lane++) {
            for (size_t t = 0; t < 16; t += Lanes) out.put(16 * (q0 + lane) + t, vr[t + lane], vi[t + lane]);
        }
    }
}

// W_(2h)^k for k = 16 q + b, h the complex length, as W^(16 q) * W^b.
struct UntangleTwiddles {
    const float* coarse_re;  // [q], q <= m / 2
    const float* coarse_im;
    const float* fine_re;    // [b], b < 16
    const float* fine_im;
};

// A real transform's untangling (RealFft::untangle) reading the packed
// transform in block order and writing its bins in natural order, which
// saves the pass that would first put the blocks in order. Bin k pairs with
// bin h - k: for k = 16 q + b that is element m - q - 1 of block 16 - b, or
// element m - q of block 0 when b = 0, so each chunk of q reads the blocks
// forwards and backwards at once.
template <size_t Lanes>
FERNSDR_FFT_INLINE void untangle16(const Outer16& p, const UntangleTwiddles& w, const float* y_re, const float* y_im,
                                   float scale, float* out_re, float* out_im) {
    using V = typename Lane<Lanes>::V;
    const size_t m = p.m, h = p.n;
    const V s = V{} + scale, negative_s = V{} - scale;
    for (size_t q0 = 0; q0 < m / 2; q0 += Lanes) {
        V fr[16], fi[16], mr[16], mi[16];
        for (size_t b = 0; b < 16; b++) {
            load(fr[b], y_re + b * p.stride + q0);
            load(fi[b], y_im + b * p.stride + q0);
        }
        // Mirror rows: row b, lane l is the partner of bin 16 (q0 + l) + b.
        for (size_t b = 1; b < 16; b++) {
            const size_t block = 16 - b;
            load(mr[b], y_re + block * p.stride + (m - q0 - Lanes));
            load(mi[b], y_im + block * p.stride + (m - q0 - Lanes));
            reverse_lanes<Lanes>(mr[b]);
            reverse_lanes<Lanes>(mi[b]);
        }
        if (q0 == 0) {
            // Bin 0 is its own partner; element m of block 0 does not exist.
            alignas(64) float row_re[Lanes], row_im[Lanes];
            row_re[0] = y_re[0];
            row_im[0] = y_im[0];
            for (size_t l = 1; l < Lanes; l++) {
                row_re[l] = y_re[m - l];
                row_im[l] = y_im[m - l];
            }
            load(mr[0], row_re);
            load(mi[0], row_im);
        } else {
            load(mr[0], y_re + (m - q0 - Lanes + 1));
            load(mi[0], y_im + (m - q0 - Lanes + 1));
            reverse_lanes<Lanes>(mr[0]);
            reverse_lanes<Lanes>(mi[0]);
        }
        for (size_t t = 0; t < 16; t += Lanes) {
            transpose<Lanes>(fr + t);
            transpose<Lanes>(fi + t);
            transpose<Lanes>(mr + t);
            transpose<Lanes>(mi + t);
        }
        for (size_t l = 0; l < Lanes; l++) {
            const size_t q = q0 + l;
            const float cr = w.coarse_re[q], ci = w.coarse_im[q];
            for (size_t t = 0; t < 16; t += Lanes) {
                const V zr = fr[t + l], zi = fi[t + l], pr = mr[t + l], pi = mi[t + l];
                const V even_re = s * (zr + pr), even_im = s * (zi - pi);
                const V odd_re = s * (zi + pi), odd_im = negative_s * (zr - pr);
                V br, bi;
                load(br, w.fine_re + t);
                load(bi, w.fine_im + t);
                const V wr = br * cr - bi * ci, wi = br * ci + bi * cr;
                const V rot_re = odd_re * wr - odd_im * wi, rot_im = odd_re * wi + odd_im * wr;
                store(out_re + 16 * q + t, V(even_re + rot_re));
                store(out_im + 16 * q + t, V(even_im + rot_im));
                // Bins h - 16 q - b for b = t .. t + Lanes - 1, highest first.
                V up_re = even_re - rot_re, up_im = rot_im - even_im;
                reverse_lanes<Lanes>(up_re);
                reverse_lanes<Lanes>(up_im);
                store(out_re + (h - 16 * q - t - Lanes + 1), up_re);
                store(out_im + (h - 16 * q - t - Lanes + 1), up_im);
            }
        }
    }
    // Bin h / 2 is its own partner, element m / 2 of block 0.
    {
        const float zr = y_re[m / 2], zi = y_im[m / 2];
        const float even_re = scale * (zr + zr), odd_re = scale * (zi + zi);
        const float cr = w.coarse_re[m / 2], ci = w.coarse_im[m / 2];
        const float rot_re = odd_re * cr, rot_im = odd_re * ci;
        out_re[h / 2] = even_re + rot_re;
        out_im[h / 2] = rot_im;
    }
    // DC and Nyquist both sit in the packed transform's bin 0.
    out_re[h] = y_re[0] - y_im[0];
    out_im[h] = 0.0f;
    out_re[0] = y_re[0] + y_im[0];
    out_im[0] = 0.0f;
}

// RealFft::untangle on the packed transform in natural order, Lanes bins
// at a time from each end: bin k pairs with bin half - k, so the top run is
// read and written in reverse. Covers bins 1 up to the last whole run below
// half / 2 and returns the first bin it left for the caller's scalar loop.
template <size_t Lanes>
FERNSDR_FFT_INLINE size_t untangle_runs(const float* z_re, const float* z_im, size_t half, const float* w_re,
                                        const float* w_im, float scale, float* out_re, float* out_im) {
    using V = typename Lane<Lanes>::V;
    const V s = V{} + scale, negative_s = V{} - scale;
    size_t k = 1;
    for (; k + Lanes <= half / 2; k += Lanes) {
        const size_t top = half - k - (Lanes - 1);
        V zr, zi, mr, mi, wr, wi;
        load(zr, z_re + k);
        load(zi, z_im + k);
        load(mr, z_re + top);
        load(mi, z_im + top);
        reverse_lanes<Lanes>(mr);
        reverse_lanes<Lanes>(mi);
        const V even_re = s * (zr + mr), even_im = s * (zi - mi);
        const V odd_re = s * (zi + mi), odd_im = negative_s * (zr - mr);
        load(wr, w_re + k);
        load(wi, w_im + k);
        const V rot_re = odd_re * wr - odd_im * wi, rot_im = odd_re * wi + odd_im * wr;
        store(out_re + k, V(even_re + rot_re));
        store(out_im + k, V(even_im + rot_im));
        V up_re = even_re - rot_re, up_im = rot_im - even_im;
        reverse_lanes<Lanes>(up_re);
        reverse_lanes<Lanes>(up_im);
        store(out_re + top, up_re);
        store(out_im + top, up_im);
    }
    return k;
}

// Everything a plan hands the kernels.
struct Plan {
    Layout layout;
    Streaming stream;
    Outer16 outer;
};

// Lanes of one: a plain transform of the whole length, by way of `z_*`.
FERNSDR_FFT_INLINE void single(const Layout& p, float* re, float* im, float* z_re, float* z_im) {
    using V = Lane<1>::V;
    vertical<V>(p.columns, reinterpret_cast<V*>(re), reinterpret_cast<V*>(im), reinterpret_cast<V*>(z_re),
                reinterpret_cast<V*>(z_im));
    std::memcpy(re, z_re, p.n1 * sizeof(float));
    std::memcpy(im, z_im, p.n1 * sizeof(float));
}

}  // namespace fft_kernels
}  // namespace fernsdr
