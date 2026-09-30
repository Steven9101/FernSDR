#include "source_file.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "../util/log.h"

namespace fernsdr {

namespace {

class FileSource : public Source {
public:
    FileSource(std::string path, SourceFormat format, double sample_rate, double center_hz,
               bool realtime, bool loop)
        : path_(std::move(path)),
          format_(format),
          sample_rate_(sample_rate),
          center_hz_(center_hz),
          realtime_(realtime),
          loop_(loop) {}

    ~FileSource() override { stop(); }

    bool start(std::string& error) override {
        stop();
        interrupted_.store(false);
        total_samples_.store(0);
        if (path_ == "-" || path_ == "stdin") {
            fd_ = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 0);
            is_stream_ = true;
        } else {
            fd_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            // A FIFO delivers at the producer's pace, so pacing must be off;
            // a regular file has to be paced or it would replay at disk speed.
            if (fd_ >= 0) is_stream_ = !is_regular_file(fd_);
        }
        if (fd_ < 0) {
            error = "cannot open " + path_ + ": " + std::strerror(errno);
            return false;
        }
        inherited_blocking_ = path_ == "-" || path_ == "stdin";
        struct stat source_info{};
        is_fifo_ = !inherited_blocking_ && ::fstat(fd_, &source_info) == 0 && S_ISFIFO(source_info.st_mode);
        if (!is_stream_ && loop_) {
            struct stat info{};
            if (::fstat(fd_, &info) == 0 && info.st_size == 0) {
                error = "cannot loop an empty sample file: " + path_;
                stop();
                return false;
            }
        }
        started_ = std::chrono::steady_clock::now();
        connected_ = !is_fifo_;
        return true;
    }

    void stop() override {
        interrupt();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        connected_ = false;
    }

    void interrupt() override { interrupted_.store(true); }

    SignalKind kind() const override { return format_.kind; }

    bool read(cfloat* out, size_t count) override {
        if (format_.kind != SignalKind::Iq) return false;
        if (!fill(count)) return false;
        convert_iq(format_.type, raw_.data(), out, count);
        total_samples_ += count;
        if (realtime_ && !is_stream_) pace();
        return !interrupted_.load();
    }

    bool read_real(float* out, size_t count) override {
        if (format_.kind != SignalKind::Real) return false;
        if (!fill(count)) return false;
        convert_real(format_.type, raw_.data(), out, count);
        total_samples_ += count;
        if (realtime_ && !is_stream_) pace();
        return !interrupted_.load();
    }

private:
    // Reads exactly `count` samples' worth of bytes, looping or ending as
    // configured.
    bool fill(size_t count) {
        const size_t stride = bytes_per_sample(format_.type, format_.kind);
        raw_.resize(count * stride);

        size_t filled = 0;
        bool rewound_without_data = false;
        while (filled < raw_.size()) {
            if (interrupted_.load()) return false;
            // dup() shares stdin's status flags with the original descriptor.
            // Do not make it nonblocking behind another user's back. Radio
            // rejects multiple stdin bands, so readiness cannot be consumed
            // by a second reader between this poll and read.
            if (inherited_blocking_ && !wait_readable()) return false;
            const ssize_t got = ::read(fd_, raw_.data() + filled, raw_.size() - filled);
            if (got > 0) {
                connected_ = true;
                filled += static_cast<size_t>(got);
                rewound_without_data = false;
                continue;
            }
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!wait_readable()) return false;
                    continue;
                }
                LOG_ERROR("source", "read from %s failed: %s", path_.c_str(), std::strerror(errno));
                connected_ = false;
                return false;
            }
            if (is_fifo_) {
                // Keep the reader open for a producer that starts later or
                // reconnects. POLLHUP stays ready after a writer closes, so
                // polling it alone would spin. Discard a partial old block,
                // and say that the next one does not follow the last.
                connected_ = false;
                filled = 0;
                discontinuity_ = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            // End of file.
            if (loop_ && !is_stream_ && !rewound_without_data && ::lseek(fd_, 0, SEEK_SET) == 0) {
                rewound_without_data = true;
                LOG_DEBUG("source", "%s reached end, looping", path_.c_str());
                continue;
            }
            LOG_INFO("source", "%s reached end of stream", path_.c_str());
            connected_ = false;
            return false;
        }
        return true;
    }

public:
    double sample_rate() const override { return sample_rate_; }
    double center_hz() const override { return center_hz_; }
    SourceStats stats() const override { return {total_samples_.load(), 0, connected_.load()}; }
    const char* kind_name() const override { return "file"; }
    bool take_discontinuity() override { return std::exchange(discontinuity_, false); }

private:
    bool wait_readable() {
        while (!interrupted_.load()) {
            pollfd descriptor{fd_, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready > 0) return !interrupted_.load();
            if (ready < 0 && errno != EINTR) {
                connected_ = false;
                return false;
            }
        }
        return false;
    }

    static bool is_regular_file(int fd) {
        struct stat info;
        if (fstat(fd, &info) != 0) return false;
        return S_ISREG(info.st_mode);
    }

    void pace() {
        const auto target = started_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(total_samples_.load() / sample_rate_));
        while (!interrupted_.load()) {
            const auto remaining = target - std::chrono::steady_clock::now();
            if (remaining <= decltype(remaining)::zero()) break;
            std::this_thread::sleep_for(std::min(remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(100))));
        }
    }

    std::string path_;
    SourceFormat format_;
    double sample_rate_;
    double center_hz_;
    bool realtime_;
    bool loop_;

    int fd_ = -1;
    bool inherited_blocking_ = false;
    bool is_stream_ = false;
    bool is_fifo_ = false;
    // Set and cleared on the band's thread only: fill() runs inside read().
    bool discontinuity_ = false;
    std::atomic<bool> connected_{false};
    std::atomic<bool> interrupted_{false};
    std::atomic<uint64_t> total_samples_{0};
    std::vector<uint8_t> raw_;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace

std::unique_ptr<Source> make_file_source(const ConfigSection& section, std::string& error) {
    const std::string kind = section.get("source", "file");
    std::string path = section.get("path", kind == "stdin" ? "-" : "");
    if (path.empty()) {
        error = "[" + section.name() + "] source=" + kind + " needs a 'path' (use '-' for stdin)";
        return nullptr;
    }

    SourceFormat format;
    if (!parse_source_format(section, format, error)) return nullptr;

    const double sample_rate = section.get_double("sample_rate", 0.0);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        error = "[" + section.name() + "] sample_rate is required for a file source";
        return nullptr;
    }

    return std::make_unique<FileSource>(path, format, sample_rate, section.get_double("center", 0.0),
                                        section.get_bool("realtime", true),
                                        section.get_bool("loop", false));
}

}  // namespace fernsdr
