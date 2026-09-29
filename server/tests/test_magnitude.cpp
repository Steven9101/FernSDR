#include "../src/dsp/magnitude.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace {

// Units in the last place between two floats of the same sign.
long ulps_apart(float a, float b) {
    int32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return std::labs(static_cast<long>(x) - static_cast<long>(y));
}

}  // namespace

TEST_CASE(magnitude_is_what_complex_abs_gives) {
    std::mt19937 rng(314);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    long checked = 0, differing = 0, worst = 0;
    const auto check = [&](float re, float im) {
        const float library = std::abs(std::complex<float>(re, im));
        const float mine = fernsdr::magnitude_of(std::complex<float>(re, im));
        checked++;
        if (std::memcmp(&library, &mine, sizeof library) != 0) {
            differing++;
            worst = std::max(worst, ulps_apart(library, mine));
        }
    };
    // One draw at a time, in the order GCC on x86-64 evaluated these as
    // arguments, last first; C++ leaves that order open.
    for (int i = 0; i < 200000; i++) {
        const float second = unit(rng);
        const float first = unit(rng);
        check(first, second);
    }
    // Across the exponent range, subnormals and values whose squares
    // overflow a float included.
    for (int e = -149; e <= 127; e += 2) {
        for (int i = 0; i < 400; i++) {
            const int spread = std::clamp(e + static_cast<int>(rng() % 9) - 4, -149, 127);
            const float second = std::ldexp(unit(rng), spread);
            const float first = std::ldexp(unit(rng), e);
            check(first, second);
        }
    }
    CHECK(checked > 200000);
#if defined(__GLIBC__)
    // Where the claim in magnitude.h was measured: the same bits.
    CHECK_EQ(differing, 0);
#else
    CHECK(worst <= 1);
#endif

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(fernsdr::magnitude_of({inf, nan}) == inf);
    CHECK(fernsdr::magnitude_of({nan, -inf}) == inf);
    CHECK(std::isnan(fernsdr::magnitude_of({nan, 1.0f})));
    CHECK(fernsdr::magnitude_of({-3.0f, 4.0f}) == 5.0f);
    CHECK(fernsdr::magnitude_of({0.0f, -0.0f}) == 0.0f);
}
