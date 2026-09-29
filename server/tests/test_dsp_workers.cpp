#include "../src/core/dsp_workers.h"
#include "../src/core/band.h"
#include "test_util.h"
#include <chrono>
#include <stdexcept>

using namespace fernsdr;

TEST_CASE(transform_pairs_finish_once_and_drain_both_exception_paths) {
    for (size_t workers : {0u, 1u, 3u}) {
        DspWorkers pool(workers, 1);
        for (int failure : {-1, 0, 1}) {
            struct Job { std::atomic<int> first{0}, second{0}, active{0}; int failure; } job{{0}, {0}, {0}, failure};
            bool caught = false;
            try {
                pool.parallel_pair([](void* opaque) {
                    auto& job = *static_cast<Job*>(opaque);
                    ++job.first;
                    if (job.failure == 0) throw std::runtime_error("first");
                }, [](void* opaque) {
                    auto& job = *static_cast<Job*>(opaque);
                    ++job.second;
                    if (job.failure == 1) throw std::runtime_error("second");
                    ++job.active;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    --job.active;
                }, &job);
            } catch (const std::runtime_error&) { caught = true; }
            CHECK_EQ(caught, failure >= 0);
            CHECK_EQ(job.first.load(), 1);
            CHECK_EQ(job.active.load(), 0);
            if (failure != 0) CHECK_EQ(job.second.load(), 1);
        }
    }
}

TEST_CASE(parallel_large_transforms_preserve_real_input_audio_and_waterfall_bytes) {
    DspWorkers pool(2, 1);
    ConfigSection section("band:test");
    section.set("source", "test");
    section.set("sample_rate", "64000000");
    section.set("signal", "real");
    section.set("fft_size", "1048576");
    section.set("spectrum_bins", "1048576");
    section.set("realtime", "false");
    std::string error;
    Band serial("test", "test", make_source(section, error), section);
    Band parallel("test", "test", make_source(section, error), section);
    parallel.set_dsp_workers(&pool);
    auto left = std::make_shared<Listener>(1, serial);
    auto right = std::make_shared<Listener>(1, parallel);
    serial.add_listener(left);
    parallel.add_listener(right);
    // Drive both bands synchronously so only the transform scheduling differs.
    for (int block = 0; block < 12; ++block) {
        CHECK(serial.process_one_block());
        CHECK(parallel.process_one_block());
        std::vector<std::vector<uint8_t>> a, b;
        left->drain(a);
        right->drain(b);
        CHECK(a == b);
    }
}

TEST_CASE(dsp_workers_cover_partial_chunks_once_with_concurrent_bands) {
    for (size_t workers : {0u, 1u, 3u}) {
        DspWorkers pool(workers, 1);
        struct Job { std::array<std::atomic<int>, 257> calls{}; } a, b;
        const auto process = [](void* context, size_t i) { ++static_cast<Job*>(context)->calls[i]; };
        std::thread first([&] { pool.run(a.calls.size(), process, &a); });
        std::thread second([&] { pool.run(b.calls.size(), process, &b); });
        first.join();
        second.join();
        for (size_t i = 0; i < a.calls.size(); i++) {
            CHECK_EQ(a.calls[i].load(), 1);
            CHECK_EQ(b.calls[i].load(), 1);
        }
    }
}

TEST_CASE(dsp_worker_failures_finish_the_batch_before_the_caller_unwinds) {
    DspWorkers pool(2, 1);
    for (bool worker_failure : {false, true}) {
        struct Job {
            std::thread::id caller = std::this_thread::get_id();
            bool worker_failure;
            std::atomic<int> active{0};
            std::atomic<bool> worker_seen{false};
        } job{std::this_thread::get_id(), worker_failure};
        bool caught = false;
        try {
            pool.run(1024, [](void* context, size_t) {
                auto& job = *static_cast<Job*>(context);
                struct Guard {
                    std::atomic<int>& active;
                    Guard(std::atomic<int>& value) : active(value) { ++active; }
                    ~Guard() { --active; }
                } guard(job.active);
                const bool worker = std::this_thread::get_id() != job.caller;
                if (worker) job.worker_seen.store(true);
                if (worker == job.worker_failure) throw std::runtime_error("injected DSP failure");
                if (!worker) {
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    while (!job.worker_seen.load() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
                } else std::this_thread::sleep_for(std::chrono::microseconds(100));
            }, &job);
        } catch (const std::runtime_error& failure) {
            caught = std::string(failure.what()) == "injected DSP failure";
        }
        CHECK(caught);
        CHECK_EQ(job.active.load(), 0);
    }
    size_t count = 0;
    pool.run(5, [](void* value, size_t) { ++*static_cast<size_t*>(value); }, &count);
    CHECK_EQ(count, 5u);
}

TEST_CASE(parallel_listener_streams_match_serial_streams_exactly) {
    DspWorkers pool(3, 1);
    ConfigSection section("band:test");
    section.set("source", "test");
    section.set("sample_rate", "192000");
    section.set("center", "7100000");
    section.set("realtime", "false");
    std::string error;
    Band serial("test", "test", make_source(section, error), section);
    Band parallel("test", "test", make_source(section, error), section);
    parallel.set_dsp_workers(&pool);
    std::vector<std::shared_ptr<Listener>> left, right;
    constexpr Mode modes[] = {Mode::Usb, Mode::Lsb, Mode::Cw, Mode::CwL, Mode::Am, Mode::Sam, Mode::Nfm, Mode::Dsb};
    for (size_t i = 0; i < 129; i++) {
        for (int side = 0; side < 2; side++) {
            auto listener = std::make_shared<Listener>(i + 1, side ? parallel : serial);
            auto settings = listener->channel();
            settings.mode = modes[i % 8];
            settings.frequency_hz += static_cast<double>(i) * 513 - 30000;
            apply_mode_defaults(settings);
            listener->set_channel(settings);
            auto viewport = listener->viewport();
            viewport.width = 64;
            listener->set_viewport(viewport);
            (side ? parallel : serial).add_listener(listener);
            (side ? right : left).push_back(listener);
        }
    }
    for (int block = 0; block < 12; block++) {
        CHECK(serial.process_one_block());
        CHECK(parallel.process_one_block());
        for (size_t i = 0; i < left.size(); i++) {
            std::vector<std::vector<uint8_t>> a, b;
            left[i]->drain(a);
            right[i]->drain(b);
            CHECK(a == b);
            if (block == 4) {
                auto setting = left[i]->channel();
                setting.frequency_hz += 127.25;
                left[i]->set_channel(setting);
                right[i]->set_channel(setting);
            }
        }
    }
}
