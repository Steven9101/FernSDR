// Child processes: modules, and the helpers the module manager runs.
//
// Everything a child gets is decided here and nowhere else. It starts from an
// absolute path with a fixed argument list, no shell and a given environment;
// it holds exactly the descriptors it was given, whatever the rest of this
// process has open; it runs in a process group of its own, so stopping it
// stops anything it started; and it is always reaped.
#pragma once
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace fernsdr {

// Set from the signal handler when the receiver is going down, so nothing
// starts a new child after that. Lock-free, so safe to set from a handler.
void request_shutdown();
bool shutdown_requested();

class Subprocess {
public:
    // What the child sees on one of its descriptors, by position: element 0
    // is its fd 0, and so on.
    enum class Stream {
        Null,       // /dev/null, read and write
        ToChild,    // a pipe this process writes and the child reads
        FromChild,  // a pipe the child writes and this process reads
    };

    struct Options {
        std::string path;                   // absolute; there is no PATH search
        std::vector<std::string> arguments; // after argv[0], which is `path`
        std::vector<std::string> environment;  // "NAME=value", and nothing else
        std::vector<Stream> streams;
    };

    struct Exit {
        bool exited = false;     // ended by exit(); `code` is its status
        int code = 0;
        int signal = 0;          // ended by this signal, when not exited
        bool abandoned = false;  // did not die even after SIGKILL; see terminate()
        std::string describe() const;
    };

    Subprocess() = default;
    ~Subprocess();
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    bool start(const Options& options, std::string& error);
    bool started() const { return pid_ > 0; }
    pid_t pid() const { return pid_; }

    // This process's end of the pipe on the child's descriptor `child_fd`,
    // non-blocking and close-on-exec; -1 when there is none or it was closed.
    int fd(size_t child_fd) const { return child_fd < ends_.size() ? ends_[child_fd] : -1; }
    void close_fd(size_t child_fd);

    // Writes to the pipe on the child's descriptor `child_fd` without the
    // SIGPIPE a child that has gone would otherwise raise in this process:
    // that must be an error to handle, not the end of the receiver. Returns
    // what write(2) would, with errno set.
    ssize_t write_to(size_t child_fd, const char* data, size_t size);

    // True, with the status, once the child has ended; never blocks.
    bool poll_exit(Exit& out);
    const Exit& exit_status() const { return exit_; }

    // Asks the child to end and makes sure it does: closes the descriptors
    // listed, waits `grace`, sends SIGTERM to its process group, waits `grace`
    // again, then SIGKILL. `pump` runs every few milliseconds meanwhile, so a
    // caller can keep draining the child's output and catch its last words.
    //
    // A process stuck in the kernel, as a USB driver can leave one, ignores
    // even SIGKILL until the kernel lets go. After `kill_wait` it is handed
    // to a thread that waits for it alone, and this returns with `abandoned`
    // set rather than holding up whoever is stopping it.
    Exit terminate(const std::vector<size_t>& close_first, std::chrono::milliseconds grace,
                   std::chrono::milliseconds kill_wait, const std::function<void()>& pump = {});

    // Runs a program to completion with fd 0 on /dev/null, or reading
    // `input` when there is one, collecting what it prints. Output past
    // `limit` bytes, a run past `timeout`, or `cancel` becoming true kills it
    // and counts as failure; `error` then says which. Data a program should
    // get goes in `input` rather than an argument when other users of the
    // machine must not see it: arguments are in /proc for anyone to read.
    static bool run(const std::string& path, const std::vector<std::string>& arguments,
                    const std::vector<std::string>& environment, std::chrono::milliseconds timeout,
                    size_t limit, std::string& output, std::string& errors, Exit& status,
                    std::string& error, const std::atomic<bool>* cancel = nullptr,
                    const std::string* input = nullptr);

    // For tests: take the path that closes inherited descriptors by listing
    // /proc/self/fd, which is what runs on C libraries older than glibc 2.34.
    static void force_descriptor_listing(bool force);

private:
    void close_all();

    pid_t pid_ = -1;
    std::vector<int> ends_;
    Exit exit_;
    bool reaped_ = false;
};

}  // namespace fernsdr
