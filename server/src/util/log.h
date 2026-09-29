// Minimal leveled logger. Thread-safe enough: single write() per line.
//
// It also keeps the last few hundred lines in memory, so the admin panel can
// show an operator why a source died without them needing shell access to the
// machine. The ring is small and fixed: a receiver runs for months, and a log
// buffer that grows is a leak with a good excuse.
#pragma once
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fernsdr {

/** The in-memory tail of the log, for the admin panel. */
class LogRing {
public:
    static constexpr size_t kCapacity = 400;

    static LogRing& instance() {
        static LogRing ring;
        return ring;
    }

    void push(std::string line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.size() >= kCapacity) lines_.pop_front();
        lines_.push_back(std::move(line));
    }

    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {lines_.begin(), lines_.end()};
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
};

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, None };

inline std::atomic<int>& log_level() {
    static std::atomic<int> lvl{static_cast<int>(LogLevel::Info)};
    return lvl;
}

inline void set_log_level(LogLevel l) { log_level().store(static_cast<int>(l)); }

inline bool log_level_from_name(const char* name, LogLevel& out) {
    struct { const char* n; LogLevel l; } table[] = {
        {"trace", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"info", LogLevel::Info},
        {"warn", LogLevel::Warn},   {"error", LogLevel::Error}, {"none", LogLevel::None},
    };
    for (auto& e : table) {
        const char* a = e.n; const char* b = name;
        while (*a && *b && (*a == (*b | 0x20))) { ++a; ++b; }
        if (!*a && !*b) { out = e.l; return true; }
    }
    return false;
}

inline void log_write(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (static_cast<int>(lvl) < log_level().load(std::memory_order_relaxed)) return;
    static const char* names[] = {"TRC", "DBG", "INF", "WRN", "ERR"};

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    const int written = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // A message longer than the buffer is cut, and the cut must not leave
    // half a UTF-8 character for the panel's JSON to choke on.
    if (written >= static_cast<int>(sizeof(msg))) {
        size_t end = sizeof(msg) - 1;
        while (end > 0 && (static_cast<unsigned char>(msg[end]) & 0xC0) == 0x80) end--;
        if (end > 0 && static_cast<unsigned char>(msg[end]) >= 0xC0) msg[end] = '\0';
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);

    char line[1280];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d %s [%s] %s\n",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ts.tv_nsec / 1000000),
                     names[static_cast<int>(lvl)], tag, msg);
    if (n > 0) {
        const size_t length = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n)
                                                                    : sizeof(line) - 1;
        fwrite(line, 1, length, stderr);
        // Without the trailing newline: the panel puts each line in its own
        // element and a stray \n would show up as an empty row.
        LogRing::instance().push(std::string(line, length > 0 ? length - 1 : 0));
    }
}

}  // namespace fernsdr

#define LOG_TRACE(tag, ...) ::fernsdr::log_write(::fernsdr::LogLevel::Trace, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) ::fernsdr::log_write(::fernsdr::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  ::fernsdr::log_write(::fernsdr::LogLevel::Info,  tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  ::fernsdr::log_write(::fernsdr::LogLevel::Warn,  tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) ::fernsdr::log_write(::fernsdr::LogLevel::Error, tag, __VA_ARGS__)
