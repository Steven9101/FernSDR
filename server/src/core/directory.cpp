#include "directory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "../util/json.h"
#include "../util/log.h"
#include "../util/programs.h"
#include "../util/subprocess.h"
#include "../util/utf8.h"

namespace fernsdr {

namespace {

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// What curl's exit says about a report that did not arrive, in words an
// operator can act on.
std::string describe_failure(const Subprocess::Exit& status, const std::string& errors) {
    const size_t at = errors.find("returned error: ");
    if (at != std::string::npos) {
        const int http = std::atoi(errors.c_str() + at + 16);
        if (http == 400) return "sdr-list.xyz refused the report (HTTP 400); check the grid square and the public address";
        if (http >= 100 && http < 600) return "sdr-list.xyz answered with HTTP " + std::to_string(http);
    }
    if (status.exited) {
        switch (status.code) {
            case 6: return "sdr-list.xyz could not be resolved; is this machine on the internet?";
            case 7: return "the connection to sdr-list.xyz was refused or timed out";
            case 28: return "sdr-list.xyz took too long to answer";
            case 35:
            case 60: return "the secure connection failed; check the machine's clock and CA certificates";
            default: break;
        }
    }
    const std::string detail = printable(errors.substr(0, errors.find('\n')), 200);
    return "curl " + status.describe() + (detail.empty() ? "" : " (" + detail + ")");
}

DirectoryListing::Send curl_sender() {
    return [](const std::string& body, std::string& error) {
        // Looked for at each report, so curl installed later is found
        // without restarting the receiver.
        const std::string curl = find_program("curl");
        if (curl.empty()) {
            error = "curl is not installed; install it to be listed";
            return false;
        }
        // Only HTTPS, no redirects: the report goes to one fixed address. The
        // body goes on standard input rather than the command line, where
        // any user of the machine could read the receiver's id, which the
        // directory does not publish and which marks the entry as its own.
        const std::vector<std::string> arguments = {
            "-q", "--silent", "--show-error", "--fail", "--proto", "=https", "--max-redirs", "0",
            "--max-time", "10", "--max-filesize", "65536", "--user-agent", "FernSDR",
            "--header", "Content-Type: application/json", "--data-binary", "@-",
            "--output", "/dev/null", DirectoryListing::kUrl,
        };
        std::string output, errors;
        Subprocess::Exit status;
        if (!Subprocess::run(curl, arguments, network_helper_environment(), std::chrono::seconds(15), 64 * 1024,
                             output, errors, status, error, nullptr, &body)) {
            return false;
        }
        if (status.exited && status.code == 0) return true;
        error = describe_failure(status, errors);
        return false;
    };
}

}  // namespace

std::string directory_report_json(const DirectoryReport& report) {
    Json body = Json::make_object();
    body.set("receiver_id", report.receiver_id);
    body.set("id", report.instance_id);
    body.set("hostname", report.host);
    body.set("port", report.port);
    body.set("grid_locator", report.grid);
    body.set("name", report.name);
    body.set("antenna", report.antenna);
    body.set("users", report.users);
    body.set("max_users", report.max_users);
    body.set("software", "FernSDR");
    body.set("receiver_count", static_cast<int>(report.bands.size()));
    if (!report.bands.empty()) {
        // The directory shows one range and adds bandwidths up across its
        // receivers, so a receiver of several bands reports the range they
        // span and the width they actually cover.
        double lowest = report.bands.front().first, highest = report.bands.front().second, width = 0;
        for (const auto& [low, high] : report.bands) {
            lowest = std::min(lowest, low);
            highest = std::max(highest, high);
            width += high - low;
        }
        body.set("range_start_hz", static_cast<double>(std::llround(lowest)));
        body.set("range_end_hz", static_cast<double>(std::llround(highest)));
        body.set("center_frequency", static_cast<double>(std::llround((lowest + highest) / 2)));
        body.set("bandwidth", static_cast<double>(std::llround(width)));
    }
    return body.serialize();
}

bool valid_grid_locator(const std::string& grid) {
    if (grid.size() != 4 && grid.size() != 6) return false;
    const auto field = [](char c) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return c >= 'A' && c <= 'R'; };
    const auto square = [](char c) { return c >= '0' && c <= '9'; };
    const auto subsquare = [](char c) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return c >= 'a' && c <= 'x'; };
    if (!field(grid[0]) || !field(grid[1]) || !square(grid[2]) || !square(grid[3])) return false;
    return grid.size() == 4 || (subsquare(grid[4]) && subsquare(grid[5]));
}

bool valid_public_host(const std::string& host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-' || host.back() == '.') {
        return false;
    }
    return std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
}

DirectoryListing::DirectoryListing() : DirectoryListing(curl_sender()) {}

DirectoryListing::DirectoryListing(Send send) : send_(std::move(send)) {
    thread_ = std::thread([this] { run(); });
}

DirectoryListing::~DirectoryListing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void DirectoryListing::set_enabled(bool enabled, int64_t now_ms, int64_t first_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled == enabled_) return;
    enabled_ = enabled;
    failures_ = 0;
    next_ms_ = now_ms + first_ms;
    wait_ms_ = -1;
    status_.state = enabled ? "waiting" : "off";
    status_.detail.clear();
}

void DirectoryListing::tick(int64_t now_ms, const std::function<std::string()>& build) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The thread knows how long to wait but not the caller's clock.
        if (wait_ms_ >= 0) {
            next_ms_ = now_ms + wait_ms_;
            wait_ms_ = -1;
        }
        if (!enabled_ || in_flight_ || now_ms < next_ms_) return;
    }
    // Built outside the lock: it reads the station, not the listing.
    std::string body = build();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || in_flight_) return;
    if (body.empty()) {
        next_ms_ = now_ms + kIntervalMs;
        return;
    }
    pending_ = std::move(body);
    in_flight_ = true;
    // Until the report has gone, the next one is not due.
    next_ms_ = INT64_MAX;
    wake_.notify_one();
}

DirectoryListing::Status DirectoryListing::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void DirectoryListing::set_sender(Send send) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_ = std::move(send);
}

void DirectoryListing::settle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !in_flight_; });
}

void DirectoryListing::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || in_flight_; });
        if (stopping_) return;
        const std::string body = pending_;
        const Send send = send_;
        lock.unlock();
        std::string error;
        const bool sent = send(body, error);
        const int64_t now = wall_ms();
        lock.lock();
        in_flight_ = false;
        if (enabled_) {
            if (sent) {
                failures_ = 0;
                status_ = {"listed", "", now};
            } else {
                // Longer and longer waits while the directory is away, so a
                // receiver whose owner forgot about the listing does not knock
                // every minute for a week.
                failures_++;
                status_.state = "failing";
                status_.detail = error;
                if (failures_ == 1) LOG_WARN("directory", "not listed on sdr-list.xyz: %s", error.c_str());
            }
            wait_ms_ = sent ? kIntervalMs : std::min(kLongestWaitMs, kIntervalMs << std::min(failures_ - 1, 4));
        }
        idle_.notify_all();
    }
}

}  // namespace fernsdr
