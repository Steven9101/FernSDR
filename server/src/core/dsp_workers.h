#pragma once
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fernsdr {

// Bands own source reads. Independent transforms and listener work may enter
// this pool. run() is a barrier; start() returns a job whose wait
// and destructor provide the same barrier, including its completion callback.
// Samples and context remain alive and immutable until that barrier finishes.
// Callers are band threads, never callbacks already running on a worker.
class DspWorkers {
public:
    DspWorkers(size_t workers, size_t max_batches);
    ~DspWorkers();
    DspWorkers(const DspWorkers&) = delete;
    DspWorkers& operator=(const DspWorkers&) = delete;
    bool has_workers() const { return !workers_.empty(); }
    void run(size_t count, void (*process)(void*, size_t), void* context);
    void parallel_pair(void (*first)(void*), void (*second)(void*), void* context);

    class Job {
    public:
        ~Job();
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
        void wait();

    private:
        friend class DspWorkers;
        Job(size_t count, void (*process)(void*, size_t), void* context,
            void (*complete)(void*, bool) = nullptr)
            : count(count), process(process), context(context), complete(complete) {}
        size_t count;
        void (*process)(void*, size_t);
        void* context;
        void (*complete)(void*, bool);
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::mutex mutex;
        std::condition_variable done;
        size_t outstanding = 1;
        bool completed = false;
        std::exception_ptr error;
        void fail();
        void work();
        void finish();
    };
    // No queue-space wait. A full queue or an empty pool runs the job on the
    // caller. `complete` runs once, also after failure, before the barrier.
    std::unique_ptr<Job> start(size_t count, void (*process)(void*, size_t), void* context,
                               void (*complete)(void*, bool) = nullptr);

private:
    void worker();
    void shutdown();
    std::mutex mutex_;
    std::condition_variable available_;
    std::vector<std::thread> workers_;
    std::vector<Job*> queue_;
    size_t head_ = 0, queued_ = 0;
    size_t idle_workers_ = 0;
    bool stopping_ = false;
};
}
