// |z| of a complex float, the number std::abs gives, without its library call.
#pragma once
#include <cmath>
#include <complex>

namespace fernsdr {

// std::abs on a std::complex<float> goes through cabsf and hypotf, a call per
// sample in every loop that takes an envelope. For finite values glibc's
// hypotf is the square root of the sum of squares worked in double; the
// product of two floats is exact in double, so the same number comes out of
// this, as tests/test_magnitude.cpp checks. Infinities and NaN keep the
// library's rules, which differ from the formula's.
inline float magnitude_of(std::complex<float> z) {
    const double re = z.real(), im = z.imag();
    const float magnitude = static_cast<float>(std::sqrt(re * re + im * im));
    return std::isfinite(magnitude) ? magnitude : std::abs(z);
}

}  // namespace fernsdr
