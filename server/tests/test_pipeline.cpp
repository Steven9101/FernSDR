#include "../src/core/band.h"
#include "../src/core/dsp_workers.h"
#include "test_util.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <stdexcept>

using namespace fernsdr;
using namespace std::chrono_literals;

namespace {

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    size_t wakes = 0;
};

class LimitedSource : public Source {
public:
    LimitedSource(std::unique_ptr<Source> source, size_t blocks, Gate* gate = nullptr, bool fail_end = false)
        : source_(std::move(source)), blocks_(blocks), remaining_(blocks), gate_(gate), fail_end_(fail_end) {}
    bool start(std::string& error) override {
        remaining_ = blocks_;
        if (gate_) {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->entered = gate_->released = false;
            gate_->wakes = 0;
        }
        return source_->start(error);
    }
    void interrupt() override {
        source_->interrupt();
        if (gate_) {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->released = true;
            gate_->changed.notify_all();
        }
    }
    void stop() override { interrupt(); source_->stop(); }
    SignalKind kind() const override { return source_->kind(); }
    double sample_rate() const override { return source_->sample_rate(); }
    double center_hz() const override { return source_->center_hz(); }
    SourceStats stats() const override { return source_->stats(); }
    const char* kind_name() const override { return "finite test"; }
    bool read(cfloat* out, size_t count) override { return next() && source_->read(out, count); }
    bool read_real(float* out, size_t count) override { return next() && source_->read_real(out, count); }

private:
    bool next() {
        if (remaining_) { --remaining_; return true; }
        if (gate_) {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            gate_->entered = true;
            gate_->changed.notify_all();
            if (fail_end_) throw std::runtime_error("source read failure");
            gate_->changed.wait(lock, [this] { return gate_->released; });
        }
        return false;
    }
    std::unique_ptr<Source> source_;
    size_t blocks_, remaining_;
    Gate* gate_;
    bool fail_end_;
};

ConfigSection pipeline_config(bool real) {
    ConfigSection section("band:test");
    section.set("source", "test");
    section.set("signal", real ? "real" : "iq");
    section.set("sample_rate", "16000000");
    section.set("center", "7100000");
    section.set("fft_size", "262144");
    section.set("spectrum_bins", "262144");
    section.set("spectrum_rate", "60");
    section.set("spectrum_averages", "1");
    section.set("realtime", "false");
    return section;
}

std::vector<std::shared_ptr<Listener>> add_listeners(Band& band, size_t count) {
    constexpr Mode modes[] = {Mode::Usb, Mode::Lsb, Mode::Cw, Mode::CwL,
                              Mode::Am, Mode::Sam, Mode::Nfm, Mode::Dsb};
    std::vector<std::shared_ptr<Listener>> listeners;
    for (size_t i = 0; i < count; i++) {
        auto listener = std::make_shared<Listener>(i + 1, band);
        auto settings = listener->channel();
        settings.mode = modes[i % 8];
        settings.frequency_hz += static_cast<double>(i) * 127.25 - 7000;
        apply_mode_defaults(settings);
        listener->set_channel(settings);
        auto viewport = listener->viewport();
        viewport.width = 64;
        listener->set_viewport(viewport);
        band.add_listener(listener);
        listeners.push_back(listener);
    }
    return listeners;
}

bool wait_for_end(Band& band) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (band.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return !band.running();
}

}

TEST_CASE(async_dsp_jobs_finish_each_item_and_report_failures_once) {
    for (size_t workers : {0u, 1u, 3u}) {
        DspWorkers pool(workers, 1);
        for (bool fail : {false, true}) {
            struct State {
                std::array<std::atomic<int>, 257> calls{};
                std::atomic<int> completed{0};
                bool fail, reported = false;
            } state{{}, {0}, fail};
            auto job = pool.start(state.calls.size(), [](void* context, size_t i) {
                auto& state = *static_cast<State*>(context);
                ++state.calls[i];
                if (state.fail && i == 100) throw std::runtime_error("async failure");
            }, &state, [](void* context, bool failed) {
                auto& state = *static_cast<State*>(context);
                state.reported = failed;
                ++state.completed;
            });
            bool caught = false;
            try { job->wait(); }
            catch (const std::runtime_error& error) { caught = std::string(error.what()) == "async failure"; }
            CHECK_EQ(caught, fail);
            CHECK_EQ(state.reported, fail);
            CHECK_EQ(state.completed.load(), 1);
            for (auto& count : state.calls) {
                CHECK(count.load() <= 1);
                if (!fail) CHECK_EQ(count.load(), 1);
            }
        }
    }
}

TEST_CASE(async_dsp_job_destructor_waits_for_completion_callback) {
    DspWorkers pool(1, 1);
    struct State { Gate processing, completion; } state;
    auto job = pool.start(1, [](void* context, size_t) {
        auto& gate = static_cast<State*>(context)->processing;
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.released; });
    }, &state, [](void* context, bool) {
        auto& gate = static_cast<State*>(context)->completion;
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate] { return gate.released; });
    });
    // start() may run completion on the caller when workers finish before
    // submission ends. Hold processing until it returns so this test can
    // release a worker's gated completion from the caller.
    {
        std::lock_guard<std::mutex> lock(state.processing.mutex);
        state.processing.released = true;
        state.processing.changed.notify_all();
    }
    auto& gate = state.completion;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, 2s, [&gate] { return gate.entered; }));
    }
    auto drain = std::async(std::launch::async, [job = std::move(job)]() mutable { job.reset(); });
    CHECK(drain.wait_for(10ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
        gate.changed.notify_all();
    }
    CHECK(drain.wait_for(2s) == std::future_status::ready);
    drain.get();
}

TEST_CASE(async_dsp_job_reports_completion_callback_failure) {
    DspWorkers pool(1, 1);
    auto job = pool.start(1, [](void*, size_t) {}, nullptr, [](void*, bool) {
        throw std::runtime_error("completion failure");
    });
    bool caught = false;
    try { job->wait(); }
    catch (const std::runtime_error& error) { caught = std::string(error.what()) == "completion failure"; }
    CHECK(caught);
}

TEST_CASE(async_dsp_full_queue_runs_on_the_caller_and_reports_completion_failure) {
    DspWorkers pool(1, 1);
    Gate gate;
    struct Work {
        std::array<std::atomic<int>, 65> calls{};
        int completed = 0;
        Gate* gate = nullptr;
        bool throw_on_completion = false;
        std::thread::id completion_thread;
    } first, second, third;
    first.gate = &gate;
    third.throw_on_completion = true;
    const auto process = [](void* context, size_t index) {
        auto& work = *static_cast<Work*>(context);
        ++work.calls[index];
        if (index == 0 && work.gate) {
            std::unique_lock<std::mutex> lock(work.gate->mutex);
            work.gate->entered = true;
            work.gate->changed.notify_all();
            work.gate->changed.wait(lock, [&work] { return work.gate->released; });
        }
    };
    const auto complete = [](void* context, bool) {
        auto& work = *static_cast<Work*>(context);
        ++work.completed;
        work.completion_thread = std::this_thread::get_id();
        if (work.throw_on_completion) throw std::runtime_error("full queue completion");
    };
    auto active = pool.start(65, process, &first, complete);
    bool entered;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        entered = gate.changed.wait_for(lock, 2s, [&gate] { return gate.entered; });
        CHECK(entered);
        if (!entered) {
            gate.released = true;
            gate.changed.notify_all();
        }
    }
    if (!entered) { active->wait(); return; }
    auto queued = pool.start(65, process, &second, complete);
    auto fallback = pool.start(65, process, &third, complete);
    CHECK(third.completion_thread == std::this_thread::get_id());
    bool caught = false;
    try { fallback->wait(); }
    catch (const std::runtime_error& error) { caught = std::string(error.what()) == "full queue completion"; }
    CHECK(caught);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
        gate.changed.notify_all();
    }
    active->wait();
    queued->wait();
    for (const auto* work : {&first, &second, &third}) {
        CHECK_EQ(work->completed, 1);
        for (const auto& calls : work->calls) CHECK_EQ(calls.load(), 1);
    }
}

TEST_CASE(pipelined_bands_match_serial_audio_and_waterfall_through_eof) {
    for (bool real : {false, true}) {
        const auto section = pipeline_config(real);
        std::string error;
        auto source = std::make_unique<LimitedSource>(make_source(section, error), 12);
        CHECK(source->start(error));
        DspWorkers pool(3, 1);
        Band serial("test", "test", std::move(source), section);
        Band pipelined("test", "test",
                       std::make_unique<LimitedSource>(make_source(section, error), 12), section);
        pipelined.set_dsp_workers(&pool);
        auto left = add_listeners(serial, 129), right = add_listeners(pipelined, 129);
        for (int block = 0; block < 12; block++) CHECK(serial.process_one_block());
        CHECK(!serial.process_one_block());
        CHECK(pipelined.start(error));
        CHECK(wait_for_end(pipelined));
        pipelined.stop();
        CHECK(pipelined.last_error().empty());
        for (size_t i = 0; i < left.size(); i++) {
            std::vector<std::vector<uint8_t>> a, b;
            left[i]->drain(a);
            right[i]->drain(b);
            CHECK(!a.empty());
            CHECK(a == b);
        }
    }
}

TEST_CASE(pipelined_bands_share_a_single_worker_and_bounded_queue) {
    const auto section = pipeline_config(true);
    std::string error;
    auto source = std::make_unique<LimitedSource>(make_source(section, error), 12);
    CHECK(source->start(error));
    Band serial("reference", "reference", std::move(source), section);
    auto expected = add_listeners(serial, 65);
    for (int block = 0; block < 12; block++) CHECK(serial.process_one_block());
    CHECK(!serial.process_one_block());

    DspWorkers pool(1, 1);
    Band first("first", "first",
        std::make_unique<LimitedSource>(make_source(section, error), 12), section);
    Band second("second", "second",
        std::make_unique<LimitedSource>(make_source(section, error), 12), section);
    first.set_dsp_workers(&pool);
    second.set_dsp_workers(&pool);
    auto left = add_listeners(first, 65), right = add_listeners(second, 65);
    CHECK(first.start(error));
    CHECK(second.start(error));
    CHECK(wait_for_end(first));
    CHECK(wait_for_end(second));
    first.stop();
    second.stop();
    CHECK(first.last_error().empty());
    CHECK(second.last_error().empty());
    for (size_t i = 0; i < expected.size(); i++) {
        std::vector<std::vector<uint8_t>> a, b, c;
        expected[i]->drain(a);
        left[i]->drain(b);
        right[i]->drain(c);
        CHECK(!a.empty());
        CHECK(a == b);
        CHECK(a == c);
    }
}

TEST_CASE(pipeline_delivers_audio_while_next_input_read_is_blocked) {
    const auto section = pipeline_config(true);
    DspWorkers pool(2, 1);
    Gate gate;
    std::string error;
    // Two blocks: a listener in a narrow mode runs at 7812.5 Hz here, 64
    // samples a block, and a codec frame takes 128.
    Band band("test", "test",
        std::make_unique<LimitedSource>(make_source(section, error), 2, &gate), section);
    band.set_dsp_workers(&pool);
    auto listeners = add_listeners(band, 64);
    band.set_wake_callback([&gate] {
        std::lock_guard<std::mutex> lock(gate.mutex);
        ++gate.wakes;
        gate.changed.notify_all();
    });
    CHECK(band.start(error));
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, 3s, [&gate] { return gate.entered && gate.wakes >= 2; }));
        CHECK(!gate.released);
    }
    for (const auto& listener : listeners) {
        std::vector<std::vector<uint8_t>> frames;
        listener->drain(frames);
        CHECK(!frames.empty());
        band.remove_listener(listener->id());
    }
    const std::weak_ptr<Listener> released = listeners.front();
    listeners.clear();
    CHECK(released.expired());
    band.stop();
    CHECK(released.expired());
    CHECK(band.last_error().empty());
}

TEST_CASE(pipeline_completion_failure_interrupts_input_and_stops_band) {
    const auto section = pipeline_config(true);
    DspWorkers pool(2, 1);
    Gate gate;
    std::string error;
    Band band("test", "test",
        std::make_unique<LimitedSource>(make_source(section, error), 1, &gate), section);
    band.set_dsp_workers(&pool);
    const auto listeners = add_listeners(band, 64);
    band.set_wake_callback([] { throw std::runtime_error("network wake failure"); });
    CHECK(band.start(error));
    CHECK(wait_for_end(band));
    band.stop();
    CHECK_EQ_STR(band.last_error(), "network wake failure");
}

TEST_CASE(pipeline_source_failure_drains_the_previous_completion_before_stopping) {
    const auto section = pipeline_config(true);
    DspWorkers pool(1, 1);
    Gate worker_gate, read_gate, completion_gate;
    auto blocker = pool.start(1, [](void* context, size_t) {
        auto& gate = *static_cast<Gate*>(context);
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate] { return gate.released; });
    }, &worker_gate);
    bool entered;
    {
        std::unique_lock<std::mutex> lock(worker_gate.mutex);
        entered = worker_gate.changed.wait_for(lock, 2s, [&worker_gate] { return worker_gate.entered; });
        CHECK(entered);
        if (!entered) {
            worker_gate.released = true;
            worker_gate.changed.notify_all();
        }
    }
    if (!entered) { blocker->wait(); return; }
    std::string error;
    Band band("test", "test",
        std::make_unique<LimitedSource>(make_source(section, error), 1, &read_gate, true), section);
    band.set_dsp_workers(&pool);
    auto listeners = add_listeners(band, 64);
    band.set_wake_callback([&completion_gate] {
        std::unique_lock<std::mutex> lock(completion_gate.mutex);
        completion_gate.entered = true;
        completion_gate.changed.notify_all();
        completion_gate.changed.wait(lock, [&completion_gate] { return completion_gate.released; });
    });
    CHECK(band.start(error));
    {
        std::unique_lock<std::mutex> lock(read_gate.mutex);
        CHECK(read_gate.changed.wait_for(lock, 3s, [&read_gate] { return read_gate.entered; }));
    }
    // The listener batch could not complete during submission because the
    // only worker was occupied. Release it after the next read has failed,
    // then hold its callback while stop() tries to join the band.
    {
        std::lock_guard<std::mutex> lock(worker_gate.mutex);
        worker_gate.released = true;
        worker_gate.changed.notify_all();
    }
    blocker->wait();
    {
        std::unique_lock<std::mutex> lock(completion_gate.mutex);
        CHECK(completion_gate.changed.wait_for(lock, 3s, [&completion_gate] { return completion_gate.entered; }));
    }
    auto stop = std::async(std::launch::async, [&band] { band.stop(); });
    CHECK(stop.wait_for(10ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(completion_gate.mutex);
        completion_gate.released = true;
        completion_gate.changed.notify_all();
    }
    CHECK(stop.wait_for(3s) == std::future_status::ready);
    stop.get();
    CHECK_EQ_STR(band.last_error(), "source read failure");
    const std::weak_ptr<Listener> released = listeners.front();
    for (const auto& listener : listeners) band.remove_listener(listener->id());
    listeners.clear();
    CHECK(released.expired());
}
