#include "fft_split.h"

#include "fft_kernels.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#if defined(__SSE2__) && !defined(FERNSDR_SCALAR)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && !defined(FERNSDR_SCALAR)
#include <arm_neon.h>
#endif

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;

#if defined(__SSE2__) && !defined(FERNSDR_SCALAR)
using Float4 = __m128;
inline Float4 mul4(Float4 a, Float4 b) { return _mm_mul_ps(a, b); }
inline void store4(float* p, Float4 a) { _mm_storeu_ps(p, a); }
// The even and odd floats of the eight from `p`.
inline void deinterleave4(const float* p, Float4& even, Float4& odd) {
    const Float4 low = _mm_loadu_ps(p), high = _mm_loadu_ps(p + 4);
    even = _mm_shuffle_ps(low, high, _MM_SHUFFLE(2, 0, 2, 0));
    odd = _mm_shuffle_ps(low, high, _MM_SHUFFLE(3, 1, 3, 1));
}
#elif defined(__ARM_NEON) && !defined(FERNSDR_SCALAR)
using Float4 = float32x4_t;
inline Float4 mul4(Float4 a, Float4 b) { return vmulq_f32(a, b); }
inline void store4(float* p, Float4 a) { vst1q_f32(p, a); }
inline void deinterleave4(const float* p, Float4& even, Float4& odd) {
    const float32x4x2_t both = vld2q_f32(p);
    even = both.val[0];
    odd = both.val[1];
}
#endif

enum class Isa { Scalar, Sse2, Neon, Avx2, Avx512 };

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__) && defined(__SSE2__) && \
    !defined(FERNSDR_SCALAR) && !defined(FERNSDR_NO_AVX2)
#define FERNSDR_FFT_AVX2 1
#if !defined(FERNSDR_NO_AVX512)
#define FERNSDR_FFT_AVX512 1
#endif
#endif
#if (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
#define FERNSDR_FFT_FOUR_LANES 1
#endif

Isa detect_isa() {
    Isa best = Isa::Scalar;
#if defined(FERNSDR_FFT_FOUR_LANES)
#if defined(__ARM_NEON)
    best = Isa::Neon;
#else
    best = Isa::Sse2;
#endif
#endif
    bool avx512 = false;
#if defined(FERNSDR_FFT_AVX2)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) best = Isa::Avx2;
#endif
#if defined(FERNSDR_FFT_AVX512)
    // Some Intel server CPUs lower the clock of every core that runs AVX-512
    // arithmetic, which a thousand listener threads would feel everywhere;
    // AMD's Zen 4 and 5 keep their clock. So AVX-512 is the default on AMD
    // and on Intel only when asked for.
    avx512 = best == Isa::Avx2 && __builtin_cpu_supports("avx512f");
    if (avx512 && __builtin_cpu_is("amd")) best = Isa::Avx512;
#endif
    // Lowering is always safe; raising only as far as the CPU goes.
    if (const char* forced = std::getenv("FERNSDR_FFT_ISA")) {
        if (std::strcmp(forced, "scalar") == 0) best = Isa::Scalar;
        else if (std::strcmp(forced, "sse2") == 0 && (best == Isa::Avx2 || best == Isa::Avx512)) best = Isa::Sse2;
        else if (std::strcmp(forced, "avx2") == 0 && best == Isa::Avx512) best = Isa::Avx2;
        else if (std::strcmp(forced, "avx512") == 0 && avx512) best = Isa::Avx512;
    }
    return best;
}

Isa isa() {
    static const Isa chosen = detect_isa();
    return chosen;
}

size_t widest_lanes() {
    switch (isa()) {
    case Isa::Avx512: return 16;
    case Isa::Avx2: return 8;
    case Isa::Sse2:
    case Isa::Neon: return 4;
    case Isa::Scalar: break;
    }
    return 1;
}

using Run = void (*)(const fft_kernels::Plan&, float* re, float* im, float* z_re, float* z_im, float* block);
using RunPacked = void (*)(const fft_kernels::Plan&, const fft_kernels::Packed&, float* re, float* im, float* z_re,
                           float* z_im, float* block);
using RunWindowed = void (*)(const fft_kernels::Plan&, const fft_kernels::Windowed&, float* re, float* im,
                             float* z_re, float* z_im, float* block);
using RunUntangleRuns = size_t (*)(const float* z_re, const float* z_im, size_t half, const float* w_re,
                                   const float* w_im, float scale, float* out_re, float* out_im);

// A long plan's kernels (Outer16 in fft_kernels.h) for one lane count: the
// radix-16 pass from each kind of input, and the two ways back to natural
// order.
struct OuterKernels {
    void (*arrays)(const fft_kernels::Outer16&, const float* re, const float* im, float* y_re, float* y_im);
    void (*packed)(const fft_kernels::Outer16&, const fft_kernels::Packed&, float* y_re, float* y_im);
    void (*windowed)(const fft_kernels::Outer16&, const fft_kernels::Windowed&, float* y_re, float* y_im);
    void (*sine)(const fft_kernels::Outer16&, const fft_kernels::Packed&, const fft_kernels::SineTables&,
                 float* y_re, float* y_im);
    void (*unpermute)(const fft_kernels::Outer16&, const float* y_re, const float* y_im, float* re, float* im);
    void (*untangle)(const fft_kernels::Outer16&, const fft_kernels::UntangleTwiddles&, const float* y_re,
                     const float* y_im, float scale, float* out_re, float* out_im);
};

// The long-transform kernels for one lane count, instantiated inside entry
// points that carry that count's target attributes.
#define FERNSDR_OUTER16_ENTRIES(SUFFIX, LANES, ATTRIBUTES)                                                       \
    ATTRIBUTES void run_outer_##SUFFIX(const fft_kernels::Outer16& p, const float* re, const float* im,          \
                                       float* y_re, float* y_im) {                                              \
        fft_kernels::outer16_pass<LANES>(p, fft_kernels::ArrayStreams{re, im}, y_re, y_im);                     \
    }                                                                                                           \
    ATTRIBUTES void run_outer_packed_##SUFFIX(const fft_kernels::Outer16& p, const fft_kernels::Packed& packed,   \
                                              float* y_re, float* y_im) {                                       \
        fft_kernels::outer16_pass<LANES>(p, fft_kernels::PackedStreams<LANES>{packed}, y_re, y_im);             \
    }                                                                                                           \
    ATTRIBUTES void run_outer_windowed_##SUFFIX(const fft_kernels::Outer16& p, const fft_kernels::Windowed& w,   \
                                                float* y_re, float* y_im) {                                     \
        fft_kernels::outer16_pass<LANES>(p, fft_kernels::WindowedStreams<LANES>{w}, y_re, y_im);                \
    }                                                                                                           \
    ATTRIBUTES void run_outer_sine_##SUFFIX(const fft_kernels::Outer16& p, const fft_kernels::Packed& packed,     \
                                            const fft_kernels::SineTables& t, float* y_re, float* y_im) {       \
        fft_kernels::outer16_pass<LANES>(p, fft_kernels::SinePackedStreams<LANES>{packed, t}, y_re, y_im);      \
    }                                                                                                           \
    ATTRIBUTES void run_unpermute_##SUFFIX(const fft_kernels::Outer16& p, const float* y_re, const float* y_im,   \
                                           float* re, float* im) {                                              \
        fft_kernels::unpermute16<LANES>(p, y_re, y_im, fft_kernels::ArrayOut{re, im});                          \
    }                                                                                                           \
    ATTRIBUTES void run_untangle_##SUFFIX(const fft_kernels::Outer16& p, const fft_kernels::UntangleTwiddles& w, \
                                          const float* y_re, const float* y_im, float scale, float* out_re,     \
                                          float* out_im) {                                                      \
        fft_kernels::untangle16<LANES>(p, w, y_re, y_im, scale, out_re, out_im);                                \
    }                                                                                                           \
    ATTRIBUTES size_t run_untangle_runs_##SUFFIX(const float* z_re, const float* z_im, size_t half,             \
                                                 const float* w_re, const float* w_im, float scale,             \
                                                 float* out_re, float* out_im) {                                \
        return fft_kernels::untangle_runs<LANES>(z_re, z_im, half, w_re, w_im, scale, out_re, out_im);          \
    }                                                                                                           \
    const OuterKernels outer_##SUFFIX = {run_outer_##SUFFIX,         run_outer_packed_##SUFFIX,                  \
                                         run_outer_windowed_##SUFFIX, run_outer_sine_##SUFFIX,                   \
                                         run_unpermute_##SUFFIX,      run_untangle_##SUFFIX};

void run_single(const fft_kernels::Plan& p, float* re, float* im, float* z_re, float* z_im, float*) {
    fft_kernels::single(p.layout, re, im, z_re, z_im);
}

void run_streaming_single(const fft_kernels::Plan& p, float* re, float* im, float* z_re, float* z_im, float*) {
    fft_kernels::streaming<1>(p.stream, re, im, z_re, z_im);
}

// The four steps reading an array, a real signal's packed samples, or a
// windowed complex front end, for one lane count.
#define FERNSDR_FOUR_STEP_ENTRIES(SUFFIX, LANES, ATTRIBUTES)                                                     \
    ATTRIBUTES void run_##SUFFIX(const fft_kernels::Plan& p, float* re, float* im, float* z_re, float* z_im,      \
                                 float* block) {                                                                \
        fft_kernels::four_step_any<LANES>(p.layout, fft_kernels::ArrayStreams{re, im},                          \
                                          fft_kernels::ArrayOut{re, im}, z_re, z_im, block);                    \
    }                                                                                                           \
    ATTRIBUTES void run_packed_##SUFFIX(const fft_kernels::Plan& p, const fft_kernels::Packed& packed, float* re,  \
                                        float* im, float* z_re, float* z_im, float* block) {                    \
        fft_kernels::four_step_any<LANES>(p.layout, fft_kernels::PackedStreams<LANES>{packed},                  \
                                          fft_kernels::ArrayOut{re, im}, z_re, z_im, block);                    \
    }                                                                                                           \
    ATTRIBUTES void run_windowed_##SUFFIX(const fft_kernels::Plan& p, const fft_kernels::Windowed& w, float* re,  \
                                          float* im, float* z_re, float* z_im, float* block) {                  \
        fft_kernels::four_step_any<LANES>(p.layout, fft_kernels::WindowedStreams<LANES>{w},                     \
                                          fft_kernels::ArrayOut{re, im}, z_re, z_im, block);                    \
    }

#if defined(FERNSDR_FFT_FOUR_LANES)
FERNSDR_FOUR_STEP_ENTRIES(four, 4, __attribute__((noinline)))

void run_streaming_four(const fft_kernels::Plan& p, float* re, float* im, float* z_re, float* z_im, float*) {
    fft_kernels::streaming<4>(p.stream, re, im, z_re, z_im);
}

void run_streaming_four_packed(const fft_kernels::Plan& p, const fft_kernels::Packed& packed, float* re, float* im,
                               float* z_re, float* z_im, float*) {
    fft_kernels::streaming<4>(p.stream, re, im, z_re, z_im, &packed);
}

FERNSDR_OUTER16_ENTRIES(four, 4, __attribute__((noinline)))
#endif

#if defined(FERNSDR_FFT_AVX2)
// Selected only after CPU feature detection; nothing else in the binary
// needs AVX2 or FMA. Four-lane transforms stay on the SSE2 entry: compiled
// for AVX2 they measured slower.
FERNSDR_FOUR_STEP_ENTRIES(avx2, 8, __attribute__((target("avx2,fma"), noinline)))

__attribute__((target("avx2,fma"), noinline))
void run_streaming_avx2(const fft_kernels::Plan& p, float* re, float* im, float* z_re, float* z_im, float*) {
    fft_kernels::streaming<8>(p.stream, re, im, z_re, z_im);
}

__attribute__((target("avx2,fma"), noinline))
void run_streaming_avx2_packed(const fft_kernels::Plan& p, const fft_kernels::Packed& packed, float* re, float* im,
                               float* z_re, float* z_im, float*) {
    fft_kernels::streaming<8>(p.stream, re, im, z_re, z_im, &packed);
}

FERNSDR_OUTER16_ENTRIES(avx2, 8, __attribute__((target("avx2,fma"), noinline)))
#endif

#if defined(FERNSDR_FFT_AVX512)
// Four steps sixteen lanes wide from 256 points, and the long plans from
// 65,536; beyond those the streaming kernels stay eight lanes wide.
FERNSDR_FOUR_STEP_ENTRIES(avx512, 16, __attribute__((target("avx512f,avx2,fma"), noinline)))

FERNSDR_OUTER16_ENTRIES(avx512, 16, __attribute__((target("avx512f,avx2,fma"), noinline)))
#endif

// Outside the long plans' range (outer_plan below), from these lengths on
// the arrays stream through passes instead of being read in columns,
// measured on the four-step's strided reads. Without vectors the columns'
// transposes buy nothing and streaming wins earlier.
constexpr size_t kStreamingFrom = 32768;
// Sixteen lanes wide the four steps beat streaming up to 65,536 points
// (measured: 29 against 54 us at 32,768, 86 against 124 at 65,536), which
// matters where FERNSDR_FFT_OUTER leaves 65,536 to them.
constexpr size_t kStreamingFromWide = 131072;
constexpr size_t kStreamingFromSingle = 8192;

// The lengths that run as one radix-16 pass and sixteen transforms a
// sixteenth as long (Outer16 in fft_kernels.h), by lane count, measured on
// Zen 5 against the plans they replace. Above them the sixteenth-length
// transforms no longer fit L2 beside their blocks and the extra pass that
// puts the bins back in order costs more than the streaming passes saved.
// FERNSDR_FFT_OUTER=from,until overrides the range, for measuring.
bool outer_plan(size_t lanes, size_t n) {
    static const std::pair<size_t, size_t> forced = [] {
        const char* e = std::getenv("FERNSDR_FFT_OUTER");
        if (!e) return std::pair<size_t, size_t>{0, 0};
        char* rest = nullptr;
        const size_t from = std::strtoull(e, &rest, 10);
        const size_t until = rest && *rest == ',' ? std::strtoull(rest + 1, nullptr, 10) : ~size_t{0};
        return std::pair<size_t, size_t>{from, until};
    }();
    if (lanes < 4) return false;
    // The pass copies 64 elements of each block at a time (kOuterRun), so a
    // block must hold at least that many.
    if (forced.first) return n >= std::max<size_t>(forced.first, 16 * fft_kernels::kOuterRun) && n <= forced.second;
    switch (lanes) {
    case 16: return n >= 65536 && n <= 524288;
    case 8: return n >= 32768 && n <= 262144;
    default: return n >= 32768 && n <= 131072;
    }
}

// Vector runs of the natural-order untangling, the widest the CPU runs.
RunUntangleRuns untangle_runs_for_isa() {
    switch (widest_lanes()) {
#if defined(FERNSDR_FFT_AVX512)
    case 16: return run_untangle_runs_avx512;
#endif
#if defined(FERNSDR_FFT_AVX2)
    case 8: return run_untangle_runs_avx2;
#endif
#if defined(FERNSDR_FFT_FOUR_LANES)
    case 4: return run_untangle_runs_four;
#endif
    default: return nullptr;
    }
}

// Floats between the starts of two blocks beyond their length: seventeen
// cache lines, so that sixteen blocks start in sixteen different sets of
// both L1 and L2 however long they are.
constexpr size_t kOuterPad = 272;

// W_2l^j for j < l, for every stage of a streaming transform of length n.
void streaming_tables(size_t n, std::vector<float>& re, std::vector<float>& im, std::vector<size_t>& offset) {
    for (size_t l = n / 2; l >= 1; l >>= 1) {
        offset.push_back(re.size());
        for (size_t j = 0; j < l; j++) {
            const double angle = -kPi * static_cast<double>(j) / static_cast<double>(l);
            re.push_back(static_cast<float>(std::cos(angle)));
            im.push_back(static_cast<float>(std::sin(angle)));
        }
    }
}

// The twiddles and output order of a vertical transform of length m (see
// VerticalPlan in fft_kernels.h). The plan's pointers are set once the
// vectors stop growing.
struct VerticalTables {
    std::vector<float> re, im;
    std::vector<size_t> offset;
    std::vector<unsigned> reverse;
    std::vector<float> w256_re, w256_im;
};

fft_kernels::VerticalPlan vertical_plan(size_t m, VerticalTables& t) {
    fft_kernels::VerticalPlan p;
    p.m = m;
    size_t len = m;
    while (len > 16) {
        const size_t b = len / 8;
        t.offset.push_back(t.re.size());
        for (size_t c = 0; c < b; c++) {
            for (size_t k = 0; k < 8; k++) {
                const double angle = -2.0 * kPi * static_cast<double>(c * k) / static_cast<double>(len);
                t.re.push_back(static_cast<float>(std::cos(angle)));
                t.im.push_back(static_cast<float>(std::sin(angle)));
            }
        }
        len = b;
        p.levels++;
    }
    p.last = len;
    if (m >= 256) {
        for (size_t c = 0; c < 16; c++) {
            for (size_t k = 0; k < 16; k++) {
                const double angle = -2.0 * kPi * static_cast<double>(c * k) / 256.0;
                t.w256_re.push_back(static_cast<float>(std::cos(angle)));
                t.w256_im.push_back(static_cast<float>(std::sin(angle)));
            }
        }
    }
    const size_t count = m / len;
    t.reverse.resize(count);
    for (size_t run = 0; run < count; run++) {
        size_t digits = run, reversed = 0;
        for (size_t level = 0; level < p.levels; level++) {
            reversed = reversed * 8 + digits % 8;
            digits /= 8;
        }
        t.reverse[run] = static_cast<unsigned>(reversed);
    }
    return p;
}

void point(fft_kernels::VerticalPlan& p, const VerticalTables& t) {
    p.twiddle_re = t.re.data();
    p.twiddle_im = t.im.data();
    p.offset = t.offset.data();
    p.reverse = t.reverse.data();
    p.w256_re = t.w256_re.empty() ? nullptr : t.w256_re.data();
    p.w256_im = t.w256_im.empty() ? nullptr : t.w256_im.data();
}
}  // namespace

struct FftSplit::Tables {
    size_t n = 0;
    size_t lanes = 1;
    Run run = nullptr;
    RunPacked run_packed = nullptr;  // plans with vectors, other than long ones
    fft_kernels::Plan plan{};
    std::vector<float> pass_re, pass_im;
    VerticalTables columns, rows;
    std::vector<float> stream_re, stream_im;
    std::vector<size_t> stream_offset;
    RunWindowed run_windowed = nullptr;  // four-step plans with vectors
    const OuterKernels* outer = nullptr;  // long plans
    std::vector<float> outer_coarse_re, outer_coarse_im, outer_fine_re, outer_fine_im;
};

namespace {
std::shared_ptr<const FftSplit::Tables> build_tables(size_t n) {
    auto t = std::make_shared<FftSplit::Tables>();
    t->n = n;
    size_t lanes = widest_lanes();
    // A four-step transform needs at least lanes rows and lanes columns.
    if (lanes == 16 && n < 256) lanes = 8;
    if (lanes == 8 && n < 64) lanes = 4;
    if (lanes == 4 && n < 16) lanes = 1;
    t->lanes = lanes;

    if (outer_plan(lanes, n)) {
        const size_t m = n / 16, groups = m / lanes;
        fft_kernels::Outer16& o = t->plan.outer;
        o.n = n;
        o.m = m;
        o.stride = m + kOuterPad;
        t->outer_coarse_re.resize(15 * groups);
        t->outer_coarse_im.resize(15 * groups);
        t->outer_fine_re.resize(15 * lanes);
        t->outer_fine_im.resize(15 * lanes);
        for (size_t k = 1; k < 16; k++) {
            for (size_t g = 0; g < groups; g++) {
                const double angle = -2.0 * kPi * static_cast<double>((g * lanes * k) % n) / static_cast<double>(n);
                t->outer_coarse_re[(k - 1) * groups + g] = static_cast<float>(std::cos(angle));
                t->outer_coarse_im[(k - 1) * groups + g] = static_cast<float>(std::sin(angle));
            }
            for (size_t lane = 0; lane < lanes; lane++) {
                const double angle = -2.0 * kPi * static_cast<double>(lane * k) / static_cast<double>(n);
                t->outer_fine_re[(k - 1) * lanes + lane] = static_cast<float>(std::cos(angle));
                t->outer_fine_im[(k - 1) * lanes + lane] = static_cast<float>(std::sin(angle));
            }
        }
        o.coarse_re = t->outer_coarse_re.data();
        o.coarse_im = t->outer_coarse_im.data();
        o.fine_re = t->outer_fine_re.data();
        o.fine_im = t->outer_fine_im.data();
#if defined(FERNSDR_FFT_FOUR_LANES)
        if (lanes == 4) t->outer = &outer_four;
#endif
#if defined(FERNSDR_FFT_AVX2)
        if (lanes == 8) t->outer = &outer_avx2;
#endif
#if defined(FERNSDR_FFT_AVX512)
        if (lanes == 16) t->outer = &outer_avx512;
#endif
        return t;
    }

    const size_t streaming_from = lanes == 16 ? kStreamingFromWide : kStreamingFrom;
    if (n >= (lanes > 1 ? streaming_from : kStreamingFromSingle)) {
        streaming_tables(n, t->stream_re, t->stream_im, t->stream_offset);
        size_t stages = 0;
        for (size_t m = n; m > 1; m >>= 1) stages++;
        t->plan.stream = {n, stages, t->stream_re.data(), t->stream_im.data(), t->stream_offset.data()};
        t->run = run_streaming_single;
#if defined(FERNSDR_FFT_FOUR_LANES)
        if (lanes == 4) {
            t->run = run_streaming_four;
            t->run_packed = run_streaming_four_packed;
        }
#endif
#if defined(FERNSDR_FFT_AVX2)
        if (lanes >= 8) {
            t->run = run_streaming_avx2;
            t->run_packed = run_streaming_avx2_packed;
        }
#endif
        return t;
    }

    size_t n1 = n, n2 = 1;
    if (lanes > 1) {
        size_t log = 0;
        while ((size_t{1} << log) < n) log++;
        n1 = size_t{1} << (log / 2);
        // Sixteen rows keep pass A a register codelet; measured faster from
        // 1,024 to 8,192 points, while 16,384 is faster balanced. Sixteen
        // lanes wide, the 8,192-point row pass then outgrows L1, and 64 rows
        // measured fastest.
        if (n >= 1024 && n <= 8192) n1 = 16;
        if (lanes == 16 && n == 8192) n1 = 64;
        n2 = n / n1;
        // Twiddles for pass A, grouped as the pass reads them: for each block
        // of lanes columns, each row, a vector of W_N^(column * row).
        t->pass_re.resize(n);
        t->pass_im.resize(n);
        for (size_t column = 0; column < n2; column += lanes) {
            for (size_t row = 0; row < n1; row++) {
                for (size_t lane = 0; lane < lanes; lane++) {
                    const size_t e = ((column + lane) * row) % n;
                    const double angle = -2.0 * kPi * static_cast<double>(e) / static_cast<double>(n);
                    t->pass_re[column * n1 + row * lanes + lane] = static_cast<float>(std::cos(angle));
                    t->pass_im[column * n1 + row * lanes + lane] = static_cast<float>(std::sin(angle));
                }
            }
        }
    }
    t->plan.layout.n1 = n1;
    t->plan.layout.n2 = n2;
    t->plan.layout.pass_re = t->pass_re.data();
    t->plan.layout.pass_im = t->pass_im.data();
    t->plan.layout.columns = vertical_plan(n1, t->columns);
    t->plan.layout.rows = vertical_plan(n2, t->rows);
    point(t->plan.layout.columns, t->columns);
    point(t->plan.layout.rows, t->rows);

    t->run = run_single;
#if defined(FERNSDR_FFT_FOUR_LANES)
    if (lanes == 4) {
        t->run = run_four;
        t->run_packed = run_packed_four;
        t->run_windowed = run_windowed_four;
    }
#endif
#if defined(FERNSDR_FFT_AVX2)
    if (lanes == 8) {
        t->run = run_avx2;
        t->run_packed = run_packed_avx2;
        t->run_windowed = run_windowed_avx2;
    }
#endif
#if defined(FERNSDR_FFT_AVX512)
    if (lanes == 16) {
        t->run = run_avx512;
        t->run_packed = run_packed_avx512;
        t->run_windowed = run_windowed_avx512;
    }
#endif
    return t;
}

std::shared_ptr<const FftSplit::Tables> shared_tables(size_t n) {
    static std::mutex mutex;
    static std::map<size_t, std::weak_ptr<const FftSplit::Tables>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    std::weak_ptr<const FftSplit::Tables>& slot = cache[n];
    if (std::shared_ptr<const FftSplit::Tables> existing = slot.lock()) return existing;
    std::shared_ptr<const FftSplit::Tables> made = build_tables(n);
    slot = made;
    return made;
}

// Four vectors per element of the longer vertical transform, plus room to
// align them for 32-byte vector loads.
size_t aligned_block_floats(const FftSplit::Tables& t) {
    if (t.lanes == 1 || t.plan.stream.n != 0 || t.plan.outer.n != 0) return 0;
    const size_t most = t.plan.layout.n1 > t.plan.layout.n2 ? t.plan.layout.n1 : t.plan.layout.n2;
    return 4 * most * t.lanes + 16;
}

float* align64(float* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return reinterpret_cast<float*>((address + 63) & ~static_cast<uintptr_t>(63));
}
}  // namespace

// Up to this length a plan works in the calling thread's scratch. Every
// listener runs transforms of these lengths, one at a time on a worker, and a
// thousand listeners each keeping their own few kilobytes of scratch found
// them cold at every block; the thread's one copy stays in the cache. One
// copy serves every plan on a thread: the only transform that runs inside
// another is a long plan's sixteenth-length one, and the long plan keeps its
// blocks in scratch of its own.
namespace {

constexpr size_t kThreadScratchMax = 16384;

struct ThreadScratch {
    DspVector<float> re;
    DspVector<float> im;
    std::vector<float> block;
};

}  // namespace

FftSplit::FftSplit(size_t n) : n_(n) {
    if (n == 0 || (n & (n - 1)) != 0) throw std::invalid_argument("FftSplit size must be a power of two");
    tables_ = shared_tables(n);
    block_floats_ = aligned_block_floats(*tables_);
    if (tables_->outer) {
        const fft_kernels::Outer16& o = tables_->plan.outer;
        scratch_re_.resize(16 * o.stride);
        scratch_im_.resize(16 * o.stride);
        sub_ = std::make_unique<FftSplit>(o.m);
    } else if (n > kThreadScratchMax) {
        scratch_re_.resize(n);
        scratch_im_.resize(n);
        block_.resize(block_floats_);
    }
}

FftSplit::FftSplit(const FftSplit& other) : FftSplit(other.n_) {}

FftSplit& FftSplit::operator=(const FftSplit& other) {
    if (this != &other) {
        FftSplit fresh(other.n_);
        n_ = fresh.n_;
        tables_ = std::move(fresh.tables_);
        sub_ = std::move(fresh.sub_);
        block_floats_ = fresh.block_floats_;
        scratch_re_ = std::move(fresh.scratch_re_);
        scratch_im_ = std::move(fresh.scratch_im_);
        block_ = std::move(fresh.block_);
    }
    return *this;
}

FftSplit::~FftSplit() = default;

bool FftSplit::outer() const { return tables_->outer != nullptr; }

void FftSplit::transform_blocks() const {
    const fft_kernels::Outer16& o = tables_->plan.outer;
    for (size_t k = 0; k < 16; k++) sub_->forward(scratch_re_.data() + k * o.stride, scratch_im_.data() + k * o.stride);
}

void FftSplit::packed_blocks(const float* older, const float* newer, const float* window) const {
    const fft_kernels::Packed packed{older, newer, window, n_};
    tables_->outer->packed(tables_->plan.outer, packed, scratch_re_.data(), scratch_im_.data());
    transform_blocks();
}

FftSplit::Scratch FftSplit::scratch() const {
    if (n_ > kThreadScratchMax) {
        return {scratch_re_.data(), scratch_im_.data(), block_.empty() ? nullptr : align64(block_.data())};
    }
    thread_local ThreadScratch local;
    if (local.re.size() < n_) {
        local.re.resize(n_);
        local.im.resize(n_);
    }
    if (local.block.size() < block_floats_) local.block.resize(block_floats_);
    return {local.re.data(), local.im.data(), block_floats_ ? align64(local.block.data()) : nullptr};
}

const char* FftSplit::instruction_set() {
    switch (isa()) {
    case Isa::Avx512: return "avx512";
    case Isa::Avx2: return "avx2";
    case Isa::Sse2: return "sse2";
    case Isa::Neon: return "neon";
    case Isa::Scalar: break;
    }
    return "scalar";
}

void FftSplit::forward(float* re, float* im) const {
    if (n_ == 1) return;
    if (outer()) {
        const fft_kernels::Outer16& o = tables_->plan.outer;
        tables_->outer->arrays(o, re, im, scratch_re_.data(), scratch_im_.data());
        transform_blocks();
        tables_->outer->unpermute(o, scratch_re_.data(), scratch_im_.data(), re, im);
        return;
    }
    const Scratch work = scratch();
    tables_->run(tables_->plan, re, im, work.re, work.im, work.block);
}

// Swapping the real and imaginary parts of input and output turns the
// forward transform into the unscaled inverse.
void FftSplit::inverse_unscaled(float* re, float* im) const { forward(im, re); }

void FftSplit::inverse(float* re, float* im) const {
    forward(im, re);
    const float scale = 1.0f / static_cast<float>(n_);
    for (size_t i = 0; i < n_; i++) {
        re[i] *= scale;
        im[i] *= scale;
    }
}

void FftSplit::forward_windowed(const float* older, const float* newer, const float* window, float* re,
                                float* im) const {
    const size_t half = n_ / 2;
    const fft_kernels::Windowed w{older, newer, window, half};
    if (outer()) {
        const fft_kernels::Outer16& o = tables_->plan.outer;
        tables_->outer->windowed(o, w, scratch_re_.data(), scratch_im_.data());
        transform_blocks();
        tables_->outer->unpermute(o, scratch_re_.data(), scratch_im_.data(), re, im);
        return;
    }
    if (tables_->run_windowed) {
        const Scratch work = scratch();
        tables_->run_windowed(tables_->plan, w, re, im, work.re, work.im, work.block);
        return;
    }
    // Plans without a first pass that can read the front end: split and
    // window into the output, and transform it there.
    for (size_t e = 0; e < n_; e++) {
        const float* s = e < half ? older + 2 * e : newer + 2 * (e - half);
        re[e] = s[0] * window[e];
        im[e] = s[1] * window[e];
    }
    forward(re, im);
}

void FftSplit::forward_packed(const float* older, const float* newer, const float* window, float* re,
                              float* im) const {
    if (outer()) {
        packed_blocks(older, newer, window);
        tables_->outer->unpermute(tables_->plan.outer, scratch_re_.data(), scratch_im_.data(), re, im);
        return;
    }
    if (tables_->run_packed) {
        const fft_kernels::Packed packed{older, newer, window, n_};
        const Scratch work = scratch();
        tables_->run_packed(tables_->plan, packed, re, im, work.re, work.im, work.block);
        return;
    }
    // Elements below n/2 come from `older`, the rest from `newer`.
    const size_t quarter = n_ / 2;
    size_t i = 0;
#if (defined(__SSE2__) || defined(__ARM_NEON)) && !defined(FERNSDR_SCALAR)
    // The stride-two reads kept the compiler's version scalar, and at a
    // million points this pass cost as much as the transform it feeds.
    for (; i + 4 <= quarter; i += 4) {
        Float4 old_even, old_odd, new_even, new_odd;
        deinterleave4(older + 2 * i, old_even, old_odd);
        deinterleave4(newer + 2 * i, new_even, new_odd);
        if (window) {
            Float4 early_even, early_odd, late_even, late_odd;
            deinterleave4(window + 2 * i, early_even, early_odd);
            deinterleave4(window + n_ + 2 * i, late_even, late_odd);
            old_even = mul4(old_even, early_even);
            old_odd = mul4(old_odd, early_odd);
            new_even = mul4(new_even, late_even);
            new_odd = mul4(new_odd, late_odd);
        }
        store4(re + i, old_even);
        store4(im + i, old_odd);
        store4(re + quarter + i, new_even);
        store4(im + quarter + i, new_odd);
    }
#endif
    for (; i < quarter; i++) {
        re[i] = older[2 * i];
        im[i] = older[2 * i + 1];
        re[quarter + i] = newer[2 * i];
        im[quarter + i] = newer[2 * i + 1];
        if (window) {
            re[i] *= window[2 * i];
            im[i] *= window[2 * i + 1];
            re[quarter + i] *= window[n_ + 2 * i];
            im[quarter + i] *= window[n_ + 2 * i + 1];
        }
    }
    forward(re, im);
}

// --- RealFft ----------------------------------------------------------------

RealFft::RealFft(size_t n) : n_(n), half_(n / 2) {
    if (n < 4 || (n & (n - 1)) != 0) throw std::invalid_argument("RealFft size must be a power of two >= 4");

    // A long half plan untangles straight from its own blocks.
    if (!half_.outer()) {
        work_re_.resize(n / 2);
        work_im_.resize(n / 2);
    }

    // Untangling twiddles: exp(-2*pi*i*k/n) for k in [0, n/4].
    const size_t quarter = n / 4 + 1;
    untangle_re_.resize(quarter);
    untangle_im_.resize(quarter);
    for (size_t k = 0; k < quarter; k++) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        untangle_re_[k] = static_cast<float>(std::cos(angle));
        untangle_im_[k] = static_cast<float>(std::sin(angle));
    }
    if (half_.outer()) {
        const size_t coarse = n / 64 + 1;
        blocks_coarse_re_.resize(coarse);
        blocks_coarse_im_.resize(coarse);
        for (size_t q = 0; q < coarse; q++) {
            const double angle = -2.0 * kPi * static_cast<double>(16 * q) / static_cast<double>(n);
            blocks_coarse_re_[q] = static_cast<float>(std::cos(angle));
            blocks_coarse_im_[q] = static_cast<float>(std::sin(angle));
        }
        blocks_fine_re_.resize(16);
        blocks_fine_im_.resize(16);
        for (size_t b = 0; b < 16; b++) {
            const double angle = -2.0 * kPi * static_cast<double>(b) / static_cast<double>(n);
            blocks_fine_re_[b] = static_cast<float>(std::cos(angle));
            blocks_fine_im_[b] = static_cast<float>(std::sin(angle));
        }
    }
}

void RealFft::forward(const float* input, float* out_re, float* out_im) const {
    forward_windowed_halves(input, input + n_ / 2, nullptr, out_re, out_im, false);
}

void RealFft::forward_sine_windowed_halves(const float* older, const float* newer, const float* window,
                                           float* out_re, float* out_im, bool analytic) const {
    if (!half_.outer()) {
        forward_windowed_halves(older, newer, window, out_re, out_im, analytic);
        return;
    }
    if (window_sin_even_.empty()) build_sine_tables();
    const fft_kernels::Outer16& o = half_.tables_->plan.outer;
    size_t m_log = 0;
    while ((size_t{1} << m_log) < o.m) m_log++;
    const fft_kernels::SineTables tables{window_sin_even_.data(), window_cos_even_.data(), window_sin_odd_.data(),
                                         window_cos_odd_.data(), window_sin_r_.data(), window_cos_r_.data(), m_log};
    const fft_kernels::Packed packed{older, newer, nullptr, n_ / 2};
    half_.tables_->outer->sine(o, packed, tables, half_.scratch_re_.data(), half_.scratch_im_.data());
    half_.transform_blocks();
    const fft_kernels::UntangleTwiddles twiddles{blocks_coarse_re_.data(), blocks_coarse_im_.data(),
                                                 blocks_fine_re_.data(), blocks_fine_im_.data()};
    half_.tables_->outer->untangle(o, twiddles, half_.scratch_re_.data(), half_.scratch_im_.data(),
                                   analytic ? 1.0f : 0.5f, out_re, out_im);
}

// The sine window's weights for SinePackedStreams: for element j of every
// block, the sine and cosine of the even and odd sample's angle, and the
// sixteen block offsets r pi / 16.
void RealFft::build_sine_tables() const {
    const size_t m = half_.tables_->plan.outer.m;
    window_sin_even_.resize(m);
    window_cos_even_.resize(m);
    window_sin_odd_.resize(m);
    window_cos_odd_.resize(m);
    for (size_t j = 0; j < m; j++) {
        const double even = kPi * (2.0 * static_cast<double>(j) + 0.5) / static_cast<double>(n_);
        const double odd = kPi * (2.0 * static_cast<double>(j) + 1.5) / static_cast<double>(n_);
        window_sin_even_[j] = static_cast<float>(std::sin(even));
        window_cos_even_[j] = static_cast<float>(std::cos(even));
        window_sin_odd_[j] = static_cast<float>(std::sin(odd));
        window_cos_odd_[j] = static_cast<float>(std::cos(odd));
    }
    window_sin_r_.resize(16);
    window_cos_r_.resize(16);
    for (size_t r = 0; r < 16; r++) {
        window_sin_r_[r] = static_cast<float>(std::sin(kPi * static_cast<double>(r) / 16.0));
        window_cos_r_[r] = static_cast<float>(std::cos(kPi * static_cast<double>(r) / 16.0));
    }
}

// Consecutive real pairs as one complex sequence: the n/2-point transform of
// that carries all the information of the n-point real transform, interleaved
// between even and odd symmetry.
void RealFft::forward_windowed_halves(const float* older, const float* newer, const float* window,
                                      float* out_re, float* out_im, bool analytic) const {
    const float interior_scale = analytic ? 2.0f : 1.0f;
    if (half_.outer()) {
        // The half plan reads every sample before the untangling writes a
        // bin, so the output may overwrite the input.
        half_.packed_blocks(older, newer, window);
        const fft_kernels::UntangleTwiddles twiddles{blocks_coarse_re_.data(), blocks_coarse_im_.data(),
                                                     blocks_fine_re_.data(), blocks_fine_im_.data()};
        half_.tables_->outer->untangle(half_.tables_->plan.outer, twiddles, half_.scratch_re_.data(),
                                       half_.scratch_im_.data(), 0.5f * interior_scale, out_re, out_im);
        return;
    }
    half_.forward_packed(older, newer, window, work_re_.data(), work_im_.data());
    untangle(out_re, out_im, interior_scale);
}

void RealFft::untangle(float* out_re, float* out_im, float interior_scale) const {
    const size_t half = n_ / 2;
    const float component_scale = 0.5f * interior_scale;

    // Untangle. Z is the packed transform; the even part carries the real
    // signal's even-symmetric component and the odd part its odd-symmetric
    // one, separated here and recombined with the half-bin twiddle. Runs of
    // the widest vectors the CPU has go from each end first (untangle_runs in
    // fft_kernels.h); bin 0, which the end overwrites anyway, the centre and
    // whatever is left are the loop's.
    static const RunUntangleRuns runs = untangle_runs_for_isa();
    size_t first = 0;
    if (runs) {
        first = runs(work_re_.data(), work_im_.data(), half, untangle_re_.data(), untangle_im_.data(), component_scale,
                     out_re, out_im);
    }
    for (size_t k = first; k <= half / 2; k++) {
        const size_t mirror = (half - k) & (half - 1);

        const float zr = work_re_[k];
        const float zi = work_im_[k];
        const float mr = work_re_[mirror];
        const float mi = work_im_[mirror];

        const float even_re = component_scale * (zr + mr);
        const float even_im = component_scale * (zi - mi);
        const float odd_re = component_scale * (zi + mi);
        const float odd_im = -component_scale * (zr - mr);

        const float wr = untangle_re_[k];
        const float wi = untangle_im_[k];
        const float rot_re = odd_re * wr - odd_im * wi;
        const float rot_im = odd_re * wi + odd_im * wr;

        out_re[k] = even_re + rot_re;
        out_im[k] = even_im + rot_im;

        // The mirrored bin follows from conjugate symmetry of a real signal.
        if (k > 0 && k < half - k) {
            const size_t upper = half - k;
            out_re[upper] = even_re - rot_re;
            out_im[upper] = -(even_im - rot_im);
        }
    }

    // Nyquist: the packed transform's DC bin carries both DC and Nyquist.
    out_re[half] = work_re_[0] - work_im_[0];
    out_im[half] = 0.0f;
    out_re[0] = work_re_[0] + work_im_[0];
    out_im[0] = 0.0f;
}

}  // namespace fernsdr
