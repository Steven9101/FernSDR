// Listing the receiver on sdr-list.xyz, the directory of web receivers.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fernsdr {

/** A 4- or 6-character Maidenhead locator, such as JO62 or JO62qm. */
bool valid_grid_locator(const std::string& grid);

/**
 * A host name or IPv4 address a directory can link to, without a scheme or a
 * port. IPv6 literals are refused: the directory builds its link and its
 * reachability check as `http://host:port/`, which needs brackets around one.
 */
bool valid_public_host(const std::string& host);

/** What a report to sdr-list.xyz says about this receiver. */
struct DirectoryReport {
    // Stable across restarts, so the directory keeps one entry per receiver.
    std::string receiver_id;
    // New at every start.
    std::string instance_id;
    std::string host;
    int port = 0;
    std::string grid;
    std::string name;
    std::string antenna;
    int users = 0;
    int max_users = 0;
    // Each band's lowest and highest frequency, in hertz.
    std::vector<std::pair<double, double>> bands;
};

/**
 * The report as the directory's JSON. It names the software but not its
 * version: the exact version tells a stranger which published fixes a
 * receiver is missing, which is why no public page of the receiver says it.
 */
std::string directory_report_json(const DirectoryReport& report);

/**
 * Tells sdr-list.xyz about this receiver once a minute while listing is on.
 *
 * The directory lists a receiver for five minutes after its last report, so a
 * receiver that stops reporting, or is turned off, drops out by itself. The
 * report goes out with curl from a thread of its own: a request that hangs
 * for ten seconds must never hold up the network thread, which carries
 * everyone's audio. What it says is built on the network thread, which owns
 * the station details, and handed over whole.
 */
class DirectoryListing {
public:
    static constexpr const char* kUrl = "https://sdr-list.xyz/api/v1/register";
    static constexpr int64_t kIntervalMs = 60'000;
    static constexpr int64_t kLongestWaitMs = 15 * 60'000;
    // The bands start in the background; the first report waits for them.
    static constexpr int64_t kFirstReportMs = 10'000;

    struct Status {
        // "off", "waiting", "listed" or "failing".
        std::string state = "off";
        // Why the last report failed, for "failing".
        std::string detail;
        // Wall-clock milliseconds of the last report the directory took, 0 for none.
        int64_t listed_ms = 0;
    };

    // Sends one report and says why when it fails. Replaced in tests.
    using Send = std::function<bool(const std::string& body, std::string& error)>;

    // Reports with curl, which is looked for when the listing is first used.
    DirectoryListing();
    explicit DirectoryListing(Send send);
    ~DirectoryListing();
    DirectoryListing(const DirectoryListing&) = delete;
    DirectoryListing& operator=(const DirectoryListing&) = delete;

    /**
     * Turns reporting on or off. On, the first report goes out at the next
     * tick after `first_ms`; off, nothing more is sent and the directory drops
     * the receiver five minutes after the last report.
     */
    void set_enabled(bool enabled, int64_t now_ms, int64_t first_ms = 0);

    /**
     * For the network thread, several times a second: when a report is due,
     * `build` makes it and the listing's thread sends it. `build` returning an
     * empty string skips the round.
     */
    void tick(int64_t now_ms, const std::function<std::string()>& build);

    Status status() const;

    /** Replaces how reports are sent, so that no test reaches the internet. */
    void set_sender(Send send);

    /** Waits for a report in flight to finish; for tests. */
    void settle();

private:
    void run();

    Send send_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::thread thread_;
    bool stopping_ = false;
    bool enabled_ = false;
    bool in_flight_ = false;
    std::string pending_;
    int64_t next_ms_ = 0;
    // Set by the thread after a report: how long until the next one.
    int64_t wait_ms_ = -1;
    int failures_ = 0;
    Status status_;
};

}  // namespace fernsdr
