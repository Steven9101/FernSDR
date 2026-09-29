#pragma once
#include <array>
#include <complex>
#include "fft.h"

namespace fernsdr {

// The shared transform has roughly 50 Hz bins. A short FFT mask alone cannot
// select a 50 or 100 Hz CW channel accurately. This selector runs after
// decimation, while the signal is still complex, before AGC can amplify a
// leaked neighbour. Narrower filters necessarily take longer to settle.
class CwFilter {
public:
    void configure(double rate, double low, double high, bool enabled);
    void process(cfloat* samples, size_t count);
    void reset();

private:
    struct Target {
        double rate = 12000, low = 0, high = 0;
        bool enabled = false;
        bool operator==(const Target& other) const {
            return rate == other.rate && low == other.low && high == other.high && enabled == other.enabled;
        }
    };
    struct Section {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::complex<double> z1{}, z2{};
    };
    struct Bank {
        Target target;
        std::array<Section, 5> sections;
        std::complex<double> phase{1, 0}, step{1, 0};
        bool enabled = false;
        void configure(const Target& target);
        cfloat process(cfloat input);
        // process() over a block, in place, for when no fade is running.
        void run(cfloat* samples, size_t count);
        void tidy();
    };
    void transition();
    std::array<Bank, 2> banks_;
    Target requested_;
    size_t active_ = 0, fade_ = 0, fade_length_ = 1;
    bool initialized_ = false, pending_ = false;
};
}
