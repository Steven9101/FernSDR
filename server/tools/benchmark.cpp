// Capacity benchmark.
//
// Produces the numbers quoted in docs/PERFORMANCE.md. Everything here is
// measured on the machine it runs on; nothing is extrapolated.
//
//   make -C server bench
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/codec/nac.h"
#include "../src/dsp/agc.h"
#include "../src/dsp/channelizer.h"
#include "../src/dsp/demod.h"
#include "../src/dsp/fft_split.h"
#include "../src/dsp/spectrum.h"

using namespace fernsdr;
using Clock = std::chrono::steady_clock;

namespace {

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void rule(const char* title) {
    printf("\n%s\n", title);
    for (size_t i = 0; i < std::strlen(title); i++) putchar('-');
    putchar('\n');
}

// How much front-end bandwidth one core can transform, which is the ceiling on
// what a single band can cover at all.
void bench_front_end() {
    rule("Front-end transform throughput (one core)");
    printf("%-12s %14s %14s\n", "FFT size", "Msps/core", "block latency");
    for (size_t n : {16384u, 65536u, 262144u, 1048576u}) {
        RealFft fft(n);
        std::vector<float> signal(n, 0.05f);
        std::vector<float> re(n / 2 + 1), im(n / 2 + 1);

        for (int i = 0; i < 3; i++) fft.forward(signal.data(), re.data(), im.data());
        auto start = Clock::now();
        int iterations = 0;
        while (seconds_since(start) < 0.4) {
            fft.forward(signal.data(), re.data(), im.data());
            iterations++;
        }
        const double elapsed = seconds_since(start);
        const double msps = iterations * (n / 2.0) / elapsed / 1e6;
        printf("%-12zu %14.1f %11.2f ms\n", n, msps, (n / 2.0) / (msps * 1e6) * 1000.0);
    }
    printf("\nA 64 Msps real front end (RX-888 mk2, 0-32 MHz) wants ~50 Hz bins,\n"
           "which is a 2^20 transform: divide 64 by the figure above for cores.\n");
}

void bench_listeners(double sample_rate, size_t fft_size, SignalKind kind, int users,
                     const char* label) {
    rule(label);

    Channelizer channelizer(sample_rate, fft_size, kind);
    size_t decimation = 1;
    while (decimation * 2 <= static_cast<size_t>(sample_rate / 12000.0 * 1.4142)) decimation *= 2;
    const size_t ifft_size = std::max<size_t>(16, fft_size / decimation);

    std::vector<Channel> channels;
    std::vector<Demodulator> demodulators(users);
    std::vector<Agc> agcs(users);
    std::vector<nac::Encoder> encoders;
    channels.reserve(users);
    encoders.reserve(users);

    std::mt19937 rng(1);
    const double audio_rate = sample_rate * ifft_size / fft_size;
    for (int u = 0; u < users; u++) {
        channels.emplace_back(channelizer, ifft_size);
        const double span = sample_rate * 0.3;
        const double origin = kind == SignalKind::Real ? sample_rate * 0.25 : 0.0;
        channels.back().set_passband(origin + (static_cast<double>(rng() % 1000) / 1000.0 - 0.5) * span,
                                     300.0, 2700.0);
        demodulators[u].configure(Mode::Usb, audio_rate);
        agcs[u].configure(audio_rate);
        encoders.emplace_back(static_cast<int>(std::lround(audio_rate)));
        encoders.back().set_bitrate(48000);
        // What a listener asks of NAC3 in USB (Listener::audio_target): the
        // passband is where the channel noise is measured. Without it the
        // noise was measured over the whole channel, the ceiling bound on
        // every frame, and the encoder looked half again as costly as in a
        // receiver.
        nac::Nac3Target target;
        target.max_snr_db = 48.0f;
        target.min_snr_db = 12.0f;
        target.passband_low_hz = 300.0f;
        target.passband_high_hz = 2700.0f;
        encoders.back().set_target(target);
    }
    // NAC3, two frames to a packet: what the receiver page's Balanced audio
    // asks for, its default.
    const auto encode = [&](int u, const float* samples) {
        // As Listener::process_block does: the estimate is kept before the AGC.
        encoders[u].set_signal_gain(std::pow(10.0f, agcs[u].gain_db() / 10.0f));
        encoders[u].add_frame(samples);
        if (encoders[u].packet_frames() == 2) encoders[u].finish_packet();
    };

    const size_t block = channelizer.block_size();
    std::vector<cfloat> iq(block);
    std::vector<float> real_input(block);
    std::normal_distribution<float> noise(0.0f, 0.05f);
    for (size_t i = 0; i < block; i++) {
        // Imaginary part first, as GCC on x86-64 drew it when this was an
        // argument list, whose order C++ leaves open.
        const float im = noise(rng);
        const float re = noise(rng);
        iq[i] = cfloat(re, im);
        real_input[i] = noise(rng);
    }

    std::vector<cfloat> baseband(ifft_size / 2);
    std::vector<float> audio(ifft_size / 2);

    // The shared transform and the per-user chain are timed separately,
    // because the whole architecture rests on the first not growing with the
    // second.
    const int blocks = std::max(4, static_cast<int>(2.0 / channelizer.block_seconds()));

    auto start = Clock::now();
    for (int b = 0; b < blocks; b++) {
        if (kind == SignalKind::Real) channelizer.process_real(real_input.data());
        else channelizer.process(iq.data());
    }
    const double shared_seconds = seconds_since(start);

    start = Clock::now();
    for (int b = 0; b < blocks; b++) {
        if (kind == SignalKind::Real) channelizer.process_real(real_input.data());
        else channelizer.process(iq.data());
        for (int u = 0; u < users; u++) {
            // Same order as Listener: AGC works on the complex baseband,
            // ahead of the demodulator, so it can see the envelope of a signal
            // before detection rather than after.
            channels[u].pull(channelizer, baseband.data());
            agcs[u].process(baseband.data(), baseband.size());
            demodulators[u].process(baseband.data(), baseband.size(), audio.data());
            for (size_t i = 0; i + nac::kFrameHop <= audio.size(); i += nac::kFrameHop) {
                encode(u, audio.data() + i);
            }
        }
    }
    const double total_seconds = seconds_since(start);

    // Where the per-listener cost actually goes. Timed one stage at a time
    // over the same work, because "a listener costs 0.2% of a core" does not
    // say whether that is worth attacking, and if so, where.
    struct Stage {
        const char* name;
        double seconds;
    };
    Stage stages[4] = {{"channel pull", 0.0}, {"AGC", 0.0}, {"demodulate", 0.0}, {"encode", 0.0}};
    for (int b = 0; b < blocks; b++) {
        if (kind == SignalKind::Real) channelizer.process_real(real_input.data());
        else channelizer.process(iq.data());
        for (int u = 0; u < users; u++) {
            auto mark = Clock::now();
            channels[u].pull(channelizer, baseband.data());
            stages[0].seconds += seconds_since(mark);

            mark = Clock::now();
            agcs[u].process(baseband.data(), baseband.size());
            stages[1].seconds += seconds_since(mark);

            mark = Clock::now();
            demodulators[u].process(baseband.data(), baseband.size(), audio.data());
            stages[2].seconds += seconds_since(mark);

            mark = Clock::now();
            for (size_t i = 0; i + nac::kFrameHop <= audio.size(); i += nac::kFrameHop) {
                encode(u, audio.data() + i);
            }
            stages[3].seconds += seconds_since(mark);
        }
    }

    const double stream_seconds = blocks * channelizer.block_seconds();
    const double per_user = (total_seconds - shared_seconds) / users;

    printf("input            %.3f Msps %s, FFT %zu, %.1f Hz bins\n", sample_rate / 1e6,
           kind == SignalKind::Real ? "real" : "IQ", fft_size, channelizer.bin_hz());
    printf("audio rate       %.0f Hz per listener\n", audio_rate);
    printf("block            %.2f ms\n", channelizer.block_seconds() * 1000.0);
    printf("shared transform %.2f%% of one core (independent of listener count)\n",
           100.0 * shared_seconds / stream_seconds);
    printf("per listener     %.4f%% of one core\n", 100.0 * per_user / stream_seconds);

    double measured = 0.0;
    for (const Stage& stage : stages) measured += stage.seconds;
    for (const Stage& stage : stages) {
        printf("  %-14s %5.1f%% of that\n", stage.name,
               measured > 0.0 ? 100.0 * stage.seconds / measured : 0.0);
    }
    printf("%-4d listeners   %.1f%% of one core in total\n", users,
           100.0 * total_seconds / stream_seconds);
}

void bench_latency() {
    rule("Pipeline latency (algorithmic, one band)");
    struct Case { double rate; size_t fft; const char* label; };
    const Case cases[] = {
        {192000.0, 4096, "192 kHz IQ"},
        {1536000.0, 32768, "1.536 MHz IQ"},
        {64000000.0, 1048576, "64 MHz real"},
    };
    printf("%-14s %11s %11s %11s %12s\n", "band", "block", "codec", "total", "audio rate");
    for (const auto& c : cases) {
        Channelizer channelizer(c.rate, c.fft, SignalKind::Iq);
        // The real rule: pick the decimation that lands the audio rate nearest
        // 12 kHz, which is what a listener actually gets.
        size_t decimation = 1;
        while (decimation * 2 <= static_cast<size_t>(c.rate / 12000.0 * 1.4142)) decimation *= 2;
        const size_t ifft = std::max<size_t>(16, c.fft / decimation);
        const double audio_rate = c.rate * ifft / c.fft;
        const double block_ms = channelizer.block_seconds() * 1000.0;
        // The codec's window is twice its hop, so it adds one hop of delay.
        const double codec_ms = 2.0 * nac::kFrameHop / audio_rate * 1000.0;
        printf("%-14s %8.2f ms %8.2f ms %8.2f ms %10.0f Hz\n", c.label, block_ms, codec_ms,
               block_ms + codec_ms, audio_rate);
    }
    printf("\nNothing above includes the network or the browser's buffer;\n"
           "tools/measure-latency.py measures those end to end against a live server.\n");
}

// What a waterfall costs per listener, with and without the shared mipmap.
// This is the number that decides whether a wideband receiver can carry a
// crowd: at 2^21 bins, a per-listener scan of the full line is two million
// comparisons per line per user, and 200 users at 20 lines/s is 8 Gcmp/s.
void bench_waterfall() {
    rule("Waterfall viewport cost (per listener, per line)");

    struct Case { size_t bins; double span_hz; const char* label; };
    const Case cases[] = {
        {32768, 1.536e6, "1.536 MHz, 32k bins"},
        {262144, 8e6, "8 MHz, 256k bins"},
        {2097152, 32e6, "32 MHz, 2M bins"},
    };
    const size_t width = 1500;  // a typical laptop waterfall

    printf("%-22s %12s %12s %10s\n", "band", "direct", "pyramid", "speedup");
    for (const Case& c : cases) {
        std::mt19937 rng(7);
        std::normal_distribution<float> noise(-110.0f, 4.0f);
        std::vector<float> line(c.bins);
        for (size_t i = 0; i < c.bins; i++) line[i] = noise(rng);

        SpectrumPyramid pyramid;
        std::vector<float> dst(width);
        const int reps = c.bins > 1000000 ? 20 : 200;

        auto start = Clock::now();
        for (int i = 0; i < reps; i++)
            render_viewport(line.data(), c.bins, 0.0, c.span_hz, 0.0, c.span_hz, dst.data(), width);
        const double direct = seconds_since(start) / reps;

        // The build is shared by every listener on the band, so it is charged
        // once here and not per user.
        start = Clock::now();
        for (int i = 0; i < reps; i++) pyramid.build(line.data(), c.bins, 0.0, c.span_hz);
        const double build = seconds_since(start) / reps;

        start = Clock::now();
        for (int i = 0; i < reps; i++) pyramid.render(0.0, c.span_hz, dst.data(), width);
        const double shared = seconds_since(start) / reps;

        printf("%-22s %9.1f us %9.2f us %8.0fx\n", c.label, direct * 1e6, shared * 1e6,
               direct / std::max(shared, 1e-12));
        printf("%-22s shared build %.0f us per line, once for the whole band\n", "", build * 1e6);
    }
    printf("\nThe pyramid also holds the peak: a carrier one bin wide survives\n"
           "every halving, so zooming out never makes a signal vanish.\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string what = argc > 1 ? argv[1] : "all";

    printf("FernSDR capacity benchmark (built %s)\n", __DATE__);

    if (what == "all" || what == "fft") bench_front_end();
    if (what == "all" || what == "users") {
        bench_listeners(192000.0, 4096, SignalKind::Iq, 200, "200 listeners, 192 kHz IQ band");
        bench_listeners(1536000.0, 32768, SignalKind::Iq, 200, "200 listeners, 1.536 MHz IQ band");
        bench_listeners(2048000.0, 32768, SignalKind::Real, 200,
                        "200 listeners, 2.048 Msps real band");
    }
    if (what == "all" || what == "waterfall") bench_waterfall();
    if (what == "wide") {
        bench_listeners(64000000.0, 1048576, SignalKind::Real, 200,
                        "200 listeners, 64 Msps real band (0-32 MHz)");
    }
    if (what == "all" || what == "latency") bench_latency();
    printf("\n");
    return 0;
}
