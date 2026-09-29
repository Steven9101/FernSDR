#include "test_util.h"
#include "../src/core/band.h"
#include "../src/core/radio.h"
#include "../src/source/source_file.h"
#include "../src/source/source_test.h"
#include "../src/source/source_udp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <atomic>
#include <thread>

using namespace fernsdr;
using namespace std::chrono_literals;

namespace {
// A failed shutdown must fail the test, not strand the entire test runner.
// The child also contains std::terminate if a joinable thread is destroyed.
bool finishes(const std::function<bool()>& body) {
    const pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) ::_exit(body() ? 0 : 1);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int status = 0;
    do {
        if (::waitpid(child, &status, WNOHANG) == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return false;
}

struct SampleFile {
    char path[64] = "/tmp/receiver-source-test-XXXXXX";
    int fd = ::mkstemp(path);
    ~SampleFile() { if (fd >= 0) ::close(fd); ::unlink(path); }
};

ConfigSection file_section(const std::string& path) {
    ConfigSection section("band:test");
    section.set("source", "file");
    section.set("path", path);
    section.set("format", "s16");
    section.set("signal", "real");
    section.set("sample_rate", "48000");
    section.set("fft_size", "1024");
    section.set("realtime", "false");
    return section;
}

int unused_udp_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool ok = fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
                    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    if (fd >= 0) ::close(fd);
    return ok ? ntohs(address.sin_port) : 0;
}
}

TEST_CASE(band_can_start_again_after_file_eof_and_be_destroyed) {
    SampleFile file;
    CHECK(file.fd >= 0);
    int16_t samples[1024]{};
    CHECK_EQ(::write(file.fd, samples, sizeof(samples)), sizeof(samples));
    CHECK(finishes([&] {
        auto section = file_section(file.path);
        std::string error;
        Band band("test", "test", make_file_source(section, error), section);
        for (int i = 0; i < 3; i++) {
            if (!band.start(error)) return false;
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (band.running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            if (band.running()) return false;
        }
        return true;
    }));
}

TEST_CASE(empty_looping_file_is_rejected_without_a_reader_spin) {
    SampleFile file;
    auto section = file_section(file.path);
    section.set("loop", "true");
    std::string error;
    auto source = make_file_source(section, error);
    CHECK(source != nullptr);
    if (!source) return;
    CHECK(!source->start(error));
    CHECK(error.find("empty") != std::string::npos);
}

TEST_CASE(quiet_fifo_udp_and_partial_stdin_can_be_stopped) {
    SampleFile fifo;
    ::unlink(fifo.path);
    CHECK_EQ(::mkfifo(fifo.path, 0600), 0);
    for (int input = 0; input < 3; input++) {
        CHECK(finishes([&] {
            auto section = file_section(fifo.path);
            std::string error;
            std::unique_ptr<Source> source;
            int pipe_fds[2] = {-1, -1};
            if (input == 1) {
                section.set("bind", "127.0.0.1");
                section.set("port", std::to_string(unused_udp_port()));
                source = make_udp_source(section, error);
            } else {
                if (input == 2) {
                    if (::pipe(pipe_fds) != 0 || ::dup2(pipe_fds[0], STDIN_FILENO) < 0) return false;
                    if (::write(pipe_fds[1], "x", 1) != 1) return false;
                    section.set("path", "-");
                }
                source = make_file_source(section, error);
            }
            if (!source) return false;
            Band band("test", "test", std::move(source), section);
            for (int i = 0; i < 4; i++) {
                if (!band.start(error)) return false;
                std::this_thread::sleep_for(10ms);
                if (!band.restart()) return false;
                band.stop();
                if (band.running() || band.restarting() || band.restart()) return false;
            }
            for (int fd : pipe_fds) if (fd >= 0) ::close(fd);
            return true;
        }));
    }
}

TEST_CASE(paced_sources_can_interrupt_a_long_sleep_and_reset_the_clock) {
    SampleFile file;
    int16_t samples[2048]{};
    CHECK_EQ(::write(file.fd, samples, sizeof(samples)), sizeof(samples));
    for (bool synthetic : {false, true}) {
        CHECK(finishes([&] {
            auto section = file_section(file.path);
            section.set("sample_rate", "1");
            section.set("realtime", "true");
            std::string error;
            auto source = synthetic ? make_test_source(section, error) : make_file_source(section, error);
            if (!source || !source->start(error)) return false;
            float block[512];
            std::thread reader([&] { source->read_real(block, 512); });
            std::this_thread::sleep_for(20ms);
            source->interrupt();
            reader.join();
            source->stop();
            if (!source->start(error) || source->stats().samples_read != 0) return false;
            source->stop();
            return true;
        }));
    }
}

TEST_CASE(multiple_bands_cannot_consume_the_same_stdin_stream) {
    Config config;
    std::string error;
    CHECK(config.parse("[band:a]\nsource=stdin\nsample_rate=48000\n"
                       "[band:b]\nsource=pipe\npath=-\nsample_rate=48000\n", error));
    Radio radio;
    CHECK(!radio.configure(config, error));
    CHECK(error.find("only one band") != std::string::npos);
}

TEST_CASE(udp_input_takes_samples_only_from_its_listed_senders) {
    // Whatever a UDP band receives, every listener hears.
    for (const bool listed : {false, true}) {
        const int port = unused_udp_port();
        fernsdr::ConfigSection section("band:udp");
        section.set("source", "udp");
        section.set("bind", "127.0.0.1");
        section.set("port", std::to_string(port));
        section.set("format", "cs16");
        section.set("sample_rate", "48000");
        section.set("senders", listed ? "loopback" : "10.0.0.0/8, 192.168.0.0/16");
        std::string error;
        auto source = make_udp_source(section, error);
        CHECK(source != nullptr);
        if (!source || !source->start(error)) continue;
        std::atomic<bool> got{false};
        std::thread reader([&] {
            fernsdr::cfloat block[256];
            got = source->read(block, 256);
        });
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<uint16_t>(port));
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int16_t samples[1024]{};
        for (int i = 0; i < 3; i++) {
            ::sendto(fd, samples, sizeof(samples), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            std::this_thread::sleep_for(20ms);
        }
        std::this_thread::sleep_for(200ms);
        source->interrupt();
        reader.join();
        ::close(fd);
        source->stop();
        CHECK_EQ(got.load(), listed);
    }
}
