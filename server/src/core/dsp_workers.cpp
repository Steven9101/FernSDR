#include "dsp_workers.h"
#include <algorithm>

namespace fernsdr {
namespace { constexpr size_t kListenerChunk = 32; }
DspWorkers::DspWorkers(size_t workers, size_t max_batches)
    : queue_(workers * std::max<size_t>(1, max_batches), nullptr) {
    workers_.reserve(workers);
    try {
        for (size_t i = 0; i < workers; i++) workers_.emplace_back([this] { worker(); });
    } catch (...) {
        shutdown();
        throw;
    }
}

DspWorkers::~DspWorkers() { shutdown(); }

void DspWorkers::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    available_.notify_all();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
}

void DspWorkers::Job::fail() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!error) error = std::current_exception();
    failed.store(true, std::memory_order_relaxed);
}

void DspWorkers::Job::finish() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (--outstanding != 0) return;
    }
    // Completion can wake the network or interrupt a failed input. It must
    // finish before wait() can release the borrowed context and this job.
    try { if (complete) complete(context, failed.load(std::memory_order_relaxed)); }
    catch (...) { fail(); }
    {
        std::lock_guard<std::mutex> lock(mutex);
        completed = true;
        done.notify_all();
    }
}

void DspWorkers::Job::wait() {
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [this] { return completed; });
    if (error) std::rethrow_exception(error);
}

DspWorkers::Job::~Job() {
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [this] { return completed; });
}

void DspWorkers::Job::work() {
    try {
        while (!failed.load(std::memory_order_relaxed)) {
            const size_t first = next.fetch_add(kListenerChunk, std::memory_order_relaxed);
            if (first >= count) break;
            for (size_t i = first; i < std::min(count, first + kListenerChunk); i++) process(context, i);
        }
    } catch (...) { fail(); }
    finish();
}

void DspWorkers::worker() {
    for (;;) {
        Job* batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++idle_workers_;
            available_.wait(lock, [this] { return stopping_ || queued_ != 0; });
            --idle_workers_;
            if (queued_ == 0) return;
            batch = queue_[head_];
            head_ = (head_ + 1) % queue_.size();
            --queued_;
        }
        batch->work();
    }
}

void DspWorkers::run(size_t count, void (*process)(void*, size_t), void* context) {
    if (workers_.empty() || count < 2 * kListenerChunk) {
        for (size_t i = 0; i < count; i++) process(context, i);
        return;
    }
    Job batch{count, process, context};
    try {
        const size_t runners = std::min(workers_.size(), (count - 1) / kListenerChunk);
        for (size_t i = 0; i < runners; i++) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queued_ == queue_.size()) break;
            {
                std::lock_guard<std::mutex> batch_lock(batch.mutex);
                ++batch.outstanding;
            }
            queue_[(head_ + queued_) % queue_.size()] = &batch;
            ++queued_;
            available_.notify_one();
        }
    } catch (...) { batch.fail(); }
    batch.work();
    // Even a throwing caller/worker must wait before unwinding the snapshot
    // and FFT buffers that other runners still read. Submission never waits
    // for queue space: the caller helps consume the batch instead.
    batch.wait();
}

std::unique_ptr<DspWorkers::Job> DspWorkers::start(size_t count, void (*process)(void*, size_t),
                                                void* context, void (*complete)(void*, bool)) {
    std::unique_ptr<Job> job(new Job(count, process, context, complete));
    size_t submitted = 0;
    try {
        const size_t runners = std::min(workers_.size(), count ? 1 + (count - 1) / kListenerChunk : 0);
        for (size_t i = 0; i < runners; i++) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queued_ == queue_.size()) break;
            {
                std::lock_guard<std::mutex> job_lock(job->mutex);
                ++job->outstanding;
            }
            queue_[(head_ + queued_) % queue_.size()] = job.get();
            ++queued_;
            ++submitted;
            available_.notify_one();
        }
    } catch (...) { job->fail(); }
    if (submitted) job->finish();
    else job->work();
    return job;
}

void DspWorkers::parallel_pair(void (*first)(void*), void (*second)(void*), void* context) {
    struct Pair { void (*second)(void*); void* context; } pair{second, context};
    Job batch(1, [](void* opaque, size_t) {
        auto& pair = *static_cast<Pair*>(opaque);
        pair.second(pair.context);
    }, &pair);
    bool submitted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Offload only when a helper can start without waiting behind other
        // work. Otherwise the producer performs both transforms directly.
        if (!stopping_ && idle_workers_ > queued_ && queued_ < queue_.size()) {
            ++batch.outstanding;
            queue_[(head_ + queued_) % queue_.size()] = &batch;
            ++queued_;
            submitted = true;
        }
    }
    if (!submitted) {
        try {
            first(context);
            second(context);
        } catch (...) { batch.fail(); }
        batch.finish();
        batch.wait();
        return;
    }
    available_.notify_one();
    try { first(context); } catch (...) { batch.fail(); }
    batch.finish();
    batch.wait();
}
}
