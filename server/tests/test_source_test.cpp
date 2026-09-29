#include "../src/source/source_test.h"
#include "test_util.h"
#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE(synthetic_oscillators_match_the_analytic_signals_across_blocks_and_restart) {
    constexpr double rate = 192000;
    constexpr double tau = 6.283185307179586;
    for (const char* kind : {"iq", "real"}) {
        fernsdr::ConfigSection section("band:test");
        section.set("sample_rate", "192000");
        section.set("noise", "0");
        section.set("realtime", "false");
        section.set("signal", kind);
        std::string error;
        auto source = fernsdr::make_test_source(section, error);
        CHECK(source != nullptr);
        if (!source) return;
        CHECK(source->start(error));
        const bool real = source->kind() == fernsdr::SignalKind::Real;
        std::vector<fernsdr::cfloat> iq(137);
        std::vector<float> samples(137);
        double maximum_error = 0;
        fernsdr::cfloat first;
        size_t position = 0;
        for (int block = 0; block < 1000; block++) {
            CHECK(real ? source->read_real(samples.data(), samples.size()) : source->read(iq.data(), iq.size()));
            for (size_t i = 0; i < iq.size(); i++, position++) {
                const double t = position / rate, next = (position + 1) / rate;
                const double offsets[] = {-60000, -24200, -800, 15000, 43600, 71000};
                const double amplitudes[] = {0.30 * (1 + 0.6 * std::cos(tau * 1000 * t)),
                    0.15, 0.20, std::fmod(4 * t, 1.0) < 0.5 ? 0.10 : 0,
                    0.22, 0.06 * (1 + 0.6 * std::cos(tau * 400 * t))};
                fernsdr::cfloat expected{};
                for (size_t s = 0; s < 6; s++) {
                    const double phase = tau * (offsets[s] + (real ? rate / 4 : 0)) * next;
                    expected += fernsdr::cfloat(amplitudes[s] * std::cos(phase), real ? 0 : amplitudes[s] * std::sin(phase));
                }
                const auto actual = real ? fernsdr::cfloat(samples[i], 0) : iq[i];
                if (position == 0) first = actual;
                // Roundoff may place a sample exactly on a CW gate boundary
                // on either side. Compare the waveform away from that edge.
                if (position % 24000 > 1 && position % 24000 < 23999) {
                    maximum_error = std::max(maximum_error, static_cast<double>(std::abs(actual - expected)));
                }
            }
        }
        CHECK(maximum_error < 2e-5);
        CHECK_EQ(source->stats().samples_read, position);
        source->stop();
        CHECK(!source->stats().connected);
        CHECK(source->start(error));
        CHECK_EQ(source->stats().samples_read, 0u);
        CHECK(real ? source->read_real(samples.data(), 1) : source->read(iq.data(), 1));
        CHECK_NEAR(std::abs((real ? fernsdr::cfloat(samples[0], 0) : iq[0]) - first), 0, 1e-6);
    }
}
