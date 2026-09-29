// FFT timing and accuracy for the transforms the receiver runs.
//
//   make fft-bench     FernSDR's own transforms.
//   make fft-compare   The same program beside FFTW 3 in single precision,
//                      planned with FFTW_PATIENT (FERNSDR_FFTW_PLANNER=measure
//                      or exhaustive to change that), its wisdom kept in
//                      build/fftw-wisdom.dat so later runs skip the planning.
//                      FFTW_PREFIX may name a build with the SIMD levels this
//                      machine has. FFTW is linked into this tool only, never
//                      into the server. Both libraries get their arrays from
//                      the same allocator, huge pages included.
//
// Complex calls first copy one of several prepared inputs into the work
// arrays. Repeating an in-place transform on its own output grows the data
// to inf and NaN, and code that checks products for NaN then runs several
// times slower than it does on signals. The copy is timed on its own and
// subtracted. Real transforms read their input without changing it, so they
// rotate through the inputs directly; at the front end's sizes those inputs
// no longer fit in the cache, as each new block would not in the receiver.
//
// Implementations alternate within every round and the median of the rounds
// is reported. Pin it to one core (taskset -c 3) and run nothing alongside.
//
// Errors compare against a double-precision transform of the same input:
// the largest bin error and the RMS error, both relative to the RMS bin.
#include "../src/dsp/fft_split.h"
#include "../src/dsp/mdct.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef FERNSDR_WITH_FFTW
#include <fftw3.h>
#endif

using namespace fernsdr;

namespace {

using Clock = std::chrono::steady_clock;
using cdouble = std::complex<double>;

constexpr int kRounds = 7;
constexpr double kSeconds = 0.03;
constexpr double kPi = 3.14159265358979323846;

// Calls `f(i)` with a rising index until kSeconds pass, reading the clock
// only every `batch` calls so that small transforms are not timed against
// the clock itself.
template <class F>
double ns_per_call(F&& f, size_t n) {
    const size_t batch = std::max<size_t>(1, 16384 / std::max<size_t>(n, 1));
    size_t calls = 0;
    const auto start = Clock::now();
    double elapsed = 0.0;
    do {
        for (size_t j = 0; j < batch; j++) f(calls++);
        elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    } while (elapsed < kSeconds);
    return elapsed / static_cast<double>(calls) * 1e9;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// In-place radix-2 transform in double. Twiddles come from one table of
// directly computed angles, so its own error stays near 1e-15.
void reference_fft(std::vector<cdouble>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<cdouble> table(n / 2);
    for (size_t k = 0; k < n / 2; k++) table[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t step = n / len;
        for (size_t start = 0; start < n; start += len) {
            for (size_t k = 0; k < len / 2; k++) {
                const cdouble u = a[start + k];
                const cdouble v = a[start + k + len / 2] * table[k * step];
                a[start + k] = u + v;
                a[start + k + len / 2] = u - v;
            }
        }
    }
}

struct Error {
    double max = 0.0;
    double rms = 0.0;
};

Error compare(const std::vector<cdouble>& reference, const float* re, const float* im) {
    double level = 0.0, sum = 0.0, worst = 0.0;
    for (size_t k = 0; k < reference.size(); k++) {
        level += std::norm(reference[k]);
        const double d = std::norm(reference[k] - cdouble(re[k], im[k]));
        sum += d;
        worst = std::max(worst, d);
    }
    const double rms_level = std::sqrt(level / static_cast<double>(reference.size()));
    return {std::sqrt(worst) / rms_level, std::sqrt(sum / static_cast<double>(reference.size())) / rms_level};
}

// Allocated as the receiver allocates its block and spectrum arrays.
DspVector<float> noise(size_t n, std::mt19937& rng) {
    std::normal_distribution<float> gauss;
    DspVector<float> v(n);
    for (auto& x : v) x = gauss(rng);
    return v;
}

void print_header() {
    std::printf("%-17s %8s %11s", "transform", "n", "FernSDR ns");
#ifdef FERNSDR_WITH_FFTW
    std::printf(" %12s %12s %9s", "FFTW split", "FFTW inter.", "FFTW/ours");
#endif
    std::printf("  %17s", "error max / rms");
#ifdef FERNSDR_WITH_FFTW
    std::printf("  %17s", "FFTW error");
#endif
    std::printf("\n");
}

struct Row {
    Row(const char* kind, size_t n, Error ours_error) : kind(kind), n(n), ours_error(ours_error) {}
    const char* kind;
    size_t n;
    double ours = 0.0;
    Error ours_error;
    double fftw_split = -1.0;
    double fftw_interleaved = -1.0;
    Error fftw_error;
};

void print_row(const Row& r) {
    std::printf("%-17s %8zu %11.1f", r.kind, r.n, r.ours);
#ifdef FERNSDR_WITH_FFTW
    const double best = r.fftw_interleaved > 0 && (r.fftw_split <= 0 || r.fftw_interleaved < r.fftw_split)
                            ? r.fftw_interleaved : r.fftw_split;
    auto cell = [](double v) {
        if (v > 0) std::printf(" %12.1f", v);
        else std::printf(" %12s", "-");
    };
    cell(r.fftw_split);
    cell(r.fftw_interleaved);
    if (best > 0) std::printf(" %8.2fx", best / r.ours);
    else std::printf(" %9s", "-");
#endif
    std::printf("  %8.1e / %.1e", r.ours_error.max, r.ours_error.rms);
#ifdef FERNSDR_WITH_FFTW
    if (r.fftw_error.rms > 0) std::printf("  %8.1e / %.1e", r.fftw_error.max, r.fftw_error.rms);
#endif
    std::printf("\n");
    std::fflush(stdout);
}

#ifdef FERNSDR_WITH_FFTW
unsigned planner_flags() {
    const char* planner = std::getenv("FERNSDR_FFTW_PLANNER");
    if (planner && std::strcmp(planner, "measure") == 0) return FFTW_MEASURE;
    if (planner && std::strcmp(planner, "exhaustive") == 0) return FFTW_EXHAUSTIVE;
    return FFTW_PATIENT;
}
#endif

constexpr size_t kComplexPool = 8;

// In place on split arrays, as the channelizer and every listener call it.
Row complex_forward(size_t n, std::mt19937& rng) {
    std::vector<DspVector<float>> pool_re, pool_im;
    for (size_t p = 0; p < kComplexPool; p++) pool_re.push_back(noise(n, rng)), pool_im.push_back(noise(n, rng));
    std::vector<cdouble> reference(n);
    for (size_t i = 0; i < n; i++) reference[i] = cdouble(pool_re[0][i], pool_im[0][i]);
    reference_fft(reference);

    FftSplit plan(n);
    DspVector<float> re(n), im(n);
    auto load = [&](size_t i) {
        std::memcpy(re.data(), pool_re[i % kComplexPool].data(), n * sizeof(float));
        std::memcpy(im.data(), pool_im[i % kComplexPool].data(), n * sizeof(float));
    };
    load(0);
    plan.forward(re.data(), im.data());
    Row row("complex forward", n, compare(reference, re.data(), im.data()));

    std::vector<double> copies, ours;
#ifdef FERNSDR_WITH_FFTW
    DspVector<float> fre_storage(n), fim_storage(n), fc_storage(2 * n);
    float* fre = fre_storage.data();
    float* fim = fim_storage.data();
    fftwf_complex* fc = reinterpret_cast<fftwf_complex*>(fc_storage.data());
    fftwf_iodim dim{static_cast<int>(n), 1, 1};
    fftwf_plan split = fftwf_plan_guru_split_dft(1, &dim, 0, nullptr, fre, fim, fre, fim, planner_flags());
    fftwf_plan inter = fftwf_plan_dft_1d(static_cast<int>(n), fc, fc, FFTW_FORWARD, planner_flags());
    std::vector<std::vector<float>> pool_c;
    for (size_t p = 0; p < kComplexPool; p++) {
        std::vector<float> c(2 * n);
        for (size_t i = 0; i < n; i++) c[2 * i] = pool_re[p][i], c[2 * i + 1] = pool_im[p][i];
        pool_c.push_back(std::move(c));
    }
    auto fload = [&](size_t i) {
        std::memcpy(fre, pool_re[i % kComplexPool].data(), n * sizeof(float));
        std::memcpy(fim, pool_im[i % kComplexPool].data(), n * sizeof(float));
    };
    auto cload = [&](size_t i) { std::memcpy(fc, pool_c[i % kComplexPool].data(), 2 * n * sizeof(float)); };
    cload(0);
    fftwf_execute(inter);
    std::vector<float> fr(n), fi(n);
    for (size_t i = 0; i < n; i++) fr[i] = fc[i][0], fi[i] = fc[i][1];
    row.fftw_error = compare(reference, fr.data(), fi.data());
    std::vector<double> fcopies, ccopies, fsplit, finter;
#endif
    for (int r = 0; r < kRounds; r++) {
        copies.push_back(ns_per_call(load, n));
        ours.push_back(ns_per_call([&](size_t i) { load(i); plan.forward(re.data(), im.data()); }, n));
#ifdef FERNSDR_WITH_FFTW
        fcopies.push_back(ns_per_call(fload, n));
        fsplit.push_back(ns_per_call([&](size_t i) { fload(i); fftwf_execute(split); }, n));
        ccopies.push_back(ns_per_call(cload, n));
        finter.push_back(ns_per_call([&](size_t i) { cload(i); fftwf_execute(inter); }, n));
#endif
    }
    row.ours = median(ours) - median(copies);
#ifdef FERNSDR_WITH_FFTW
    row.fftw_split = median(fsplit) - median(fcopies);
    row.fftw_interleaved = median(finter) - median(ccopies);
    fftwf_destroy_plan(split);
    fftwf_destroy_plan(inter);
#endif
    return row;
}

// The listeners' direction. FFTW's inverse costs what its forward does.
Row complex_inverse(size_t n, std::mt19937& rng) {
    std::vector<DspVector<float>> pool_re, pool_im;
    for (size_t p = 0; p < kComplexPool; p++) pool_re.push_back(noise(n, rng)), pool_im.push_back(noise(n, rng));
    // The inverse of x is the conjugate of the forward transform of conj(x).
    std::vector<cdouble> reference(n);
    for (size_t i = 0; i < n; i++) reference[i] = cdouble(pool_re[0][i], -pool_im[0][i]);
    reference_fft(reference);
    for (auto& v : reference) v = std::conj(v);

    FftSplit plan(n);
    DspVector<float> re(n), im(n);
    auto load = [&](size_t i) {
        std::memcpy(re.data(), pool_re[i % kComplexPool].data(), n * sizeof(float));
        std::memcpy(im.data(), pool_im[i % kComplexPool].data(), n * sizeof(float));
    };
    load(0);
    plan.inverse_unscaled(re.data(), im.data());
    Row row("complex inverse", n, compare(reference, re.data(), im.data()));
    std::vector<double> copies, ours;
    for (int r = 0; r < kRounds; r++) {
        copies.push_back(ns_per_call(load, n));
        ours.push_back(ns_per_call([&](size_t i) { load(i); plan.inverse_unscaled(re.data(), im.data()); }, n));
    }
    row.ours = median(ours) - median(copies);
    return row;
}

// Enough distinct inputs that the large sizes read memory the way a new
// block does, bounded to keep the tool's footprint modest.
size_t real_pool(size_t n) { return std::clamp<size_t>((size_t{64} << 20) / (n * sizeof(float)), 2, 16); }

Row real_forward(size_t n, std::mt19937& rng) {
    const size_t pool_size = real_pool(n);
    std::vector<DspVector<float>> pool;
    for (size_t p = 0; p < pool_size; p++) pool.push_back(noise(n, rng));
    std::vector<cdouble> full(n);
    for (size_t i = 0; i < n; i++) full[i] = cdouble(pool[0][i], 0.0);
    reference_fft(full);
    std::vector<cdouble> reference(full.begin(), full.begin() + static_cast<long>(n / 2 + 1));

    RealFft plan(n);
    DspVector<float> out_re(n / 2 + 1), out_im(n / 2 + 1);
    plan.forward(pool[0].data(), out_re.data(), out_im.data());
    Row row("real forward", n, compare(reference, out_re.data(), out_im.data()));
    std::vector<double> ours;
#ifdef FERNSDR_WITH_FFTW
    std::vector<DspVector<float>> fpool_storage(pool.begin(), pool.end());
    std::vector<float*> fpool;
    for (auto& buffer : fpool_storage) fpool.push_back(buffer.data());
    DspVector<float> ore_storage(n / 2 + 1), oim_storage(n / 2 + 1), oc_storage(n + 2), spare_storage(n);
    float* ore = ore_storage.data();
    float* oim = oim_storage.data();
    fftwf_complex* oc = reinterpret_cast<fftwf_complex*>(oc_storage.data());
    fftwf_iodim dim{static_cast<int>(n), 1, 1};
    // Planning may overwrite the arrays it is given, so it gets a spare input.
    float* spare = spare_storage.data();
    fftwf_plan split = fftwf_plan_guru_split_dft_r2c(1, &dim, 0, nullptr, spare, ore, oim, planner_flags());
    fftwf_plan inter = fftwf_plan_dft_r2c_1d(static_cast<int>(n), spare, oc, planner_flags());
    fftwf_execute_dft_r2c(inter, fpool[0], oc);
    std::vector<float> fr(n / 2 + 1), fi(n / 2 + 1);
    for (size_t i = 0; i <= n / 2; i++) fr[i] = oc[i][0], fi[i] = oc[i][1];
    row.fftw_error = compare(reference, fr.data(), fi.data());
    std::vector<double> fsplit, finter;
#endif
    for (int r = 0; r < kRounds; r++) {
        ours.push_back(ns_per_call([&](size_t i) { plan.forward(pool[i % pool_size].data(), out_re.data(), out_im.data()); }, n));
#ifdef FERNSDR_WITH_FFTW
        fsplit.push_back(ns_per_call([&](size_t i) { fftwf_execute_split_dft_r2c(split, fpool[i % pool_size], ore, oim); }, n));
        finter.push_back(ns_per_call([&](size_t i) { fftwf_execute_dft_r2c(inter, fpool[i % pool_size], oc); }, n));
#endif
    }
    row.ours = median(ours);
#ifdef FERNSDR_WITH_FFTW
    row.fftw_split = median(fsplit);
    row.fftw_interleaved = median(finter);
    fftwf_destroy_plan(split);
    fftwf_destroy_plan(inter);
#endif
    return row;
}

// The channelizer's call for a real front end: two half blocks, the sine
// window and the analytic doubling in one pass. FFTW is given the window as
// a pass of its own, with the doubling folded into it.
Row front_end(size_t n, std::mt19937& rng) {
    const size_t half = n / 2;
    const size_t pool_size = real_pool(half) + 1;
    std::vector<DspVector<float>> pool;
    for (size_t p = 0; p < pool_size; p++) pool.push_back(noise(half, rng));
    DspVector<float> window(n);
    for (size_t i = 0; i < n; i++) window[i] = static_cast<float>(std::sin(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));

    std::vector<cdouble> full(n);
    for (size_t i = 0; i < n; i++) full[i] = cdouble((i < half ? pool[0][i] : pool[1][i - half]) * static_cast<double>(window[i]), 0.0);
    reference_fft(full);
    std::vector<cdouble> reference(full.begin(), full.begin() + static_cast<long>(half + 1));
    for (size_t k = 1; k < half; k++) reference[k] *= 2.0;

    RealFft plan(n);
    DspVector<float> out_re(half + 1), out_im(half + 1);
    plan.forward_sine_windowed_halves(pool[0].data(), pool[1].data(), window.data(), out_re.data(), out_im.data(), true);
    Row row("front end", n, compare(reference, out_re.data(), out_im.data()));
    std::vector<double> ours;
#ifdef FERNSDR_WITH_FFTW
    std::vector<DspVector<float>> fpool_storage(pool.begin(), pool.end());
    std::vector<float*> fpool;
    for (auto& buffer : fpool_storage) fpool.push_back(buffer.data());
    DspVector<float> doubled_storage(n), windowed_storage(n), oc_storage(n + 2);
    float* doubled = doubled_storage.data();
    for (size_t i = 0; i < n; i++) doubled[i] = 2.0f * window[i];
    float* windowed = windowed_storage.data();
    fftwf_complex* oc = reinterpret_cast<fftwf_complex*>(oc_storage.data());
    fftwf_plan inter = fftwf_plan_dft_r2c_1d(static_cast<int>(n), windowed, oc, planner_flags());
    auto run = [&](size_t i) {
        const float* older = fpool[i % pool_size];
        const float* newer = fpool[(i + 1) % pool_size];
        for (size_t j = 0; j < half; j++) windowed[j] = older[j] * doubled[j];
        for (size_t j = 0; j < half; j++) windowed[half + j] = newer[j] * doubled[half + j];
        fftwf_execute(inter);
    };
    run(0);
    std::vector<float> fr(half + 1), fi(half + 1);
    for (size_t k = 0; k <= half; k++) {
        const float scale = (k == 0 || k == half) ? 0.5f : 1.0f;
        fr[k] = oc[k][0] * scale, fi[k] = oc[k][1] * scale;
    }
    row.fftw_error = compare(reference, fr.data(), fi.data());
    std::vector<double> finter;
#endif
    for (int r = 0; r < kRounds; r++) {
        ours.push_back(ns_per_call([&](size_t i) {
            plan.forward_sine_windowed_halves(pool[i % pool_size].data(), pool[(i + 1) % pool_size].data(), window.data(),
                                         out_re.data(), out_im.data(), true);
        }, n));
#ifdef FERNSDR_WITH_FFTW
        finter.push_back(ns_per_call(run, n));
#endif
    }
    row.ours = median(ours);
#ifdef FERNSDR_WITH_FFTW
    row.fftw_interleaved = median(finter);
    fftwf_destroy_plan(inter);
#endif
    return row;
}

// An IQ band's channelizer: the previous and the new block as the source
// delivers them, interleaved pairs, windowed and transformed. FFTW gets the
// pass that windows them into its buffer and its faster, interleaved plan.
Row iq_front_end(size_t n, std::mt19937& rng) {
    const size_t half = n / 2;
    const size_t pool_size = real_pool(n) + 1;
    std::vector<DspVector<float>> pool;
    for (size_t p = 0; p < pool_size; p++) pool.push_back(noise(n, rng));  // half pairs each
    DspVector<float> window(n);
    for (size_t i = 0; i < n; i++) window[i] = static_cast<float>(std::sin(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));

    std::vector<cdouble> reference(n);
    for (size_t i = 0; i < n; i++) {
        const float* pair = i < half ? &pool[0][2 * i] : &pool[1][2 * (i - half)];
        reference[i] = cdouble(pair[0], pair[1]) * static_cast<double>(window[i]);
    }
    reference_fft(reference);

    FftSplit plan(n);
    DspVector<float> out_re(n), out_im(n);
    plan.forward_windowed(pool[0].data(), pool[1].data(), window.data(), out_re.data(), out_im.data());
    Row row("iq front end", n, compare(reference, out_re.data(), out_im.data()));
    std::vector<double> ours;
#ifdef FERNSDR_WITH_FFTW
    DspVector<float> buffer_storage(2 * n);
    fftwf_complex* buffer = reinterpret_cast<fftwf_complex*>(buffer_storage.data());
    fftwf_plan inter = fftwf_plan_dft_1d(static_cast<int>(n), buffer, buffer, FFTW_FORWARD, planner_flags());
    auto run = [&](size_t i) {
        const float* older = pool[i % pool_size].data();
        const float* newer = pool[(i + 1) % pool_size].data();
        float* b = buffer_storage.data();
        for (size_t j = 0; j < half; j++) {
            b[2 * j] = older[2 * j] * window[j];
            b[2 * j + 1] = older[2 * j + 1] * window[j];
        }
        for (size_t j = 0; j < half; j++) {
            b[n + 2 * j] = newer[2 * j] * window[half + j];
            b[n + 2 * j + 1] = newer[2 * j + 1] * window[half + j];
        }
        fftwf_execute(inter);
    };
    run(0);
    std::vector<float> fr(n), fi(n);
    for (size_t k = 0; k < n; k++) fr[k] = buffer[k][0], fi[k] = buffer[k][1];
    row.fftw_error = compare(reference, fr.data(), fi.data());
    std::vector<double> finter;
#endif
    for (int r = 0; r < kRounds; r++) {
        ours.push_back(ns_per_call([&](size_t i) {
            plan.forward_windowed(pool[i % pool_size].data(), pool[(i + 1) % pool_size].data(), window.data(),
                                  out_re.data(), out_im.data());
        }, n));
#ifdef FERNSDR_WITH_FFTW
        finter.push_back(ns_per_call(run, n));
#endif
    }
    row.ours = median(ours);
#ifdef FERNSDR_WITH_FFTW
    row.fftw_interleaved = median(finter);
    fftwf_destroy_plan(inter);
#endif
    return row;
}

void mdct(size_t m, std::mt19937& rng) {
    Mdct plan(m);
    std::vector<DspVector<float>> pool;
    for (size_t p = 0; p < kComplexPool; p++) pool.push_back(noise(2 * m, rng));
    std::vector<float> coeffs(m), out(2 * m);
    std::vector<double> forward, inverse;
    for (int r = 0; r < kRounds; r++) {
        forward.push_back(ns_per_call([&](size_t i) { plan.forward(pool[i % kComplexPool].data(), coeffs.data()); }, m));
        inverse.push_back(ns_per_call([&](size_t i) { plan.inverse(pool[i % kComplexPool].data(), out.data()); }, m));
    }
    std::printf("%-17s %8zu %11.1f\n%-17s %8zu %11.1f\n", "mdct forward", m, median(forward), "mdct inverse", m,
                median(inverse));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string what = argc > 1 ? argv[1] : "all";
#ifdef FERNSDR_WITH_FFTW
    const char* wisdom = std::getenv("FERNSDR_FFTW_WISDOM");
    if (!wisdom) wisdom = "fftw-wisdom.dat";
    fftwf_import_wisdom_from_filename(wisdom);
    std::printf("FFTW %s, compiled with: %s\n", fftwf_version, fftwf_cc);
#endif
    std::printf("FernSDR transforms on %s. Median of %d rounds, ns per transform; error relative to the RMS bin.\n\n",
                FftSplit::instruction_set(), kRounds);
    print_header();
    std::mt19937 rng(20260924);
    if (what == "all" || what == "complex") {
        for (size_t n = 16; n <= (size_t{1} << 20); n *= 2) {
            if (n > 32768 && n != 65536 && n != 262144 && n != (size_t{1} << 20)) continue;
            print_row(complex_forward(n, rng));
        }
        for (size_t n = 16; n <= 4096; n *= 2) print_row(complex_inverse(n, rng));
    }
    if (what == "all" || what == "real") {
        for (size_t n : {size_t{1} << 14, size_t{1} << 16, size_t{1} << 18, size_t{1} << 20, size_t{1} << 21})
            print_row(real_forward(n, rng));
        for (size_t n : {size_t{1} << 16, size_t{1} << 18, size_t{1} << 20}) print_row(front_end(n, rng));
    }
    if (what == "all" || what == "iq") {
        for (size_t n : {size_t{1} << 11, size_t{1} << 15, size_t{1} << 16, size_t{1} << 17, size_t{1} << 18,
                         size_t{1} << 19, size_t{1} << 20})
            print_row(iq_front_end(n, rng));
    }
    if (what == "all" || what == "mdct") mdct(128, rng);
#ifdef FERNSDR_WITH_FFTW
    fftwf_export_wisdom_to_filename(wisdom);
#endif
}
