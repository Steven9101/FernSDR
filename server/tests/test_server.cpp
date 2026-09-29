// Socket-level tests for the server and session lifecycle.
//
// These exist because a bug that every unit test passed - a disconnect
// callback that never fired, leaving sessions holding references to freed
// connections - only showed up when a real client actually hung up.
#include "../src/codec/nac.h"
#include "../src/core/app.h"
#include "../src/core/protocol.h"
#include "../src/core/radio.h"
#include "../src/net/server.h"
#include "../src/util/sha1.h"
#include "../src/util/password.h"
#include "../src/update/release.h"
#include "../src/update/service.h"
#include "../src/util/ed25519.h"
#include "../src/version.h"
#include "test_util.h"

#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <functional>
#include <dirent.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// A blocking WebSocket client, enough to drive the server.
class TestClient {
public:
    explicit TestClient(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = inet_addr("127.0.0.1");
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;

        timeval timeout{2, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    ~TestClient() { close(); }

    bool connected() const { return connected_; }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    void send_raw(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    bool peer_closed() {
        char byte;
        return ::recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
    }

    bool read_until(const std::string& value) {
        while (buffer_.find(value) == std::string::npos) {
            if (!pump()) return false;
        }
        return true;
    }

    bool handshake(const std::string& path = "/ws") {
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        send_raw("GET " + path +
                 " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");

        while (buffer_.find("\r\n\r\n") == std::string::npos) {
            if (!pump()) return false;
        }
        const size_t end = buffer_.find("\r\n\r\n") + 4;
        const std::string head = buffer_.substr(0, end);
        buffer_.erase(0, end);
        if (head.find("101") == std::string::npos) return false;
        return head.find(fernsdr::ws::accept_key(key)) != std::string::npos;
    }

    void send_text(const std::string& text) {
        std::string frame;
        frame += static_cast<char>(0x81);
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        if (text.size() < 126) {
            frame += static_cast<char>(0x80 | text.size());
        } else {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(text.size() >> 8);
            frame += static_cast<char>(text.size() & 0xFF);
        }
        for (uint8_t m : mask) frame += static_cast<char>(m);
        for (size_t i = 0; i < text.size(); i++) frame += static_cast<char>(text[i] ^ mask[i & 3]);
        send_raw(frame);
    }

    // Collects frames until `deadline_ms` elapses.
    void collect(int milliseconds, std::vector<std::pair<uint8_t, std::string>>& out) {
        const int64_t deadline = fernsdr::monotonic_ms() + milliseconds;
        while (fernsdr::monotonic_ms() < deadline) {
            if (!pump()) break;
            drain_frames(out);
        }
        drain_frames(out);
    }

    std::string http_get(const std::string& path) {
        return http_request("GET", path);
    }

    std::string http_request(const std::string& method, const std::string& path,
                             const std::string& body = "", const std::string& headers = "",
                             const std::string& host = "localhost") {
        send_raw(method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" +
                 headers + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
        while (pump()) {}
        return buffer_;
    }

private:
    bool pump() {
        char chunk[8192];
        const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
        if (got <= 0) return false;
        buffer_.append(chunk, static_cast<size_t>(got));
        return true;
    }

    void drain_frames(std::vector<std::pair<uint8_t, std::string>>& out) {
        while (buffer_.size() >= 2) {
            const uint8_t byte0 = static_cast<uint8_t>(buffer_[0]);
            const uint8_t byte1 = static_cast<uint8_t>(buffer_[1]);
            size_t offset = 2;
            uint64_t length = byte1 & 0x7F;
            if (length == 126) {
                if (buffer_.size() < 4) return;
                length = (static_cast<uint8_t>(buffer_[2]) << 8) | static_cast<uint8_t>(buffer_[3]);
                offset = 4;
            } else if (length == 127) {
                if (buffer_.size() < 10) return;
                length = 0;
                for (int i = 0; i < 8; i++) length = (length << 8) | static_cast<uint8_t>(buffer_[2 + i]);
                offset = 10;
            }
            if (buffer_.size() < offset + length) return;
            out.emplace_back(byte0 & 0x0F, buffer_.substr(offset, static_cast<size_t>(length)));
            buffer_.erase(0, offset + static_cast<size_t>(length));
        }
    }

    int fd_ = -1;
    bool connected_ = false;
    std::string buffer_;
};

// Brings up a real server on an ephemeral port with a synthetic band.
struct Harness {
    fernsdr::Config config;
    fernsdr::Radio radio;
    std::unique_ptr<fernsdr::Application> application;
    std::unique_ptr<fernsdr::Server> server;
    std::thread thread;
    bool ok = false;

    // With `config_file`, the configuration is written there and loaded from
    // it, for what reads or rewrites the file on disk. `site_config` goes into
    // the [site] section, which only the first of its name is read from.
    Harness(int header_timeout_ms = 15000, int request_timeout_ms = 120000,
            const std::string& extra_config = "", const std::string& config_file = "",
            const std::string& site_config = "",
            const std::function<void(fernsdr::ServerConfig&)>& tweak = {}) {
        std::string error;
        const std::string text =
            "[site]\nname = Test Receiver\nmax_users = 3\n" + site_config +
            "[server]\nport = 0\n"
            "[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\nrealtime = true\n" + extra_config;
        if (config_file.empty()) {
            config.parse(text, error);
        } else if (!fernsdr::write_text_file(config_file, text, error) || !config.load(config_file, error)) {
            return;
        }
        if (!radio.configure(config, error)) return;

        fernsdr::ServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = 0;  // let the kernel choose
        server_config.max_connections = 8;
        server_config.header_timeout_ms = header_timeout_ms;
        server_config.request_timeout_ms = request_timeout_ms;
        if (tweak) tweak(server_config);

        application = std::make_unique<fernsdr::Application>(radio, config);
        // No test reaches sdr-list.xyz, whatever its configuration says, and
        // one that wants the reports puts its own sender in. They go out at
        // the first tick once listing is on, not ten seconds later.
        application->directory().set_sender([](const std::string&, std::string& error) {
            error = "tests do not report to sdr-list.xyz";
            return false;
        });
        application->set_listing_delay_ms(0);
        // What the application knows of the server, as main() hands it over;
        // only a tweak sets anything here that matters to it.
        if (tweak) application->set_server_config(server_config);
        server = std::make_unique<fernsdr::Server>(server_config, *application);
        application->set_server(server.get());
        if (!server->start(error)) return;
        radio.set_wake_callback([this] { server->wake(); });
        if (!radio.start(error)) return;

        thread = std::thread([this] { server->run(); });
        ok = true;
    }

    ~Harness() {
        if (server) server->stop();
        if (thread.joinable()) thread.join();
        radio.stop();
    }

    int port() const { return server ? server->bound_port() : 0; }
};

void wait_ms(int milliseconds) {
    struct timespec ts {milliseconds / 1000, (milliseconds % 1000) * 1000L * 1000L};
    nanosleep(&ts, nullptr);
}

bool contains(const std::vector<std::pair<uint8_t, std::string>>& frames, const std::string& needle) {
    for (const auto& frame : frames) {
        if (frame.first == 0x1 && frame.second.find(needle) != std::string::npos) return true;
    }
    return false;
}

int count_binary(const std::vector<std::pair<uint8_t, std::string>>& frames, uint8_t stream) {
    int total = 0;
    for (const auto& frame : frames) {
        if (frame.first == 0x2 && !frame.second.empty() &&
            static_cast<uint8_t>(frame.second[0]) == stream) {
            total++;
        }
    }
    return total;
}

}  // namespace

namespace {

fernsdr::Json json_body(const std::string& response) {
    fernsdr::Json body;
    const size_t split = response.find("\r\n\r\n");
    CHECK(split != std::string::npos);
    if (split != std::string::npos) CHECK(fernsdr::Json::parse(response.substr(split + 4), body));
    return body;
}

/** Signs in the way the panel does, then signs every request it sends. */
struct AdminClient {
    int port = 0;
    std::string host = "localhost";
    uint8_t key[32] = {};
    std::string cookie;
    std::string context;
    uint64_t counter = 0;

    bool sign_in(int server_port, const std::string& password) {
        using fernsdr::Json;
        port = server_port;
        const Json challenge = json_body(TestClient(port).http_request("POST", "/api/admin/challenge", "", "", host));
        uint8_t mac[32];
        fernsdr::pbkdf2_sha256(password, challenge["salt"].string(),
            static_cast<int>(challenge["iterations"].number()), key, sizeof(key));
        const std::string nonce = challenge["nonce"].string();
        fernsdr::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(), mac);
        Json login = Json::make_object();
        login.set("nonce", nonce);
        login.set("proof", fernsdr::to_hex(mac, sizeof(mac)));
        const std::string response =
            TestClient(port).http_request("POST", "/api/admin/login", login.serialize(), "", host);
        const size_t cookie_start = response.find("fernsdr_admin=");
        if (response.find("200 OK") == std::string::npos || cookie_start == std::string::npos) return false;
        cookie = response.substr(cookie_start, response.find(';', cookie_start) - cookie_start);
        context = json_body(response)["signing_context"].string();
        uint8_t derived[32];
        std::copy(std::begin(key), std::end(key), derived);
        fernsdr::AdminAuth::session_key(derived, context, key);
        return true;
    }

    std::string request(const std::string& method, const std::string& path, const std::string& body = "") {
        const std::string serial = std::to_string(++counter);
        const std::string signed_text = "fernsdr-admin-v3\n" + context + "\n" + serial + "\n" + method + "\n" + path + "\n" + body;
        uint8_t mac[32];
        fernsdr::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(signed_text.data()), signed_text.size(), mac);
        return TestClient(port).http_request(method, path, body,
            "Cookie: " + cookie + "\r\nX-FernSDR-Counter: " + serial + "\r\nX-FernSDR-Signature: " + fernsdr::to_hex(mac, sizeof(mac)) + "\r\n",
            host);
    }
};

}  // namespace

// The reports a test sees instead of sdr-list.xyz. Declared before the
// harness whose listing sends them, so that they outlive it.
struct ListingReports {
    std::mutex mutex;
    std::vector<std::string> bodies;

    fernsdr::DirectoryListing::Send sender() {
        return [this](const std::string& body, std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            bodies.push_back(body);
            return true;
        };
    }

    // The latest report once there are `count`, or null after three seconds.
    fernsdr::Json wait_for(size_t count) {
        for (int i = 0; i < 300; i++) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                fernsdr::Json report;
                if (bodies.size() >= count && fernsdr::Json::parse(bodies.back(), report)) return report;
            }
            wait_ms(10);
        }
        return fernsdr::Json();
    }
};

TEST_CASE(admin_listing_needs_a_grid_square_and_an_address) {
    using fernsdr::Json;
    const std::string password = "test admin directory listing";
    ListingReports reports;
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    harness.application->directory().set_sender(reports.sender());
    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    const auto save = [&](const std::string& station) {
        return admin.request("POST", "/api/admin/station", "{\"station\":" + station + "}");
    };
    // Refused with the reason, and nothing changes.
    std::string reply = save("{\"sdr_list\":true,\"public_host\":\"sdr.example.org\"}");
    CHECK(reply.find("400 Bad Request") != std::string::npos);
    CHECK(json_body(reply)["error"].string().find("grid square") != std::string::npos);
    reply = save("{\"sdr_list\":true,\"grid\":\"JO62qm\",\"public_host\":\"http://sdr.example.org\"}");
    CHECK(reply.find("400 Bad Request") != std::string::npos);
    CHECK(!json_body(admin.request("GET", "/api/admin/directory"))["enabled"].boolean());

    reply = save("{\"sdr_list\":true,\"grid\":\"JO62qm\",\"public_host\":\"sdr.example.org\"}");
    CHECK(reply.find("200 OK") != std::string::npos);
    CHECK(json_body(admin.request("GET", "/api/admin/directory"))["enabled"].boolean());
    const Json report = reports.wait_for(1);
    CHECK_EQ(report["receiver_id"].string().size(), 32u);
    CHECK_EQ_STR(report["hostname"].string(), "sdr.example.org");
    // No public port given: the one the receiver listens on.
    CHECK_EQ(report["port"].number(), static_cast<double>(harness.port()));
    CHECK_EQ_STR(report["grid_locator"].string(), "JO62qm");
    CHECK_EQ_STR(report["software"].string(), "FernSDR");
    CHECK(!report.has("version"));
    CHECK_EQ(report["receiver_count"].number(), 1);
    CHECK(save("{\"sdr_list\":false}").find("200 OK") != std::string::npos);
}

TEST_CASE(listing_keeps_its_receiver_id_across_a_restart) {
    // One entry per receiver: a restart that drew a new id would leave the
    // old entry in the directory for five minutes beside the new one.
    char made[] = "/tmp/fernsdr-listing-id-XXXXXX";
    CHECK(::mkdtemp(made) != nullptr);
    const std::string directory = made;
    const std::string listed = "sdr_list = yes\ngrid = JO62\npublic_host = sdr.example.org\npublic_port = 443\n";
    std::string first;
    for (int start = 0; start < 2; start++) {
        ListingReports reports;
        Harness harness(15000, 120000, "", directory + "/config.ini", listed);
        CHECK(harness.ok);
        if (!harness.ok) return;
        harness.application->directory().set_sender(reports.sender());
        // The harness's own sender refused the first report; switched off,
        // the next tick switches the listing on again and reports at once.
        harness.application->directory().set_enabled(false, 0);
        const fernsdr::Json report = reports.wait_for(1);
        CHECK_EQ(report["port"].number(), 443);
        if (start == 0) first = report["receiver_id"].string();
        else CHECK_EQ_STR(report["receiver_id"].string(), first);
    }
    CHECK_EQ(first.size(), 32u);
    ::unlink((directory + "/config.ini").c_str());
    ::unlink((directory + "/fernsdr-settings.json").c_str());
    ::rmdir(made);
}

TEST_CASE(admin_http_rejects_numbers_before_integer_conversion) {
    using fernsdr::Json;
    const std::string password = "test admin numeric boundaries";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    const auto request = [&](const std::string& method, const std::string& path, const std::string& body = "") {
        return admin.request(method, path, body);
    };
    for (const char* value : {"1e300", "-1e300", "-1", "0", "1.5", "9007199254740992", "null", "true", "\"1\""}) {
        CHECK(request("POST", "/api/admin/disconnect", std::string("{\"id\":") + value + "}").find("400 Bad Request") != std::string::npos);
    }
    CHECK(request("POST", "/api/admin/disconnect", "{\"id\":1}").find("200 OK") != std::string::npos);
    for (const char* value : {"-1e300", "-1", "1.5", "null", "true", "\"5\""}) {
        CHECK(request("POST", "/api/admin/mute", std::string("{\"address\":\"127.0.0.2\",\"minutes\":") + value + "}").find("400 Bad Request") != std::string::npos);
    }
    const Json muted = json_body(request("POST", "/api/admin/mute", "{\"address\":\"127.0.0.2\",\"minutes\":1e300}"));
    CHECK(muted["muted"].is_array());
    const double now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK_NEAR(muted["muted"][size_t{0}]["until"].number(), now + 7 * 24 * 60 * 60000.0, 2000);
    const Json permanent = json_body(request("POST", "/api/admin/mute", "{\"address\":\"127.0.0.2\",\"minutes\":0}"));
    CHECK_EQ(permanent["muted"][size_t{0}]["until"].number(), 0);

    wait_ms(150);
    for (const char* query : {"hz=nan", "hz=1e300", "hz=-1e300", "hz=7100000junk", "hz=null", "hz=0",
                             "hz=7100000&width=1e300", "hz=7100000&width=nan", "hz=7100000&width=-1", "hz=7100000&width=0"}) {
        CHECK(request("GET", std::string("/api/admin/level?band=demo&") + query).find("400 Bad Request") != std::string::npos);
    }
    CHECK(request("GET", "/api/admin/level?band=demo&hz=7100000&width=1000").find("200 OK") != std::string::npos);
}

TEST_CASE(admin_config_editor_keeps_the_password_hash_on_the_server) {
    using fernsdr::Json;
    const std::string password = "test admin config editor";
    const std::string hash = fernsdr::hash_password(password, 1000);
    char directory[] = "/tmp/fernsdr-editor-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    {
        Harness harness(15000, 120000, "[admin]\npassword_hash = " + hash + "\n", path);
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), password));

        const Json config = json_body(admin.request("GET", "/api/admin/config"));
        const std::string shown = config["text"].string();
        CHECK(!shown.empty());
        CHECK(shown.find(hash) == std::string::npos);
        CHECK(shown.find("password_hash = (kept on the machine)") != std::string::npos);

        // Saved back with another change, the hash is what the file held.
        std::string edited = shown;
        edited.replace(edited.find("Test Receiver"), 13, "Renamed Receiver");
        Json save = Json::make_object();
        save.set("text", edited);
        CHECK(admin.request("POST", "/api/admin/config", save.serialize()).find("200 OK") != std::string::npos);
        std::string written;
        CHECK(fernsdr::read_text_file(path, written));
        CHECK(written.find("password_hash = " + hash) != std::string::npos);
        CHECK(written.find("Renamed Receiver") != std::string::npos);

        // Replacing it, removing it or adding a second [admin] is refused, and
        // the file is left alone.
        std::string other = edited;
        other.replace(other.find("(kept on the machine)"), 21, fernsdr::hash_password("another one", 1000));
        std::string removed = edited;
        removed.erase(removed.find("password_hash"), removed.find('\n', removed.find("password_hash")) -
                                                           removed.find("password_hash") + 1);
        const std::string doubled = edited + "\n[admin]\npassword_hash = " + hash + "\n";
        for (const std::string& attempt : {other, removed, doubled}) {
            Json refused = Json::make_object();
            refused.set("text", attempt);
            const std::string response = admin.request("POST", "/api/admin/config", refused.serialize());
            CHECK(response.find("400 Bad Request") != std::string::npos);
            CHECK(response.find("Station page") != std::string::npos);
        }
        std::string after;
        CHECK(fernsdr::read_text_file(path, after));
        CHECK(after == written);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

TEST_CASE(admin_config_editor_cannot_point_the_receiver_at_other_files) {
    using fernsdr::Json;
    const std::string password = "test admin config file settings";
    char directory[] = "/tmp/fernsdr-kept-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    {
        Harness harness(15000, 120000, "[admin]\npassword_hash = " + fernsdr::hash_password(password, 1000) + "\n",
                        path);
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), password));
        std::string before;
        CHECK(fernsdr::read_text_file(path, before));
        const std::string shown = json_body(admin.request("GET", "/api/admin/config"))["text"].string();
        const auto edited = [&](const std::string& from, const std::string& to) {
            std::string text = shown;
            text.replace(text.find(from), from.size(), to);
            return text;
        };
        // Serving the directory the configuration is in would publish the
        // password hash at the next restart.
        const std::pair<std::string, std::string> attempts[] = {
            {edited("port = 0\n", "port = 0\nuploads = .\n"), "[server] section"},
            {edited("port = 0\n", "port = 0\nroot = /\n"), "[server] section"},
            {edited("port = 0\n", "port = 0\ntrusted_proxies = 0.0.0.0/0\n"), "[server] section"},
            {edited("realtime = true\n", "realtime = true\nhistory_path = " + std::string(directory) + "/x.wfa\n"),
             "[band:demo] history_path"},
            {edited("max_users = 3\n", "max_users = 3\ntheme_file = " + path + "\n"), "[site] theme_file"},
        };
        for (const auto& [text, reason] : attempts) {
            Json save = Json::make_object();
            save.set("text", text);
            const std::string response = admin.request("POST", "/api/admin/config", save.serialize());
            CHECK(response.find("400 Bad Request") != std::string::npos);
            CHECK(response.find(reason) != std::string::npos);
        }
        std::string after;
        CHECK(fernsdr::read_text_file(path, after));
        CHECK(after == before);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

namespace {

void remove_directory(const std::string& path) {
    if (DIR* directory = ::opendir(path.c_str())) {
        while (const dirent* entry = ::readdir(directory)) {
            if (entry->d_name[0] != '.') ::unlink((path + "/" + entry->d_name).c_str());
        }
        ::closedir(directory);
    }
    ::rmdir(path.c_str());
}

}  // namespace

TEST_CASE(admin_a_backup_moves_a_receiver_to_another_machine) {
    using fernsdr::Json;
    char old_directory[] = "/tmp/fernsdr-backup-old-XXXXXX";
    char new_directory[] = "/tmp/fernsdr-backup-new-XXXXXX";
    CHECK(::mkdtemp(old_directory) != nullptr);
    CHECK(::mkdtemp(new_directory) != nullptr);
    const std::string old_path = std::string(old_directory) + "/fernsdr.conf";
    const std::string new_path = std::string(new_directory) + "/fernsdr.conf";
    const std::string old_hash = fernsdr::hash_password("the old machine", 1000);
    const std::string new_hash = fernsdr::hash_password("the new machine", 1000);
    std::string error;
    CHECK(fernsdr::write_text_file(std::string(old_directory) + "/fernsdr-theme.json", "{\"meter\":\"needle\"}", error));
    // A receiver listed on sdr-list.xyz, with a listener muted in the chat:
    // neither belongs in a file that moves.
    CHECK(fernsdr::write_text_file(std::string(old_directory) + "/fernsdr-settings.json",
                                   "{\"directory_id\":\"0123456789abcdef0123456789abcdef\","
                                   "\"muted\":[{\"address\":\"203.0.113.9\",\"until\":0}]}",
                                   error));

    Json backup;
    std::string picture;
    {
        Harness harness(15000, 120000,
                        "[band:forty]\nsource = test\nsample_rate = 192k\ncenter = 7.05M\nhistory_path = " +
                            std::string(old_directory) + "/forty.wfa\n[modules]\ndirectory = " + old_directory +
                            "/mods\n[admin]\npassword_hash = " + old_hash + "\n",
                        old_path, "", [&](fernsdr::ServerConfig& config) {
                            config.uploads_root = old_directory;
                            config.document_root = old_directory;
                        });
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), "the old machine"));
        std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\rIHDR", 8) +
                          std::string("\0\0\0\x01\0\0\0\x01\x08\x02\0\0\0", 13) + std::string(4, '\0');
        const std::string uploaded = admin.request("POST", "/api/admin/upload", png);
        const size_t at = uploaded.find("/uploads/");
        CHECK(at != std::string::npos);
        if (at == std::string::npos) return;
        picture = uploaded.substr(at + 9, uploaded.find('"', at) - at - 9);

        backup = json_body(admin.request("GET", "/api/admin/backup"));
        CHECK_EQ(backup["fernsdr_backup"].number(), 1);
        CHECK_EQ_STR(backup["station"].string(), "Test Receiver");
        const std::string text = backup["files"]["fernsdr.conf"].string();
        // The machine's own sections stay on it, the password's hash with them.
        CHECK(text.find("[band:forty]") != std::string::npos);
        CHECK(text.find(old_hash) == std::string::npos);
        CHECK(text.find("[admin]") == std::string::npos);
        CHECK(text.find("[server]") == std::string::npos);
        CHECK(text.find("[modules]") == std::string::npos);
        CHECK_EQ_STR(backup["files"]["fernsdr-theme.json"].string(), "{\"meter\":\"needle\"}");
        const std::string settings = backup["files"]["fernsdr-settings.json"].string();
        CHECK(!settings.empty());
        CHECK(settings.find("0123456789abcdef0123456789abcdef") == std::string::npos);
        CHECK(settings.find("203.0.113.9") == std::string::npos);
        CHECK_EQ(backup["pictures"].size(), 1u);
        CHECK_EQ_STR(backup["pictures"][size_t{0}]["name"].string(), picture);
    }

    CHECK(fernsdr::write_text_file(std::string(new_directory) + "/fernsdr-settings.json",
                                   "{\"directory_id\":\"fedcba9876543210fedcba9876543210\"}", error));
    {
        Harness harness(15000, 120000,
                        "[modules]\ndirectory = " + std::string(new_directory) + "/mods\n[admin]\npassword_hash = " +
                            new_hash + "\n",
                        new_path, "", [&](fernsdr::ServerConfig& config) {
                            config.uploads_root = new_directory;
                            config.document_root = new_directory;
                        });
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), "the new machine"));
        std::string before;
        CHECK(fernsdr::read_text_file(new_path, before));

        // Refused, and nothing written: what is not a backup, a band that
        // reads a file or takes samples from the network, as the
        // configuration editor refuses those.
        const auto with_band = [&](const std::string& band) {
            Json changed = backup;
            Json files = changed["files"];
            files.set("fernsdr.conf", files["fernsdr.conf"].string() + band);
            changed.set("files", files);
            return changed.serialize();
        };
        const std::pair<std::string, std::string> refusals[] = {
            {"{\"files\":{}}", "not a FernSDR backup"},
            {with_band("[band:file]\nsource = file\npath = /etc/passwd\nsample_rate = 192k\ncenter = 7.1M\n"),
             "names a file"},
            {with_band("[band:net]\nsource = udp\nport = 5555\nsample_rate = 192k\ncenter = 7.1M\n"),
             "takes its samples over the network"},
            {[&] {
                 Json changed = backup;
                 Json files = changed["files"];
                 files.set("fernsdr-theme.json", "{\"meter\":\"dial\"}");
                 changed.set("files", files);
                 return changed.serialize();
             }(),
             "look cannot be used here"},
        };
        for (const auto& [body, reason] : refusals) {
            const std::string response = admin.request("POST", "/api/admin/restore", body);
            CHECK(response.find("400 Bad Request") != std::string::npos);
            CHECK(response.find(reason) != std::string::npos);
        }
        std::string unchanged;
        CHECK(fernsdr::read_text_file(new_path, unchanged));
        CHECK(unchanged == before);

        // A band on a module this machine lacks is taken, and the module
        // named to install.
        Json moved = json_body(admin.request("POST", "/api/admin/restore", [&] {
            Json changed;
            Json::parse(with_band("[band:rtl]\nsource = module\nmodule = rtlsdr\nsample_rate = 2.4M\ncenter = 14.1M\n"),
                        changed);
            Json modules = Json::make_array();
            Json rtlsdr = Json::make_object();
            rtlsdr.set("id", "rtlsdr");
            rtlsdr.set("origin", "catalog");
            modules.push_back(rtlsdr);
            changed.set("modules", modules);
            return changed.serialize();
        }()));
        CHECK(moved["ok"].boolean(false));
        CHECK_EQ(moved["modules"].size(), 1u);
        CHECK_EQ_STR(moved["modules"][size_t{0}]["id"].string(), "rtlsdr");

        std::string after;
        CHECK(fernsdr::read_text_file(new_path, after));
        CHECK(after.find("[band:forty]") != std::string::npos);
        CHECK(after.find("[band:rtl]") != std::string::npos);
        CHECK(after.find("history_path") == std::string::npos);
        CHECK(after.find(new_hash) != std::string::npos);
        CHECK(after.find(std::string(new_directory) + "/mods") != std::string::npos);
        CHECK(after.find(old_directory) == std::string::npos);
        fernsdr::Config reread;
        CHECK(reread.parse(after, error));
        std::string theme, settings, stored;
        CHECK(fernsdr::read_text_file(std::string(new_directory) + "/fernsdr-theme.json", theme));
        CHECK_EQ_STR(theme, "{\"meter\":\"needle\"}");
        // The backup's settings, with this machine's own listing id kept.
        CHECK(fernsdr::read_text_file(std::string(new_directory) + "/fernsdr-settings.json", settings));
        Json written;
        CHECK(Json::parse(settings, written));
        CHECK_EQ_STR(written["directory_id"].string(), "fedcba9876543210fedcba9876543210");
        CHECK(settings.find("203.0.113.9") == std::string::npos);
        CHECK_EQ(moved["pictures_not_restored"].number(), 0);
        for (const char* left : {"/fernsdr.conf.restore", "/fernsdr-settings.json.restore", "/fernsdr-theme.json.restore"}) {
            CHECK(::access((std::string(new_directory) + left).c_str(), F_OK) != 0);
        }
        CHECK(fernsdr::read_text_file(std::string(new_directory) + "/" + picture, stored));

        // Until the restart, changes are refused rather than written over it.
        CHECK(json_body(admin.request("GET", "/api/admin/state"))["restored"].boolean(false));
        Json save = Json::make_object();
        save.set("text", after);
        const std::string refused = admin.request("POST", "/api/admin/config", save.serialize());
        CHECK(refused.find("409") != std::string::npos);
        CHECK(refused.find("restart FernSDR") != std::string::npos);
    }
    remove_directory(old_directory);
    remove_directory(new_directory);
}

TEST_CASE(radio_names_the_files_it_keeps_for_itself) {
    char directory[] = "/tmp/fernsdr-own-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    std::string error;
    CHECK(fernsdr::write_text_file(path,
        "[modules]\ndirectory = mods\n"
        "[band:a]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\nhistory_path = /srv/a.wfa\n"
        "[band:b]\nsource = test\nsample_rate = 192k\ncenter = 14.2M\n", error));
    {
        fernsdr::Config config;
        CHECK(config.load(path, error));
        fernsdr::Radio radio;
        CHECK(radio.configure(config, error));
        const auto files = radio.own_files();
        const auto named = [&](const std::string& file) {
            for (const auto& entry : files) {
                if (entry.second == file) return true;
            }
            return false;
        };
        CHECK(named(path));
        CHECK(named(std::string(directory) + "/fernsdr-settings.json"));
        CHECK(named(std::string(directory) + "/mods"));
        CHECK(named("/srv/a.wfa"));
        // Off, and so still at its default, where the panel would turn it on.
        CHECK(named("fernsdr-history-b.wfa"));
        CHECK_EQ(files.size(), 5u);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

TEST_CASE(listener_connections_get_linear_retransmission_timeouts) {
    // A thin stream's retries after a dropped second must not back off
    // exponentially, or the audio stays silent long after the link is back.
    struct Handler : fernsdr::ServerHandler {
        std::atomic<int> linear{-2}, low_water{-2};
        bool on_connect(fernsdr::Connection& connection) override {
            linear = connection.tcp_option(TCP_THIN_LINEAR_TIMEOUTS);
            low_water = connection.tcp_option(TCP_NOTSENT_LOWAT);
            return true;
        }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        bool on_http(fernsdr::Connection&, const fernsdr::HttpRequest&, std::string&) override { return false; }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    {
        TestClient client(server.bound_port());
        CHECK(client.handshake());
    }
    server.stop();
    thread.join();
    CHECK_EQ(handler.linear.load(), 1);
    CHECK_EQ(handler.low_water.load(), 4096);
}

TEST_CASE(server_serves_only_upload_names_from_the_uploads_directory) {
    struct Handler : fernsdr::ServerHandler {
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        bool on_http(fernsdr::Connection&, const fernsdr::HttpRequest&, std::string&) override { return false; }
    } handler;
    char directory[] = "/tmp/fernsdr-uploads-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string picture = std::string(directory) + "/0123456789abcdef.png";
    const std::string other = std::string(directory) + "/fernsdr.conf";
    std::string error;
    CHECK(fernsdr::write_text_file(picture, "PICTURE", error));
    CHECK(fernsdr::write_text_file(other, "password_hash = SECRET", error));
    {
        fernsdr::ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.port = 0;
        config.uploads_root = directory;
        fernsdr::Server server(config, handler);
        CHECK(server.start(error));
        std::thread thread([&] { server.run(); });
        const std::string served = TestClient(server.bound_port()).http_get("/uploads/0123456789abcdef.png");
        CHECK(served.find("200 OK") != std::string::npos);
        CHECK(served.find("PICTURE") != std::string::npos);
        // Whatever else the directory holds, for instance because the
        // configuration pointed it at the wrong place.
        for (const char* target : {"/uploads/fernsdr.conf", "/uploads/%66ernsdr.conf", "/uploads/",
                                   "/uploads/0123456789abcdef.png/../fernsdr.conf"}) {
            const std::string refused = TestClient(server.bound_port()).http_get(target);
            CHECK(refused.find("404 Not Found") != std::string::npos);
            CHECK(refused.find("SECRET") == std::string::npos);
        }
        server.stop();
        thread.join();
    }
    ::unlink(picture.c_str());
    ::unlink(other.c_str());
    ::rmdir(directory);
}

TEST_CASE(admin_turning_the_chat_off_reaches_listeners_at_once) {
    using fernsdr::Json;
    const std::string password = "test admin chat switch";
    char directory[] = "/tmp/fernsdr-chat-switch-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    {
        Harness harness(15000, 120000, "[admin]\npassword_hash = " + fernsdr::hash_password(password, 1000) + "\n",
                        path);
        CHECK(harness.ok);
        if (!harness.ok) return;
        TestClient listener(harness.port());
        CHECK(listener.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        listener.collect(200, frames);

        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), password));
        std::string text = json_body(admin.request("GET", "/api/admin/config"))["text"].string();
        text.replace(text.find("max_users = 3\n"), 14, "max_users = 3\nchat = no\n");
        Json save = Json::make_object();
        save.set("text", text);
        CHECK(admin.request("POST", "/api/admin/config", save.serialize()).find("200 OK") != std::string::npos);

        frames.clear();
        listener.collect(300, frames);
        CHECK(contains(frames, "\"type\":\"station\""));
        CHECK(contains(frames, "\"chat\":false"));
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

TEST_CASE(admin_over_plain_http_is_for_this_machine_only) {
    const std::string password = "test admin transport";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    // A browser on this machine: the loopback peer with nothing forwarded.
    CHECK(TestClient(harness.port()).http_request("POST", "/api/admin/challenge").find("200 OK") != std::string::npos);
    // The same socket peer, but the request says it came from elsewhere, as
    // it would through a proxy nobody listed or a raw tunnel with a liar on
    // the far end: over plain HTTP that is refused, whatever it claims.
    for (const char* header : {"X-Forwarded-For: 198.51.100.7\r\n", "X-Forwarded-Proto: https\r\n",
                               "Forwarded: for=198.51.100.7;proto=https\r\n"}) {
        const std::string response = TestClient(harness.port()).http_request("POST", "/api/admin/challenge", "", header);
        CHECK(response.find("403 Forbidden") != std::string::npos);
        CHECK(response.find("ssh -L") != std::string::npos);
    }
}

TEST_CASE(admin_over_plain_http_from_anywhere_is_an_opt_in_the_panel_is_told_about) {
    const std::string password = "test admin anywhere";
    const std::string hash = fernsdr::hash_password(password, 1000);
    const std::string forwarded = "X-Forwarded-For: 198.51.100.7\r\n";
    {
        Harness harness(15000, 120000, "[admin]\npassword_hash=" + hash + "\n");
        CHECK(harness.ok);
        if (!harness.ok) return;
        CHECK(TestClient(harness.port()).http_request("GET", "/api/admin/session", "", forwarded).find("403 Forbidden") !=
              std::string::npos);
        const std::string local = TestClient(harness.port()).http_get("/api/admin/session");
        CHECK(json_body(local)["exposed"].boolean(true) == false);
    }
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + hash + "\nplain_http_anywhere = yes\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    const std::string response = TestClient(harness.port()).http_request("GET", "/api/admin/session", "", forwarded);
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(json_body(response)["exposed"].boolean(false));
    // From this machine it is the usual, safe case, and the panel says nothing.
    CHECK(json_body(TestClient(harness.port()).http_get("/api/admin/session"))["exposed"].boolean(true) == false);
}

// What the server tells the handler about each request from its socket peer,
// which the admin rule relies on: whether the peer is a listed proxy, and
// HTTPS only on such a proxy's word. Both for an ordinary request and for the
// head of an upload, which the handler sees before its body arrives.
TEST_CASE(server_marks_requests_from_a_listed_proxy) {
    struct Recorder : fernsdr::ServerHandler {
        std::mutex mutex;
        int requests = 0, uploads = 0;
        bool secure = false, via_proxy = false, upload_secure = false, upload_via_proxy = false;
        bool on_connect(fernsdr::Connection&) override { return false; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        bool may_upload(fernsdr::Connection&, const fernsdr::HttpRequest& head) override {
            std::lock_guard<std::mutex> lock(mutex);
            uploads++;
            upload_secure = head.secure;
            upload_via_proxy = head.via_trusted_proxy;
            return false;
        }
        bool on_http(fernsdr::Connection&, const fernsdr::HttpRequest& request, std::string& response) override {
            std::lock_guard<std::mutex> lock(mutex);
            requests++;
            secure = request.secure;
            via_proxy = request.via_trusted_proxy;
            response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            return true;
        }
    };
    for (const bool listed : {true, false}) {
        Recorder recorder;
        fernsdr::ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.port = 0;
        if (listed) config.trusted_proxies = {"loopback"};
        fernsdr::Server server(config, recorder);
        std::string error;
        CHECK(server.start(error));
        std::thread thread([&server] { server.run(); });
        TestClient(server.bound_port()).http_request("GET", "/api/anything", "", "X-Forwarded-Proto: https\r\n");
        TestClient(server.bound_port())
            .http_request("POST", "/api/admin/upload", std::string(100 * 1024, 'x'), "X-Forwarded-Proto: https\r\n");
        server.stop();
        thread.join();
        std::lock_guard<std::mutex> lock(recorder.mutex);
        CHECK_EQ(recorder.requests, 1);
        CHECK_EQ(recorder.uploads, 1);
        CHECK_EQ(recorder.via_proxy, listed);
        CHECK_EQ(recorder.secure, listed);
        CHECK_EQ(recorder.upload_via_proxy, listed);
        CHECK_EQ(recorder.upload_secure, listed);
    }
}

// The Updates page through the server: a development build says why it
// cannot update itself; one that can finds a release and asks for it.
TEST_CASE(admin_updates_page_checks_and_asks_through_the_api) {
    char name[] = "/tmp/fernsdr-updates-api-XXXXXX";
    CHECK(::mkdtemp(name) != nullptr);
    const std::string root = name;
    ::mkdir((root + "/update").c_str(), 0755);
    const std::string password = "test admin updates page";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n",
                    root + "/fernsdr.conf");
    CHECK(harness.ok);
    if (!harness.ok) return;
    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));

    // This test program is no release, and carries no release key.
    const fernsdr::Json dev = json_body(admin.request("GET", "/api/admin/update"));
    CHECK_EQ_STR(dev["running"].string(), fernsdr::kVersion);
    CHECK(!dev["available"].boolean(true));
    CHECK(!dev["unavailable"].string().empty());
    const std::string refused = admin.request("POST", "/api/admin/update/check", "{}");
    CHECK(refused.find("409") != std::string::npos);

    // One that can, with a release server played by the test.
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(i * 3);
    fernsdr::ReleaseKey key;
    fernsdr::ed25519_public_key(seed, key.data());
    fernsdr::ReleaseManifest manifest;
    manifest.version = "99.0.0";
    manifest.date = "2026-10-01";
    manifest.channel = "stable";
    manifest.notes = "<b>not markup</b>";
    manifest.assets.push_back({"linux-x86_64", "fernsdr-99.0.0-linux-x86_64.tar", 10, std::string(64, 'b')});
    std::string text, error;
    CHECK(fernsdr::format_release_manifest(manifest, text, error));
    const std::string message = fernsdr::release_signed_message(text);
    uint8_t signature[64];
    fernsdr::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature);
    const std::string sig(reinterpret_cast<const char*>(signature), 64);
    fernsdr::UpdateServiceOptions options;
    options.state = root;
    options.update = root + "/update";
    options.install = "/opt/fernsdr";
    options.executable = "/opt/fernsdr/releases/" + std::string(fernsdr::kVersion) + "/fernsdr";
    options.running_version = fernsdr::kVersion;
    options.platform = "linux-x86_64";
    options.keys = {key};
    options.base_url = "https://releases.test/";
    options.fetch = [text, sig](const std::string& url, size_t, std::string& body, std::string&) {
        body = url.find(".sig") != std::string::npos ? sig : text;
        return true;
    };
    harness.application->set_update_service(std::make_unique<fernsdr::UpdateService>(options));

    CHECK(admin.request("POST", "/api/admin/update/check", "{}").find("202") != std::string::npos);
    fernsdr::Json view;
    for (int i = 0; i < 50; i++) {
        view = json_body(admin.request("GET", "/api/admin/update"));
        if (view["check"]["state"].string() != "checking") break;
        wait_ms(20);
    }
    CHECK_EQ_STR(view["check"]["version"].string(), "99.0.0");
    CHECK_EQ_STR(view["check"]["notes"].string(), "<b>not markup</b>");
    CHECK(view["check"]["newer"].boolean());
    CHECK(admin.request("POST", "/api/admin/update/start", "{}").find("400") != std::string::npos);
    CHECK(admin.request("POST", "/api/admin/update/start", "{\"version\":\"99.0.0\"}").find("202") !=
          std::string::npos);
    CHECK(::access((root + "/update-request").c_str(), F_OK) == 0);
    // Not without a session's signature.
    CHECK(TestClient(harness.port()).http_request("POST", "/api/admin/update/check", "{}").find("401") !=
          std::string::npos);

    for (const char* file : {"/fernsdr.conf", "/update-request", "/update-bands"}) ::unlink((root + file).c_str());
    ::rmdir((root + "/update").c_str());
    ::rmdir(root.c_str());
}

// A receiver that is the new version of an update on trial says it works
// once it has served with every band that ran before; not while one is
// missing, and not for a trial of another version.
TEST_CASE(receiver_on_trial_confirms_the_update_once_its_bands_run) {
    char name[] = "/tmp/fernsdr-trial-XXXXXX";
    CHECK(::mkdtemp(name) != nullptr);
    const std::string root = name, state = root + "/state", update = root + "/update";
    ::mkdir(state.c_str(), 0700);
    ::mkdir(update.c_str(), 0755);
    const auto write = [](const std::string& path, const std::string& text) {
        std::string error;
        CHECK(fernsdr::write_text_file(path, text, error));
    };
    const auto exists = [](const std::string& path) { return ::access(path.c_str(), F_OK) == 0; };
    ::setenv("FERNSDR_UPDATE_DIR", update.c_str(), 1);
    for (const std::string& bands : {std::string("demo\n"), std::string("demo\nmissing\n")}) {
        write(update + "/trial", std::string("old 0.0.1\nnew ") + fernsdr::kVersion + "\nstarted 1\npresent\n");
        write(state + "/update-bands", bands);
        ::unlink((state + "/update-commit").c_str());
        {
            Harness harness(15000, 120000, "", state + "/fernsdr.conf");
            CHECK(harness.ok);
            if (!harness.ok) break;
            harness.application->set_update_settle_ms(0);
            for (int i = 0; i < 40 && !exists(state + "/update-commit"); i++) wait_ms(100);
        }
        CHECK_EQ(exists(state + "/update-commit"), bands == "demo\n");
    }
    // A trial of another version is not this one's to confirm.
    write(update + "/trial", "old 0.0.1\nnew 99.0.0\nstarted 1\npresent\n");
    write(state + "/update-bands", "demo\n");
    ::unlink((state + "/update-commit").c_str());
    {
        Harness harness(15000, 120000, "", state + "/fernsdr.conf");
        harness.application->set_update_settle_ms(0);
        wait_ms(2500);
    }
    CHECK(!exists(state + "/update-commit"));
    ::unsetenv("FERNSDR_UPDATE_DIR");
    for (const char* file : {"/state/fernsdr.conf", "/state/update-bands", "/state/update-commit", "/update/trial"}) {
        ::unlink((root + file).c_str());
    }
    ::rmdir(state.c_str());
    ::rmdir(update.c_str());
    ::rmdir(root.c_str());
}

// Debian and Raspberry Pi OS resolve the machine's own name to 127.0.1.1, so
// a browser on the Pi that opens http://raspberrypi:8073 arrives from
// loopback asking for a home name: in with home_network = yes, and told to
// use localhost without it.
TEST_CASE(admin_home_network_lets_this_machine_in_under_its_own_name) {
    const std::string hash = fernsdr::hash_password("test admin home network", 1000);
    for (const bool home : {true, false}) {
        Harness harness(15000, 120000,
                        "[admin]\npassword_hash=" + hash + "\nhome_network=" + (home ? "yes" : "no") + "\n");
        CHECK(harness.ok);
        if (!harness.ok) return;
        const std::string response =
            TestClient(harness.port()).http_request("POST", "/api/admin/challenge", "", "", "raspberrypi:8073");
        if (home) {
            CHECK(response.find("200 OK") != std::string::npos);
        } else {
            CHECK(response.find("403 Forbidden") != std::string::npos);
            CHECK(response.find("Open it as localhost on this machine or through ssh -L.") != std::string::npos);
        }
        // A public name is refused either way, as it would come through a raw tunnel.
        CHECK(TestClient(harness.port())
                  .http_request("POST", "/api/admin/challenge", "", "", "sdr.example.org")
                  .find("403 Forbidden") != std::string::npos);
    }
}

TEST_CASE(admin_spectrum_is_for_the_panel_only_and_bounded) {
    using fernsdr::Json;
    const std::string password = "test admin spectrum view";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    // Nobody without a session gets it: it is the band as the operator sees it.
    CHECK(TestClient(harness.port()).http_request("GET", "/api/admin/spectrum?band=demo").find("200 OK") ==
          std::string::npos);

    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    CHECK(admin.request("GET", "/api/admin/spectrum?band=nothing").find("404") != std::string::npos);
    wait_ms(300);
    // However many cells are asked for, the answer stays within 32 and 2048.
    for (const auto& [asked, most] : std::vector<std::pair<std::string, size_t>>{
             {"1e300", 2048}, {"-5", 32}, {"nan", 512}, {"128", 128}}) {
        const Json view = json_body(admin.request("GET", "/api/admin/spectrum?band=demo&bins=" + asked));
        CHECK(view["levels"].is_array());
        CHECK(view["levels"].size() > 0 && view["levels"].size() <= most);
        CHECK(view["high"].number() > view["low"].number());
        CHECK(!view.has("recent"));
    }
    const Json view = json_body(admin.request("GET", "/api/admin/spectrum?band=demo&recent=1"));
    const Json& recent = view["recent"];
    CHECK(recent.is_object());
    CHECK_EQ(recent["width"].number(), fernsdr::Band::kRecentBins);
    CHECK(recent["count"].number() >= 1);
    CHECK(recent["age_ms"].number() >= 0);
    CHECK_EQ(recent["floor_db"].number(), -140);
    CHECK_EQ(recent["ceiling_db"].number(), -20);
    // Base64 of count lines of width bytes.
    const size_t bytes = static_cast<size_t>(recent["count"].number() * recent["width"].number());
    CHECK_EQ(recent["lines"].string().size(), (bytes + 2) / 3 * 4);
}

TEST_CASE(admin_carrier_reading_finds_a_known_carrier) {
    using fernsdr::Json;
    const std::string password = "test admin carrier reading";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    // The test source's unmodulated carrier sits 800 Hz below the centre.
    const std::string near = "/api/admin/carrier?band=demo&hz=7099300";
    CHECK(TestClient(harness.port()).http_request("GET", near).find("200 OK") == std::string::npos);

    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    for (const char* query : {"hz=nan", "hz=1", "hz=7099300&span=10", "hz=7099300&span=1e9", "span=2000"}) {
        CHECK(admin.request("GET", std::string("/api/admin/carrier?band=demo&") + query).find("400 Bad Request") !=
              std::string::npos);
    }
    CHECK(admin.request("GET", "/api/admin/carrier?band=none&hz=7099300").find("404") != std::string::npos);
    wait_ms(400);
    const Json reading = json_body(admin.request("GET", near + "&span=2000"));
    CHECK(reading["found"].boolean());
    CHECK_NEAR(reading["hz"].number(), 7099200.0, 5.0);
    CHECK_EQ(reading["ppm"].number(), 0);
    CHECK_EQ(reading["frequency_offset"].number(), 0);
}

TEST_CASE(completed_upload_releases_receive_memory_and_preserves_pipelined_requests) {
    struct Handler : fernsdr::ServerHandler {
        std::atomic<size_t> largest_capacity{0};
        std::atomic<size_t> uploads{0};
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        // Signed in, as far as this test is concerned: it is about what the
        // buffers do with an upload that is allowed.
        bool may_upload(fernsdr::Connection&, const fernsdr::HttpRequest&) override { return true; }
        bool on_http(fernsdr::Connection& connection, const fernsdr::HttpRequest& request,
                     std::string& response) override {
            largest_capacity.store(std::max(largest_capacity.load(), connection.receive_capacity()));
            if (request.method == "POST") {
                if (request.body.size() == 2 * 1024 * 1024) ++uploads;
                response = fernsdr::build_http_response(404, "text/plain", "disabled", {}, true);
            } else {
                response = fernsdr::build_http_response(200, "text/plain", "TAIL_OK", {}, true);
            }
            return true;
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    std::vector<std::unique_ptr<TestClient>> clients;
    const std::string request = "POST /api/admin/upload HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 2097152\r\n\r\n" + std::string(2 * 1024 * 1024, 'x') +
        "GET /tail HTTP/1.1\r\nHost: localhost\r\n\r\n";
    // More completed uploads than concurrent slots, with every socket kept
    // open. A following GET must survive the buffer replacement too.
    for (int i = 0; i < 6; ++i) {
        clients.push_back(std::make_unique<TestClient>(server.bound_port()));
        CHECK(clients.back()->connected());
        clients.back()->send_raw(request);
        CHECK(clients.back()->read_until("TAIL_OK"));
    }
    server.stop();
    thread.join();
    CHECK_EQ(handler.uploads.load(), 6u);
    CHECK(handler.largest_capacity.load() <= 64 * 1024);
}

// Past the output limit a client is taken for one that stopped reading and
// dropped, unless the handler allowed that response more: the operator's
// backup, several megabytes, on a slow link.
TEST_CASE(server_sends_an_allowed_large_response_to_a_slow_reader) {
    struct Handler : fernsdr::ServerHandler {
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        bool on_http(fernsdr::Connection& connection, const fernsdr::HttpRequest& request,
                     std::string& response) override {
            response = fernsdr::build_http_response(200, "text/plain", std::string(6 * 1024 * 1024, 'b') + "END",
                                                    {}, false);
            if (request.path == "/allowed") connection.allow_output(response.size());
            return true;
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    for (const std::string path : {"/allowed", "/plain"}) {
        // A small receive buffer, set before connecting, and nothing read for
        // a while: the server's queue has to hold nearly all of it.
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int small = 16 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(server.bound_port()));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        CHECK(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        wait_ms(300);
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string received;
        char buffer[65536];
        for (;;) {
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            received.append(buffer, static_cast<size_t>(got));
        }
        ::close(fd);
        const bool whole = received.size() > 6 * 1024 * 1024 && received.compare(received.size() - 3, 3, "END") == 0;
        CHECK_EQ(whole, path == "/allowed");
    }
    server.stop();
    thread.join();
}

// A client that asked to close after the answer, does not read it, and sends
// one byte more: the server has stopped reading, and epoll, which reports
// level, must not be asked about those unread bytes, or it answers at once,
// forever, and the network thread every listener's audio goes through spins.
TEST_CASE(server_does_not_spin_on_a_closing_client_that_does_not_read) {
    struct Handler : fernsdr::ServerHandler {
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        // Larger than the kernel's buffers take, so that some of it waits in
        // the server's own queue, as the admin page's bundle does behind a
        // slow link.
        bool on_http(fernsdr::Connection& connection, const fernsdr::HttpRequest&, std::string& response) override {
            response = fernsdr::build_http_response(200, "text/plain", std::string(8 * 1024 * 1024, 'b'), {}, false);
            connection.allow_output(response.size());
            return true;
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    const int small = 4096;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(server.bound_port()));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string request = "GET /big HTTP/1.0\r\nHost: localhost\r\n\r\n";
    CHECK(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
    wait_ms(200);
    // Once the server has stopped reading.
    CHECK(::send(fd, "X", 1, MSG_NOSIGNAL) == 1);
    wait_ms(100);
    const auto cpu = [] {
        timespec now{};
        ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        return now.tv_sec + now.tv_nsec / 1e9;
    };
    const double before = cpu();
    wait_ms(1000);
    const double spent = cpu() - before;
    CHECK(spent < 0.3);
    ::close(fd);
    server.stop();
    thread.join();
}

// Clients that ask for large answers and read none of them hold what they
// asked for, up to a limit each; together they hold no more than the backlog,
// and a large answer beyond it is refused until there is room.
TEST_CASE(server_holds_all_unread_http_answers_to_one_backlog) {
    struct Handler : fernsdr::ServerHandler {
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        bool on_http(fernsdr::Connection&, const fernsdr::HttpRequest&, std::string& response) override {
            response = fernsdr::build_http_response(200, "text/plain", std::string(6 * 1024 * 1024, 'b'), {}, true);
            return true;
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    config.max_connections_per_address = 0;
    // Answers larger than the kernel's socket buffers (4 MB here), so that
    // some of each waits in the server, as behind a slow link.
    config.max_output_bytes = 16 * 1024 * 1024;
    config.http_backlog_bytes = 8 * 1024 * 1024;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    std::vector<int> sockets;
    int refused = 0;
    for (int i = 0; i < 12; i++) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int small = 4096;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(server.bound_port()));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        const std::string request = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
        CHECK(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        wait_ms(50);
        char head[64] = {0};
        const ssize_t got = ::recv(fd, head, sizeof(head) - 1, MSG_DONTWAIT);
        if (got > 0 && std::string(head).find(" 503 ") != std::string::npos) refused++;
        sockets.push_back(fd);
    }
    // The first few are answered; the rest wait.
    CHECK(refused > 0);
    CHECK(refused < 12);
    for (int fd : sockets) ::close(fd);
    server.stop();
    thread.join();
}

TEST_CASE(server_completes_the_handshake_and_sends_a_welcome) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.connected());
    CHECK(client.handshake());

    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(500, frames);
    CHECK(contains(frames, "\"type\":\"welcome\""));
    CHECK(contains(frames, "Test Receiver"));
    CHECK(contains(frames, "\"type\":\"state\""));
}

TEST_CASE(server_streams_audio_and_waterfall_after_tuning) {
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> everything;
    client.collect(300, everything);

    client.send_text(R"({"type":"tune","band":"demo","freq":7099200,"mode":"usb"})");
    client.send_text(R"({"type":"viewport","enabled":true,"low":7050000,"high":7150000,"width":256,"fps":10})");

    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(1200, frames);

    // Roughly 94 audio frames a second; allow plenty of slack for scheduling.
    CHECK(count_binary(frames, 0x01) > 30);
    CHECK(count_binary(frames, 0x02) > 2);
    CHECK(contains(frames, "\"type\":\"meter\""));

    // The audio configuration is announced as soon as the stream exists, which
    // is before the tune arrives, so look across the whole conversation.
    everything.insert(everything.end(), frames.begin(), frames.end());
    CHECK(contains(everything, "\"type\":\"audio-config\""));
    CHECK(contains(everything, "\"rate\":"));
}

TEST_CASE(server_releases_the_session_when_a_client_disconnects) {
    // The regression this whole file exists for.
    Harness harness;
    if (!harness.ok) return;

    for (int round = 0; round < 3; round++) {
        TestClient client(harness.port());
        CHECK(client.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        client.collect(250, frames);
        CHECK_EQ(harness.application->session_count(), 1);
        CHECK_EQ(harness.radio.total_listeners(), 1);

        client.close();
        // Give the server a moment to notice.
        for (int i = 0; i < 50 && harness.application->session_count() != 0; i++) wait_ms(20);

        CHECK_EQ(harness.application->session_count(), 0);
        CHECK_EQ(harness.radio.total_listeners(), 0);
    }
}

TEST_CASE(server_refuses_users_beyond_capacity) {
    Harness harness;  // configured with max_users = 3
    if (!harness.ok) return;

    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 3; i++) {
        clients.push_back(std::make_unique<TestClient>(harness.port()));
        CHECK(clients.back()->handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        clients.back()->collect(200, frames);
    }
    CHECK_EQ(harness.application->session_count(), 3);

    TestClient extra(harness.port());
    CHECK(extra.handshake());  // the handshake succeeds; the close comes after
    std::vector<std::pair<uint8_t, std::string>> frames;
    extra.collect(400, frames);
    // Rejected with a close frame rather than a welcome.
    CHECK(!contains(frames, "\"type\":\"welcome\""));
    CHECK_EQ(harness.application->session_count(), 3);
}

TEST_CASE(server_refuses_one_address_more_listeners_than_its_share) {
    Harness harness(15000, 120000, "", "", "max_users_per_address = 2\n");
    if (!harness.ok) return;

    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 2; i++) {
        clients.push_back(std::make_unique<TestClient>(harness.port()));
        CHECK(clients.back()->handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        clients.back()->collect(200, frames);
        CHECK(contains(frames, "\"type\":\"welcome\""));
    }

    // There is room on the receiver, but not for a third from this address,
    // and the listener is told which of the two it was.
    TestClient extra(harness.port());
    CHECK(extra.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    extra.collect(400, frames);
    CHECK(!contains(frames, "\"type\":\"welcome\""));
    int closes = 0;
    for (const auto& frame : frames) {
        if (frame.first != 0x8) continue;
        closes++;
        CHECK(frame.second.find("too many listeners from this address") != std::string::npos);
    }
    CHECK_EQ(closes, 1);
    CHECK_EQ(harness.application->session_count(), 2);
}

TEST_CASE(server_tells_listeners_when_the_chat_is_off) {
    Harness harness(15000, 120000, "", "", "chat = no\n");
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(200, frames);
    CHECK(contains(frames, "\"chat\":false"));
    client.send_text("{\"type\":\"chat\",\"name\":\"G0ABC\",\"text\":\"hello\"}");
    frames.clear();
    client.collect(300, frames);
    CHECK(contains(frames, "\"type\":\"chat-refused\""));
    CHECK(contains(frames, "the chat is off on this receiver"));
    CHECK(!contains(frames, "\"type\":\"chat\""));
}

TEST_CASE(server_answers_the_status_api) {
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    const std::string response = client.http_get("/api/status");
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("Test Receiver") != std::string::npos);
    CHECK(response.find("\"bands\"") != std::string::npos);
}

TEST_CASE(server_negotiates_binary_meters_without_changing_legacy_clients) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);
    CHECK(contains(frames, "meter-v1"));
    CHECK(contains(frames, "audio-discontinuity"));
    CHECK_EQ(count_binary(frames, 0x03), 0);
    for (const auto& frame : frames) {
        if (frame.first == 2 && frame.second.size() > 4 && frame.second[0] == 1) CHECK_EQ(frame.second[1] & 2, 0);
    }
    client.send_text(R"({"type":"hello","capabilities":["meter-v1","nac2"]})");
    frames.clear();
    client.collect(400, frames);
    CHECK(count_binary(frames, 0x03) > 0);
    for (const auto& frame : frames) {
        if (frame.first == 2 && !frame.second.empty() && frame.second[0] == 3) CHECK_EQ(frame.second.size(), 26u);
    }
    CHECK(count_binary(frames, 0x01) > 10);
    size_t compact_audio = 0;
    for (const auto& frame : frames) {
        if (frame.first == 2 && frame.second.size() > 4 && frame.second[0] == 1 && (frame.second[1] & 2)) compact_audio++;
    }
    CHECK(compact_audio > 5);
}

TEST_CASE(server_sends_nac3_packets_that_decode_on_an_unbroken_frame_clock) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames, opening;
    client.collect(250, opening);
    CHECK(contains(opening, "nac3"));
    client.send_text(R"({"type":"hello","capabilities":["nac3","audio-discontinuity"]})");
    client.send_text(R"({"type":"audio","enabled":true,"bitrate":48000,"frames":3,"noise_margin":14})");
    client.send_text(R"({"type":"tune","band":"demo","freq":7099200,"mode":"usb"})");
    frames.clear();
    client.collect(1500, frames);
    CHECK(contains(frames, "\"audio_codec\":\"nac3\""));
    CHECK(contains(frames, "\"audio_frames\":3"));

    // Once the settings apply, every audio message is a NAC3 packet of three
    // frames whose sequence follows the previous one by its frame count.
    int rate = 0;
    std::vector<std::pair<uint8_t, std::string>> conversation = opening;
    conversation.insert(conversation.end(), frames.begin(), frames.end());
    for (const auto& frame : conversation) {
        if (frame.first != 1) continue;
        const size_t found = frame.second.find("\"rate\":");
        if (frame.second.find("audio-config") != std::string::npos && found != std::string::npos) {
            rate = std::atoi(frame.second.c_str() + found + 7);
        }
    }
    CHECK(rate > 0);
    if (rate <= 0) return;
    fernsdr::nac::Decoder decoder(rate);
    std::vector<float> out(fernsdr::nac::kFrameHop * fernsdr::nac::kMaxPacketFrames);
    int packets = 0, decoded_frames = 0, expected = -1;
    for (const auto& frame : frames) {
        if (frame.first != 2 || frame.second.size() < 5 || frame.second[0] != 1) continue;
        const auto* bytes = reinterpret_cast<const uint8_t*>(frame.second.data());
        if (!(bytes[1] & fernsdr::proto::kAudioFlagPacket)) continue;
        const int sequence = bytes[2] | (bytes[3] << 8);
        const int count = fernsdr::proto::audio_frames(bytes, frame.second.size());
        if (expected >= 0) CHECK_EQ(sequence, expected);
        CHECK_EQ(bytes[1] & fernsdr::proto::kAudioFlagDiscontinuity, 0);
        expected = (sequence + count) & 0xffff;
        bool ok = false;
        CHECK_EQ(decoder.decode_packet(bytes + 4, frame.second.size() - 4, out.data(),
                                       fernsdr::nac::kMaxPacketFrames, ok), count);
        CHECK(ok);
        packets++;
        decoded_frames += count;
        if (packets > 3) CHECK_EQ(count, 3);
    }
    CHECK(packets > 10);
    CHECK(decoded_frames > 30);
}

TEST_CASE(server_keeps_the_audio_frame_clock_across_packet_changes) {
    // Packet size and codec change while a packet is half built. Frames
    // already coded carry sequence numbers the client is waiting for, so
    // every change must leave the numbering unbroken and every packet whole.
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);
    client.send_text(R"({"type":"hello","capabilities":["nac3","nac2","audio-discontinuity"]})");
    client.send_text(R"({"type":"audio","enabled":true,"bitrate":48000,"frames":4})");
    client.send_text(R"({"type":"tune","band":"demo","freq":7099200,"mode":"usb"})");
    client.collect(900, frames);
    client.send_text(R"({"type":"audio","frames":1})");
    client.collect(400, frames);
    client.send_text(R"({"type":"audio","frames":3})");
    client.collect(400, frames);
    // Leaving NAC3 altogether, as a client that reconnects without it would.
    client.send_text(R"({"type":"hello","capabilities":["nac2","audio-discontinuity"]})");
    client.collect(600, frames);

    int expected = -1, packets = 0, plain = 0, generation = -1;
    bool one_frame_packet = false;
    for (const auto& frame : frames) {
        if (frame.first != 2 || frame.second.size() < 5 || frame.second[0] != 1) continue;
        const auto* bytes = reinterpret_cast<const uint8_t*>(frame.second.data());
        // A rebuilt channel starts a new stream; numbering is only promised
        // within one.
        const int frame_generation = bytes[1] >> 4;
        if (frame_generation != generation) {
            generation = frame_generation;
            expected = -1;
        }
        const int sequence = bytes[2] | (bytes[3] << 8);
        const int count = fernsdr::proto::audio_frames(bytes, frame.second.size());
        if (expected >= 0) CHECK_EQ(sequence, expected);
        CHECK_EQ(bytes[1] & fernsdr::proto::kAudioFlagDiscontinuity, 0);
        expected = (sequence + count) & 0xffff;
        if (bytes[1] & fernsdr::proto::kAudioFlagPacket) {
            packets++;
            if (count == 1) one_frame_packet = true;
        } else {
            plain++;
        }
    }
    CHECK(packets > 10);
    CHECK(one_frame_packet);
    CHECK(plain > 10);
}

TEST_CASE(server_prunes_before_writing_without_disturbing_upgrade_control_or_close) {
    struct Handler : fernsdr::ServerHandler {
        std::atomic<int> dropped{0};
        std::atomic<int> disconnects{0};
        bool on_connect(fernsdr::Connection& connection) override {
            connection.send_text("welcome");
            connection.send_binary(std::vector<uint8_t>{1, 0, 0, 0});
            connection.send_text("audio-config");
            connection.close(1000, "finished");
            return true;
        }
        void on_text(fernsdr::Connection&, const std::string&) override {}
        void on_disconnect(fernsdr::Connection&) override { disconnects++; }
        void on_flush() override {}
        void on_tick() override {}
        void on_before_write(fernsdr::Connection& connection) override {
            dropped += static_cast<int>(connection.prune_binary(fernsdr::monotonic_ms(), 0,
                [](uint8_t*, size_t, int64_t) { return false; }));
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    TestClient client(server.bound_port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(100, frames);
    server.stop();
    thread.join();
    CHECK_EQ(handler.dropped.load(), 1);
    CHECK_EQ(handler.disconnects.load(), 1);
    CHECK_EQ(frames.size(), 3);
    if (frames.size() == 3) {
        CHECK_EQ_STR(frames[0].second, "welcome");
        CHECK_EQ_STR(frames[1].second, "audio-config");
        CHECK_EQ(frames[2].first, 8);
    }
}

TEST_CASE(session_negotiates_audio_discontinuity_per_connection) {
    fernsdr::Radio radio;
    fernsdr::Session first(1, radio), second(2, radio);
    CHECK(!first.delivery().audio_discontinuity());
    first.handle_text(R"({"type":"hello","capabilities":["audio-discontinuity"]})");
    CHECK(first.delivery().audio_discontinuity());
    first.handle_text(R"({"type":"hello","capabilities":["nac2"]})");
    CHECK(!first.delivery().audio_discontinuity());
    first.handle_text(R"({"type":"hello","capabilities":["audio-discontinuity"]})");
    CHECK(first.delivery().audio_discontinuity());
    CHECK(!second.delivery().audio_discontinuity());
    second.handle_text(R"({"type":"hello","capabilities":["nac2"]})");
    CHECK(!second.delivery().audio_discontinuity());
}

TEST_CASE(server_keeps_a_connection_open_when_pruning_removes_all_pending_output) {
    struct Handler : fernsdr::ServerHandler {
        std::atomic<bool> pruned_empty{false};
        bool on_connect(fernsdr::Connection&) override { return true; }
        void on_text(fernsdr::Connection& connection, const std::string& text) override {
            if (text == "discard") connection.send_binary(std::vector<uint8_t>{1, 0, 0, 0});
            else connection.send_text("still connected");
        }
        void on_disconnect(fernsdr::Connection&) override {}
        void on_flush() override {}
        void on_tick() override {}
        void on_before_write(fernsdr::Connection& connection) override {
            const size_t removed = connection.prune_binary(fernsdr::monotonic_ms(), 0,
                [](uint8_t*, size_t, int64_t) { return false; });
            if (removed && !connection.pending_bytes()) pruned_empty.store(true);
        }
    } handler;
    fernsdr::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    fernsdr::Server server(config, handler);
    std::string error;
    CHECK(server.start(error));
    if (!error.empty()) return;
    std::thread thread([&] { server.run(); });
    TestClient client(server.bound_port());
    CHECK(client.handshake());
    client.send_text("discard");
    const auto deadline = fernsdr::monotonic_ms() + 5000;
    while (!handler.pruned_empty.load() && fernsdr::monotonic_ms() < deadline) wait_ms(1);
    CHECK(handler.pruned_empty.load());
    client.send_text("check");
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(100, frames);
    server.stop();
    thread.join();
    CHECK(contains(frames, "still connected"));
    CHECK_EQ(count_binary(frames, 1), 0);
}

TEST_CASE(server_updates_band_counts_when_another_listener_leaves) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient observer(harness.port()), other(harness.port());
    CHECK(observer.handshake());
    CHECK(other.handshake());
    const auto count = [&observer] {
        std::vector<std::pair<uint8_t, std::string>> frames;
        observer.collect(1400, frames);
        int listeners = -1;
        for (const auto& frame : frames) {
            fernsdr::Json message;
            if (frame.first == 1 && fernsdr::Json::parse(frame.second, message) &&
                message["type"].string() == "band-status") {
                listeners = static_cast<int>(message["bands"][size_t{0}]["listeners"].number());
            }
        }
        return listeners;
    };
    CHECK_EQ(count(), 2);
    other.close();
    CHECK_EQ(count(), 1);
}

TEST_CASE(server_header_and_body_deadlines_survive_trickled_bytes) {
    Harness harness(100, 150);
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient header(harness.port()), body(harness.port());
    header.send_raw("GET / HTTP/1.1\r\nHost: localhost\r\nX-Slow: ");
    body.send_raw("POST /api/admin/login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10000\r\n\r\n");
    for (int i = 0; i < 40; i++) {
        if (!header.peer_closed()) header.send_raw("x");
        if (!body.peer_closed()) body.send_raw("x");
        if (header.peer_closed() && body.peer_closed()) break;
        wait_ms(50);
    }
    CHECK(header.peer_closed());
    CHECK(body.peer_closed());
    TestClient healthy(harness.port());
    CHECK(healthy.http_get("/api/status").find("200 OK") != std::string::npos);
}

TEST_CASE(server_bounds_http_pipeline_work) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    std::string requests;
    for (int i = 0; i < 100; i++) requests += "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    client.send_raw(requests);
    CHECK(client.read_until("429 Too Many Requests"));
}

TEST_CASE(server_rejects_non_get_websocket_upgrades) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    client.send_raw("POST /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\n"
                    "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    CHECK(client.read_until("400 Bad Request"));
    CHECK_EQ(harness.application->session_count(), 0);
}

TEST_CASE(server_reports_errors_without_dropping_the_connection) {
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);

    client.send_text("this is not json");
    client.send_text(R"({"type":"tune","mode":"not-a-mode"})");
    client.send_text(R"({"type":"nonsense"})");

    frames.clear();
    client.collect(400, frames);
    CHECK(contains(frames, "\"type\":\"error\""));
    // Still connected and still streaming: a bad message must not cost the
    // user their audio.
    CHECK_EQ(harness.application->session_count(), 1);
    CHECK(count_binary(frames, 0x01) > 5);
}

TEST_CASE(server_clamps_out_of_range_settings_and_says_so) {
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);

    // A passband far wider than the band allows, and a frequency outside it.
    client.send_text(R"({"type":"tune","freq":99000000,"mode":"usb","low":-500000,"high":500000})");
    frames.clear();
    client.collect(400, frames);
    CHECK(contains(frames, "\"note\""));
    CHECK_EQ(harness.application->session_count(), 1);
}

TEST_CASE(server_holds_the_audio_filters_to_their_range_and_reports_them) {
    using fernsdr::Json;
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.handshake());
    client.send_text(R"({"type":"tune","band":"demo","freq":7099200,"mode":"fm"})");
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);

    // What the listener's state says the two filters are now.
    const auto reported = [&](double& highpass, double& deemphasis) {
        client.send_text(R"({"type":"state"})");
        frames.clear();
        client.collect(300, frames);
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            Json message;
            if (it->first != 1 || !Json::parse(it->second, message) || message["type"].string() != "state") continue;
            highpass = message["highpass"].number(-1.0);
            deemphasis = message["deemphasis"].number(-1.0);
            return true;
        }
        return false;
    };
    double highpass = -1.0, deemphasis = -1.0;
    CHECK(reported(highpass, deemphasis));
    CHECK_EQ(highpass, 0.0);         // off until asked for
    CHECK_EQ(deemphasis, 300.0);     // what a land-mobile transmitter pre-emphasises with

    client.send_text(R"({"type":"dsp","highpass":200,"deemphasis":75})");
    CHECK(reported(highpass, deemphasis));
    CHECK_EQ(highpass, 200.0);
    CHECK_EQ(deemphasis, 75.0);

    // Beyond what either is for: held to the nearest end, not refused.
    client.send_text(R"({"type":"dsp","highpass":50000,"deemphasis":-20})");
    CHECK(reported(highpass, deemphasis));
    CHECK_EQ(highpass, 1000.0);
    CHECK_EQ(deemphasis, 0.0);
    client.send_text(R"({"type":"dsp","highpass":-5,"deemphasis":1e9})");
    CHECK(reported(highpass, deemphasis));
    CHECK_EQ(highpass, 0.0);
    CHECK_EQ(deemphasis, 2000.0);
    CHECK_EQ(harness.application->session_count(), 1);
}

TEST_CASE(server_reports_the_gain_control_a_listener_asked_for_and_the_one_it_has) {
    using fernsdr::Json;
    Harness harness;
    if (!harness.ok) return;

    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(250, frames);
    // The latest state, and whether an error came with it.
    const auto state = [&](Json& latest, bool& error) {
        client.send_text(R"({"type":"state"})");
        frames.clear();
        client.collect(300, frames);
        error = false;
        bool found = false;
        for (const auto& [opcode, text] : frames) {
            Json message;
            if (opcode != 1 || !Json::parse(text, message)) continue;
            if (message["type"].string() == "error") error = true;
            if (message["type"].string() == "state") latest = message, found = true;
        }
        return found;
    };
    Json latest;
    bool error = false;
    CHECK(state(latest, error));
    CHECK_EQ_STR(latest["agc"].string(), "auto");
    CHECK_EQ_STR(latest["agc_effective"].string(), "slow");

    client.send_text(R"({"type":"dsp","agc":"steady"})");
    CHECK(state(latest, error));
    CHECK_EQ_STR(latest["agc"].string(), "slow");

    // An unknown name is answered with an error and costs only itself.
    client.send_text(R"({"type":"dsp","agc":"bogus","volume":0.5})");
    CHECK(state(latest, error));
    CHECK(error);
    CHECK_EQ_STR(latest["agc"].string(), "slow");
    CHECK_EQ(latest["volume"].number(), 0.5);

    // NFM runs without, whatever was asked for.
    client.send_text(R"({"type":"dsp","agc":"fast"})");
    client.send_text(R"({"type":"tune","band":"demo","freq":7099200,"mode":"nfm"})");
    CHECK(state(latest, error));
    CHECK_EQ_STR(latest["agc"].string(), "fast");
    CHECK_EQ_STR(latest["agc_effective"].string(), "off");
    CHECK_EQ(harness.application->session_count(), 1);
}

// --- Forwarded addresses ---------------------------------------------------
//
// Anyone can send X-Forwarded-For. Believing it from an untrusted peer would
// let a listener write any address they liked into the log and walk straight
// past a per-address limit, so these tests are about what is *not* believed.

namespace {

fernsdr::HttpRequest request_with(const std::vector<std::pair<std::string, std::string>>& headers) {
    fernsdr::HttpRequest request;
    request.headers = headers;
    return request;
}

}  // namespace

TEST_CASE(cidr_matching) {
    CHECK(fernsdr::address_matches_cidr("127.0.0.1", "loopback"));
    CHECK(fernsdr::address_matches_cidr("127.9.9.9", "loopback"));
    CHECK(!fernsdr::address_matches_cidr("128.0.0.1", "loopback"));
    CHECK(fernsdr::address_matches_cidr("10.4.5.6", "10.0.0.0/8"));
    CHECK(!fernsdr::address_matches_cidr("11.4.5.6", "10.0.0.0/8"));
    CHECK(fernsdr::address_matches_cidr("192.168.1.7", "192.168.1.7"));
    CHECK(!fernsdr::address_matches_cidr("192.168.1.8", "192.168.1.7"));
    CHECK(fernsdr::address_matches_cidr("8.8.8.8", "any"));
    CHECK(!fernsdr::address_matches_cidr("8.8.8.8", "none"));
    CHECK(!fernsdr::address_matches_cidr("8.8.8.8", ""));
    // Nonsense must not match a prefix by accident.
    CHECK(!fernsdr::address_matches_cidr("10.0.0", "10.0.0.0/8"));
    CHECK(!fernsdr::address_matches_cidr("10.0.0.1.5", "10.0.0.0/8"));
    CHECK(!fernsdr::address_matches_cidr("10.0.0.300", "10.0.0.0/8"));
    // IPv6, added when the listening socket became dual-stack. Before this,
    // a receiver reached over IPv6 through a local proxy attributed every
    // listener to the proxy, because ::1 matched nothing.
    CHECK(fernsdr::address_matches_cidr("::1", "loopback"));
    CHECK(fernsdr::address_matches_cidr("::1", "::1/128"));
    CHECK(!fernsdr::address_matches_cidr("::2", "::1/128"));
    CHECK(fernsdr::address_matches_cidr("2001:db8::5", "2001:db8::/32"));
    CHECK(!fernsdr::address_matches_cidr("2001:db9::5", "2001:db8::/32"));
    CHECK(fernsdr::address_matches_cidr("fd00:1234:5678::1", "fd00::/8"));
    CHECK(!fernsdr::address_matches_cidr("2001:db8::1", "loopback"));
    CHECK(!fernsdr::address_matches_cidr("not:an:address", "::1/128"));

    // An IPv4 peer on a dual-stack socket arrives mapped. It has to match the
    // IPv4 CIDRs an operator actually wrote, or trusted_proxies silently stops
    // working the day the receiver gains an IPv6 address.
    CHECK(fernsdr::address_matches_cidr("::ffff:127.0.0.1", "loopback"));
    CHECK(fernsdr::address_matches_cidr("::ffff:10.1.2.3", "10.0.0.0/8"));
    CHECK(!fernsdr::address_matches_cidr("::ffff:11.1.2.3", "10.0.0.0/8"));
    // And the reverse: a plain IPv4 address against an IPv6 prefix.
    CHECK(fernsdr::address_matches_cidr("10.1.2.3", "::ffff:10.0.0.0/104"));
}

TEST_CASE(network_keys_group_an_ipv6_network_and_keep_ipv4_addresses_apart) {
    using fernsdr::network_key;
    CHECK(network_key("192.0.2.7") == "192.0.2.7");
    CHECK(network_key("192.0.2.8") == "192.0.2.8");
    CHECK(network_key("::ffff:192.0.2.7") == "192.0.2.7");
    CHECK(network_key("2001:db8:1:2:aaaa::1") == "2001:db8:1:2::/64");
    CHECK(network_key("2001:DB8:1:2:bbbb:0:0:2") == "2001:db8:1:2::/64");
    CHECK(network_key("2001:db8:1:3::1") == "2001:db8:1:3::/64");
    CHECK(network_key("2001:0db8:0000:0000:0000::9") == "2001:db8::/64");
    // This machine is one, whichever loopback address a process picks.
    for (const char* address : {"127.0.0.1", "127.3.4.5", "::1", "::ffff:127.0.0.9"}) {
        CHECK(network_key(address) == "loopback");
    }
    CHECK(network_key("::2") == "::/64");
    // A key given back is the same key, and anything else is left alone.
    CHECK(network_key("2001:db8:1:2::/64") == "2001:db8:1:2::/64");
    CHECK(network_key("loopback") == "loopback");
    CHECK(network_key("not an address") == "not an address");
    CHECK(network_key("") == "");
}

TEST_CASE(forwarded_headers_are_ignored_from_an_untrusted_peer) {
    const auto request = request_with({{"x-forwarded-for", "1.2.3.4"}, {"x-real-ip", "5.6.7.8"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("203.0.113.9", request, {"loopback"}),
                 "203.0.113.9");
    // An empty trust list means nothing is believed, not that everything is.
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", request, {}), "127.0.0.1");
}

TEST_CASE(forwarded_headers_are_believed_from_a_trusted_proxy) {
    const auto request = request_with({{"x-forwarded-for", "1.2.3.4"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", request, {"loopback"}),
                 "1.2.3.4");

    // Only skip hops the operator has explicitly marked as trusted.
    const auto chained = request_with({{"x-forwarded-for", "1.2.3.4, 10.0.0.1, 10.0.0.2"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", chained, {"loopback"}),
                 "10.0.0.2");
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", chained, {"loopback", "10.0.0.0/8"}),
                 "1.2.3.4");

    // X-Real-IP is the fallback when there is no chain.
    const auto real = request_with({{"x-real-ip", " 9.9.9.9 "}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", real, {"loopback"}), "9.9.9.9");
}

TEST_CASE(forwarded_headers_may_carry_either_family) {
    const auto six = request_with({{"x-forwarded-for", "2001:db8::42"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("::1", six, {"loopback"}), "2001:db8::42");
    const auto mapped = request_with({{"x-real-ip", "::ffff:203.0.113.4"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", mapped, {"loopback"}),
                 "::ffff:203.0.113.4");
}

TEST_CASE(a_malformed_forwarded_header_falls_back_to_the_peer) {
    for (const char* value : {"", "not-an-address", "1.2.3", "1.2.3.4.5", "; drop table", "999.1.1.1"}) {
        const auto request = request_with({{"x-forwarded-for", value}});
        CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", request, {"loopback"}),
                     "127.0.0.1");
    }
}

TEST_CASE(cidr_typos_do_not_expand_proxy_trust) {
    for (const char* prefix : {"garbage", "24oops", "-1", "+0", "9999999999999999999999", ""}) {
        CHECK(!fernsdr::address_matches_cidr("10.1.2.3", std::string("10.0.0.0/") + prefix));
        CHECK(!fernsdr::address_matches_cidr("2001:db8::1", std::string("2001:db8::/") + prefix));
    }
    const auto forged = request_with({{"x-forwarded-for", "1.2.3.4, 203.0.113.9"}});
    CHECK_EQ_STR(fernsdr::resolve_client_address("127.0.0.1", forged, {"loopback"}), "203.0.113.9");
}

TEST_CASE(https_requires_a_trusted_socket_peer) {
    const auto claimed = request_with({{"x-forwarded-proto", "https"}, {"x-forwarded-for", "127.0.0.1"}});
    CHECK(!fernsdr::request_is_secure("203.0.113.4", claimed, {"loopback"}));
    CHECK(!fernsdr::request_is_secure("127.0.0.1", claimed, {}));
    CHECK(fernsdr::request_is_secure("127.0.0.1", claimed, {"loopback"}));
    CHECK(!fernsdr::request_is_secure("127.0.0.1", request_with({{"x-forwarded-proto", "http"}}), {"loopback"}));
}

// --- Limits a single client cannot get around ------------------------------

TEST_CASE(server_limits_the_connections_one_address_may_hold) {
    // One address could otherwise open every slot and keep it, and nobody else
    // could connect: not a listener, not the operator.
    Harness harness(15000, 120000, "", "", "", [](fernsdr::ServerConfig& config) {
        config.max_connections_per_address = 3;
    });
    CHECK(harness.ok);
    std::vector<std::unique_ptr<TestClient>> held;
    for (int i = 0; i < 3; i++) {
        held.push_back(std::make_unique<TestClient>(harness.port()));
        CHECK(held.back()->connected());
    }
    wait_ms(100);
    TestClient fourth(harness.port());
    CHECK(fourth.read_until("too many connections from this address"));
    held.pop_back();
    wait_ms(100);
    TestClient again(harness.port());
    const std::string answer = again.http_get("/api/health");
    CHECK(answer.find("200 OK") != std::string::npos);
}

TEST_CASE(server_lets_this_machine_in_when_full) {
    // The operator's way in over ssh -L arrives from loopback; a full
    // receiver keeps a few slots for it.
    Harness harness(15000, 120000, "", "", "", [](fernsdr::ServerConfig& config) {
        config.max_connections = 2;
        config.max_connections_per_address = 0;
    });
    CHECK(harness.ok);
    TestClient one(harness.port()), two(harness.port());
    wait_ms(100);
    TestClient three(harness.port());
    const std::string answer = three.http_get("/api/health");
    CHECK(answer.find("200 OK") != std::string::npos);
}

TEST_CASE(server_closes_a_kept_alive_connection_with_nothing_to_do) {
    Harness harness(15000, 120000, "", "", "", [](fernsdr::ServerConfig& config) {
        config.keepalive_idle_ms = 300;
    });
    CHECK(harness.ok);
    TestClient client(harness.port());
    client.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(client.read_until("\r\n\r\n"));
    wait_ms(100);
    CHECK(!client.peer_closed());
    wait_ms(700);
    CHECK(client.peer_closed());
}

TEST_CASE(server_refuses_an_upload_body_from_someone_not_signed_in) {
    // Answered as soon as the head shows the body is large and nobody has
    // signed in, not once all of it has trickled in: four slow uploads used to
    // hold every upload slot for two minutes each.
    Harness harness;
    CHECK(harness.ok);
    TestClient client(harness.port());
    client.send_raw("POST /api/admin/upload HTTP/1.1\r\nHost: localhost\r\nContent-Type: image/png\r\n"
                    "Content-Length: 5000000\r\n\r\n");
    client.send_raw(std::string(70 * 1024, 'x'));
    CHECK(client.read_until("401"));
    CHECK(client.read_until("sign in to the admin panel to upload"));
}

TEST_CASE(server_refuses_a_listener_message_over_its_limit) {
    Harness harness;
    CHECK(harness.ok);
    TestClient small(harness.port());
    CHECK(small.handshake());
    small.send_text("{\"type\":\"chat\",\"text\":\"" + std::string(7000, 'a') + "\"}");
    std::vector<std::pair<uint8_t, std::string>> frames;
    small.collect(300, frames);
    CHECK(!small.peer_closed());

    TestClient large(harness.port());
    CHECK(large.handshake());
    large.send_text("{\"type\":\"chat\",\"text\":\"" + std::string(9000, 'a') + "\"}");
    frames.clear();
    large.collect(500, frames);
    bool closed = large.peer_closed();
    for (const auto& frame : frames) closed = closed || frame.first == 0x8;
    CHECK(closed);
}

TEST_CASE(radio_refuses_a_band_id_that_would_not_be_a_plain_name) {
    // The id names the default history file and travels in URLs.
    for (const char* id : {"../escape", "a/b", ".hidden", "sp ace"}) {
        fernsdr::Config config;
        std::string error;
        CHECK(config.parse(std::string("[band:") + id + "]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n", error));
        fernsdr::Radio radio;
        CHECK(!radio.configure(config, error));
        CHECK(error.find("needs a name of letters") != std::string::npos);
    }
    fernsdr::Config config;
    std::string error;
    CHECK(config.parse("[band:40m_cw.2]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n", error));
    fernsdr::Radio radio;
    CHECK(radio.configure(config, error));
}

TEST_CASE(server_groups_ipv6_addresses_by_48_for_its_wider_limit) {
    // Two /64s of one /48 are one household for the wider limit, and an IPv4
    // address has no wider group.
    CHECK_EQ_STR(fernsdr::wide_network_key("2001:db8:1:2::1"), "2001:db8:1::/48");
    CHECK_EQ_STR(fernsdr::wide_network_key("2001:db8:1:3::99"), "2001:db8:1::/48");
    CHECK(fernsdr::network_key("2001:db8:1:2::1") != fernsdr::network_key("2001:db8:1:3::99"));
    CHECK(fernsdr::wide_network_key("2001:db8:2::1") != fernsdr::wide_network_key("2001:db8:1::1"));
    CHECK_EQ_STR(fernsdr::wide_network_key("203.0.113.7"), "");
    CHECK_EQ_STR(fernsdr::wide_network_key("::ffff:203.0.113.7"), "");
}

TEST_CASE(admin_a_signed_in_upload_passes_the_early_check) {
    // The early refusal must not refuse the operator: a large upload from a
    // signed-in session goes through the real check and is stored. Once as
    // localhost, once under the machine's own name with home_network = yes,
    // which the early check has to honour as the API does.
    const std::string password = "test admin large upload";
    for (const std::string host : {"localhost", "raspberrypi:8073"}) {
        char uploads[] = "/tmp/fernsdr-large-upload-XXXXXX";
        CHECK(::mkdtemp(uploads) != nullptr);
        Harness harness(15000, 120000,
                        "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\nhome_network = " +
                            (host == "localhost" ? "no" : "yes") + "\n",
                        "", "",
                        [&](fernsdr::ServerConfig& config) {
                            config.uploads_root = uploads;
                            config.document_root = uploads;
                        });
        CHECK(harness.ok);
        AdminClient admin;
        admin.host = host;
        CHECK(admin.sign_in(harness.port(), password));
        // A valid PNG header, then enough bytes to pass the 64 kB mark.
        std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\rIHDR", 8) +
                          std::string("\0\0\0\x01\0\0\0\x01\x08\x02\0\0\0", 13) + std::string(4, '\0');
        png += std::string(100 * 1024, '\0');
        const std::string response = admin.request("POST", "/api/admin/upload", png);
        CHECK(response.find("401") == std::string::npos);
        CHECK(response.find("/uploads/") != std::string::npos);
        // Emptied again: whatever was stored, then the directory.
        if (DIR* directory = ::opendir(uploads)) {
            while (const dirent* entry = ::readdir(directory)) {
                if (entry->d_name[0] != '.') ::unlink((std::string(uploads) + "/" + entry->d_name).c_str());
            }
            ::closedir(directory);
        }
        ::rmdir(uploads);
    }
}

TEST_CASE(session_warns_a_minute_before_the_listener_timeout_and_expires_once) {
    fernsdr::Radio radio;
    fernsdr::Session session(1, radio);
    const auto now = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const int64_t limit = 10 * 60000;
    const int64_t start = now();
    auto texts_after = [&](int64_t at) {
        std::vector<std::string> texts;
        std::vector<std::vector<uint8_t>> binaries;
        const auto result = session.check_inactivity(limit, at);
        session.collect(texts, binaries);
        return std::make_pair(result, texts);
    };
    // No limit: nothing, however long.
    CHECK(session.check_inactivity(0, start + 100 * limit) == fernsdr::Session::Inactivity::None);
    auto [early, early_texts] = texts_after(start + 8 * 60000);
    CHECK(early == fernsdr::Session::Inactivity::None);
    CHECK(early_texts.empty());
    auto [warned, warning] = texts_after(start + limit - 30000);
    CHECK(warned == fernsdr::Session::Inactivity::None);
    CHECK_EQ(warning.size(), 1);
    if (!warning.empty()) CHECK(warning[0].find("\"inactivity\"") != std::string::npos);
    // Only once.
    auto [again, again_texts] = texts_after(start + limit - 20000);
    CHECK(again_texts.empty());
    // Answering resets the clock and the warning.
    session.handle_text(R"({"type":"active"})");
    const int64_t answered = now();
    CHECK(session.check_inactivity(limit, answered + limit - 90000) == fernsdr::Session::Inactivity::None);
    auto [rewarned, rewarning] = texts_after(answered + limit - 10000);
    CHECK_EQ(rewarning.size(), 1);
    // Past the limit: expired, and reported once.
    CHECK(session.check_inactivity(limit, answered + limit + 1) == fernsdr::Session::Inactivity::Expired);
    CHECK(session.check_inactivity(limit, answered + limit + 5000) == fernsdr::Session::Inactivity::None);
}

// The chat's backlog is up to 80 lines of 400 bytes, and asking for it is
// 36: answered once in five seconds per connection, a script that keeps
// asking cannot turn the receiver's uplink into its own.
// Up to a megabyte of history for a request of a hundred bytes, read from
// disk: an address may read 16 MB at once and a megabyte a second after
// that, and is told to wait beyond it.
TEST_CASE(server_holds_one_address_to_a_budget_for_the_waterfall_archive) {
    char directory[] = "/tmp/fernsdr-history-budget-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    {
        Harness harness(15000, 120000,
                        "history = public\nhistory_path = " + std::string(directory) +
                            "/h.wfa\nhistory_bins = 4096\nhistory_interval = 0.1\n");
        CHECK(harness.ok);
        if (!harness.ok) return;
        wait_ms(3500);
        int answered = 0;
        bool refused = false;
        for (int i = 0; i < 600 && !refused; i++) {
            const std::string reply = TestClient(harness.port()).http_get("/api/history?band=demo&from=0");
            if (reply.find("200 OK") != std::string::npos) answered++;
            refused = reply.find("429") != std::string::npos && reply.find("Retry-After: 5") != std::string::npos;
        }
        CHECK(answered > 3);
        CHECK(refused);
    }
    ::unlink((std::string(directory) + "/h.wfa").c_str());
    ::unlink((std::string(directory) + "/h.wfa.span").c_str());
    ::rmdir(directory);
}

TEST_CASE(session_answers_a_chat_backlog_request_once_in_a_while) {
    fernsdr::Radio radio;
    fernsdr::Session session(1, radio);
    const auto backlogs = [&] {
        std::vector<std::string> texts;
        std::vector<std::vector<uint8_t>> binaries;
        session.collect(texts, binaries);
        int found = 0;
        for (const std::string& text : texts) found += text.find("\"chat-history\"") != std::string::npos;
        return found;
    };
    backlogs();
    for (int i = 0; i < 50; i++) session.handle_text(R"({"type":"chat","history":true})");
    CHECK_EQ(backlogs(), 1);
}

TEST_CASE(session_counts_what_a_person_does_not_what_the_page_does) {
    fernsdr::Radio radio;
    fernsdr::Session session(1, radio);
    const int64_t start = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t limit = 60000 * 5;
    session.handle_text(R"({"type":"ping","t":1})");
    session.handle_text(R"({"type":"state"})");
    // Neither moved the clock: the limit still counts from the start.
    CHECK(session.check_inactivity(limit, start + limit + 1000) == fernsdr::Session::Inactivity::Expired);
}

TEST_CASE(listener_timeout_takes_whole_minutes_up_to_a_day) {
    fernsdr::Radio radio;
    std::string error;
    auto set = [&](const char* json) {
        fernsdr::Json values;
        fernsdr::Json::parse(json, values);
        error.clear();
        return radio.apply_site_json(values, error);
    };
    CHECK(set(R"({"listener_timeout":45})"));
    CHECK_EQ(radio.site().listener_timeout_minutes, 45);
    CHECK(!set(R"({"listener_timeout":1441})"));
    CHECK(!set(R"({"listener_timeout":-1})"));
    CHECK(!set(R"({"listener_timeout":2.5})"));
    CHECK(!error.empty());
    CHECK_EQ(radio.site().listener_timeout_minutes, 45);
    CHECK(set(R"({"listener_timeout":0})"));
    CHECK_EQ(radio.site().listener_timeout_minutes, 0);
}

TEST_CASE(session_warns_at_half_a_short_listener_timeout) {
    fernsdr::Radio radio;
    fernsdr::Session session(1, radio);
    const int64_t start = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> binaries;
    // One minute: no question in the first half, so an answer is not met at
    // once by the next one.
    session.check_inactivity(60000, start + 20000);
    session.collect(texts, binaries);
    CHECK(texts.empty());
    session.check_inactivity(60000, start + 31000);
    session.collect(texts, binaries);
    CHECK_EQ(texts.size(), 1);
}

TEST_CASE(band_plan_is_checked_and_reaches_listeners) {
    {
        fernsdr::Config config;
        fernsdr::Radio radio;
        std::string error;
        config.parse("[site]\nband_plan = region1\n[band:demo]\nsource = test\nsample_rate = 192k\ncenter = 7.1M\n", error);
        CHECK(!radio.configure(config, error));
        CHECK(error.find("band_plan") != std::string::npos);
    }

    const std::string password = "test admin band plan";
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n", "",
                    "band_plan = gb\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    {
        TestClient client(harness.port());
        CHECK(client.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        client.collect(500, frames);
        CHECK(contains(frames, "\"band_plan\":\"gb\""));
    }

    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    std::string reply = admin.request("POST", "/api/admin/station", "{\"station\":{\"band_plan\":\"mars\"}}");
    CHECK(reply.find("400 Bad Request") != std::string::npos);
    CHECK(json_body(reply)["error"].string().find("band plan") != std::string::npos);
    reply = admin.request("POST", "/api/admin/station", "{\"station\":{\"band_plan\":\"jp\"}}");
    CHECK(reply.find("200 OK") != std::string::npos);
    CHECK_EQ_STR(json_body(reply)["station"]["band_plan"].string(), "jp");
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(500, frames);
    CHECK(contains(frames, "\"band_plan\":\"jp\""));
}

TEST_CASE(server_sends_the_ctcss_flag_only_to_pages_that_read_it) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    const auto nfm_flags = [&](const std::string& hello) {
        TestClient client(harness.port());
        CHECK(client.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        client.collect(200, frames);
        CHECK(contains(frames, "meter-ctcss"));
        client.send_text(hello);
        client.send_text(R"({"type":"tune","band":"demo","freq":7100000,"mode":"nfm"})");
        frames.clear();
        client.collect(600, frames);
        // Counted from the first meter in NFM on: one already on its way in
        // the mode before is not about the tone.
        struct { int meters = 0, with = 0, without_after = 0; } counted;
        for (const auto& frame : frames) {
            if (frame.first != 2 || frame.second.size() != 26 || frame.second[0] != 3) continue;
            counted.meters++;
            if (frame.second[1] & 16) {
                counted.with++;
            } else if (counted.with > 0) {
                counted.without_after++;
            }
        }
        return counted;
    };
    // A page from before the tone was measured throws away a reading with a
    // flag it does not know: it must never get one.
    const auto older = nfm_flags(R"({"type":"hello","capabilities":["meter-v1"]})");
    CHECK_EQ(older.with, 0);
    CHECK(older.meters > 0);
    const auto current = nfm_flags(R"({"type":"hello","capabilities":["meter-v1","meter-ctcss"]})");
    CHECK(current.with > 0);
    CHECK_EQ(current.without_after, 0);
}

TEST_CASE(server_answers_a_burst_of_changes_with_few_states_ending_on_the_latest) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.send_text(R"({"type":"tune","band":"demo","freq":7100000,"mode":"usb"})");
    client.collect(400, frames);
    frames.clear();
    // A slider dragged across its range: twenty changes as fast as they come.
    for (int i = 1; i <= 20; i++) {
        client.send_text("{\"type\":\"dsp\",\"nr\":" + std::to_string(i / 100.0) + ",\"request_id\":" + std::to_string(i) + "}");
    }
    client.collect(600, frames);
    std::vector<fernsdr::Json> states;
    for (const auto& frame : frames) {
        fernsdr::Json message;
        if (frame.first == 1 && fernsdr::Json::parse(frame.second, message) && message["type"].string() == "state") {
            states.push_back(message);
        }
    }
    // Before, one state per change: twenty, all of them full.
    CHECK(!states.empty());
    CHECK(states.size() <= 4u);
    CHECK_EQ(states.back()["ack"]["dsp"].number(), 20.0);
    CHECK(std::fabs(states.back()["nr"].number() - 0.2) < 1e-6);
}

TEST_CASE(server_takes_a_standard_tone_for_the_tone_squelch_and_nothing_else) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.send_text(R"({"type":"tune","band":"demo","freq":7100000,"mode":"nfm"})");
    client.collect(300, frames);
    const auto latest = [&](const std::string& message) {
        frames.clear();
        client.send_text(message);
        client.collect(400, frames);
        fernsdr::Json state;
        for (const auto& frame : frames) {
            fernsdr::Json parsed;
            if (frame.first == 1 && fernsdr::Json::parse(frame.second, parsed) && parsed["type"].string() == "state") state = parsed;
        }
        return state["ctcss_squelch"].number(-1.0);
    };
    CHECK_EQ(latest(R"({"type":"dsp","ctcss_squelch":88.5})"), 88.5);
    CHECK_EQ(latest(R"({"type":"dsp","ctcss_squelch":88.4})"), 0.0);
    CHECK_EQ(latest(R"({"type":"dsp","ctcss_squelch":"loud"})"), 0.0);
}

TEST_CASE(welcome_says_what_each_band_listens_with) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(300, frames);
    CHECK(contains(frames, "\"receiver\":\"test\""));
}

TEST_CASE(decodes_reach_listeners_only_from_decoders_the_operator_made_public) {
    {
        Harness closed;
        CHECK(closed.ok);
        if (!closed.ok) return;
        const std::string answer = TestClient(closed.port()).http_get("/api/decodes");
        CHECK(answer.find("404 Not Found") != std::string::npos);
    }

    Harness harness(15000, 120000,
                    "[decoder:ft8]\nmodule = fake\nchannels = demo:7074000\npublic = yes\n"
                    "[decoder:quiet]\nmodule = fake\nchannels = demo:7047500\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    const auto add = [&](const std::string& decoder, const std::string& message) {
        fernsdr::Decode decode;
        decode.decoder = decoder;
        decode.band = "demo";
        decode.mode = "ft8";
        decode.message = message;
        decode.quality = "bp";
        harness.radio.decodes().add(decode, 1000);
    };
    add("ft8", "CQ K1ABC FN42");
    add("quiet", "CQ K9XYZ EN52");
    add("ft8", "K1ABC W9XYZ EN37");

    {
        TestClient client(harness.port());
        CHECK(client.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        client.collect(300, frames);
        CHECK(contains(frames, "\"decoders\":[{\"id\":\"ft8\""));
        CHECK(!contains(frames, "\"quiet\""));
    }

    std::string answer = TestClient(harness.port()).http_get("/api/decodes");
    CHECK(answer.find("200 OK") != std::string::npos);
    fernsdr::Json body = json_body(answer);
    CHECK_EQ(body["decodes"].size(), 2u);
    CHECK_EQ_STR(body["decodes"][0]["message"].string(), "CQ K1ABC FN42");
    CHECK(answer.find("K9XYZ") == std::string::npos);
    CHECK_EQ(body["through"].number(), 3.0);

    // Asked for one at a time, a page walks past the private one without
    // seeing it and without stopping on it.
    answer = TestClient(harness.port()).http_get("/api/decodes?since=1&limit=1");
    body = json_body(answer);
    CHECK_EQ(body["decodes"].size(), 1u);
    CHECK_EQ_STR(body["decodes"][0]["message"].string(), "K1ABC W9XYZ EN37");
    CHECK_EQ(body["through"].number(), 3.0);
    answer = TestClient(harness.port()).http_get("/api/decodes?since=3");
    CHECK_EQ(json_body(answer)["decodes"].size(), 0u);

    for (const char* bad : {"/api/decodes?limit=0", "/api/decodes?limit=5000", "/api/decodes?since=-1",
                            "/api/decodes?since=1x"}) {
        CHECK(TestClient(harness.port()).http_get(bad).find("400 Bad Request") != std::string::npos);
    }
}

TEST_CASE(admin_saving_a_decoder_section_starts_it_and_tells_listeners) {
    using fernsdr::Json;
    const std::string password = "test admin decoders";
    char directory[] = "/tmp/fernsdr-decoders-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    {
        Harness harness(15000, 120000, "[admin]\npassword_hash = " + fernsdr::hash_password(password, 1000) + "\n", path);
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), password));
        TestClient listener(harness.port());
        CHECK(listener.handshake());
        std::vector<std::pair<uint8_t, std::string>> frames;
        listener.collect(200, frames);

        const std::string text = json_body(admin.request("GET", "/api/admin/config"))["text"].string();
        Json save = Json::make_object();
        save.set("text", text + "\n[decoder:ft8]\nmodule = fake\nchannels = demo:7074000\npublic = yes\n");
        std::string reply = admin.request("POST", "/api/admin/config", save.serialize());
        CHECK(reply.find("200 OK") != std::string::npos);
        CHECK_EQ(json_body(reply)["decoders_changed"].size(), 1u);
        CHECK_EQ(json_body(admin.request("GET", "/api/admin/decoders"))["decoders"].size(), 1u);
        frames.clear();
        listener.collect(500, frames);
        CHECK(contains(frames, "\"type\":\"station\""));
        CHECK(contains(frames, "\"decoders\":[{\"id\":\"ft8\""));
        CHECK(TestClient(harness.port()).http_get("/api/decodes").find("200 OK") != std::string::npos);

        // Saved again unchanged, nothing restarts; taken out, it stops.
        reply = admin.request("POST", "/api/admin/config", save.serialize());
        CHECK_EQ(json_body(reply)["decoders_changed"].size(), 0u);

        // Made private, listeners hear of it at once though nothing restarts,
        // and made public again they see only what it decodes from then on.
        fernsdr::Decode heard;
        heard.decoder = "ft8";
        heard.band = "demo";
        heard.mode = "ft8";
        heard.message = "CQ K1ABC FN42";
        heard.quality = "bp";
        harness.radio.decodes().add(heard, 1000);
        CHECK_EQ(json_body(TestClient(harness.port()).http_get("/api/decodes"))["decodes"].size(), 1u);
        Json quiet = Json::make_object();
        quiet.set("text", text + "\n[decoder:ft8]\nmodule = fake\nchannels = demo:7074000\npublic = no\n");
        frames.clear();
        reply = admin.request("POST", "/api/admin/config", quiet.serialize());
        CHECK_EQ(json_body(reply)["decoders_changed"].size(), 0u);
        listener.collect(500, frames);
        CHECK(contains(frames, "\"decoders\":[]"));
        CHECK(TestClient(harness.port()).http_get("/api/decodes").find("404 Not Found") != std::string::npos);
        heard.message = "CQ DL1ABC JO31";
        harness.radio.decodes().add(heard, 1000);
        reply = admin.request("POST", "/api/admin/config", save.serialize());
        const Json visible = json_body(TestClient(harness.port()).http_get("/api/decodes"));
        CHECK_EQ(visible["decodes"].size(), 0u);
        CHECK_EQ(visible["epoch"].string().size(), 16u);
        heard.message = "CQ W9XYZ EN52";
        harness.radio.decodes().add(heard, 1000);
        const Json after = json_body(TestClient(harness.port()).http_get("/api/decodes"));
        CHECK_EQ(after["decodes"].size(), 1u);
        CHECK_EQ_STR(after["decodes"][0]["message"].string(), "CQ W9XYZ EN52");
        // The operator sees everything.
        CHECK_EQ(json_body(admin.request("GET", "/api/decodes"))["decodes"].size(), 3u);
        save.set("text", text);
        reply = admin.request("POST", "/api/admin/config", save.serialize());
        CHECK_EQ(json_body(reply)["decoders_changed"].size(), 1u);
        CHECK_EQ(json_body(admin.request("GET", "/api/admin/decoders"))["decoders"].size(), 0u);
        CHECK(TestClient(harness.port()).http_get("/api/decodes").find("404 Not Found") != std::string::npos);
        CHECK(admin.request("POST", "/api/admin/decoders/restart", "{\"id\":\"ft8\"}").find("404") != std::string::npos);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
}

TEST_CASE(broadcast_fm_is_offered_only_where_the_band_is_wide_enough) {
    Harness harness;
    CHECK(harness.ok);
    if (!harness.ok) return;
    TestClient client(harness.port());
    CHECK(client.handshake());
    std::vector<std::pair<uint8_t, std::string>> frames;
    client.collect(300, frames);
    // 192 kHz of band cannot hold a 200 kHz channel.
    CHECK(!contains(frames, "\"wfm\":true"));
    CHECK(contains(frames, "\"wfm\"]"));
    client.send_text(R"({"type":"tune","band":"demo","freq":7100000,"mode":"wfm"})");
    frames.clear();
    client.collect(400, frames);
    CHECK(contains(frames, "this band has no broadcast FM; narrow FM instead"));
    CHECK(contains(frames, "\"mode\":\"nfm\""));
    CHECK(!contains(frames, "\"mode\":\"wfm\""));
    CHECK(!contains(frames, "\"type\":\"error\""));
}

TEST_CASE(admin_lists_the_radios_and_restarts_only_where_something_starts_it_again) {
    const std::string password = "test admin hardware and restart";
    ::unsetenv("INVOCATION_ID");
    ::unsetenv("FERNSDR_SUPERVISED");
    ::unsetenv("FERNSDR_CONTAINER");
    Harness harness(15000, 120000, "[admin]\npassword_hash=" + fernsdr::hash_password(password, 1000) + "\n");
    CHECK(harness.ok);
    if (!harness.ok) return;
    AdminClient admin;
    CHECK(admin.sign_in(harness.port(), password));
    const fernsdr::Json hardware = json_body(admin.request("GET", "/api/admin/hardware"));
    CHECK(hardware["radios"].is_array());
    CHECK(hardware["drivers"].is_array());
    // Started by hand, as here: nothing would start it again.
    CHECK(!hardware["can_restart"].boolean(true));
    CHECK(hardware["restart_note"].string().find("by hand") != std::string::npos);
    std::string reply = admin.request("POST", "/api/admin/restart", "{}");
    CHECK(reply.find("409") != std::string::npos);
    CHECK(admin.request("GET", "/api/admin/session").find("200 OK") != std::string::npos);

    // Under a supervisor: answered, then the receiver stops for it to restart.
    ::setenv("FERNSDR_SUPERVISED", "1", 1);
    CHECK(json_body(admin.request("GET", "/api/admin/hardware"))["can_restart"].boolean(false));
    reply = admin.request("POST", "/api/admin/restart", "{}");
    CHECK(reply.find("200 OK") != std::string::npos);
    ::unsetenv("FERNSDR_SUPERVISED");
    if (harness.thread.joinable()) harness.thread.join();
    CHECK(admin.request("GET", "/api/admin/session").empty());
}

TEST_CASE(admin_changes_its_password_from_the_panel_without_sending_either) {
    using fernsdr::Json;
    const std::string old_password = "test admin old password";
    const std::string new_password = "test admin brand new password";
    char directory[] = "/tmp/fernsdr-password-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fernsdr.conf";
    {
        Harness harness(15000, 120000,
                        "[admin]\n# the panel's password\npassword_hash = " + fernsdr::hash_password(old_password, 1000) +
                            "\nsession_hours = 8\n",
                        path);
        CHECK(harness.ok);
        if (!harness.ok) return;
        AdminClient admin;
        CHECK(admin.sign_in(harness.port(), old_password));
        // What the page sends: a proof of the old password against a fresh
        // challenge, and the new hash it made itself.
        const auto change = [&](const std::string& current, const std::string& hash) {
            const Json challenge =
                json_body(TestClient(harness.port()).http_request("POST", "/api/admin/challenge", "", "", admin.host));
            uint8_t key[32], mac[32];
            fernsdr::pbkdf2_sha256(current, challenge["salt"].string(), static_cast<int>(challenge["iterations"].number()),
                                   key, sizeof(key));
            const std::string nonce = challenge["nonce"].string();
            fernsdr::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(), mac);
            Json body = Json::make_object();
            body.set("nonce", nonce);
            body.set("proof", fernsdr::to_hex(mac, sizeof(mac)));
            body.set("hash", hash);
            return admin.request("POST", "/api/admin/password", body.serialize());
        };
        const std::string new_hash = fernsdr::hash_password(new_password, 100000);
        // The wrong current password, or a weak hash, changes nothing.
        CHECK(change("not the password at all", new_hash).find("403") != std::string::npos);
        CHECK(change(old_password, fernsdr::hash_password(new_password, 1000)).find("400") != std::string::npos);
        const std::string reply = change(old_password, new_hash);
        CHECK(reply.find("200 OK") != std::string::npos);
        CHECK(reply.find(new_password) == std::string::npos);
        // Every session ended; the new password signs in, the old one not.
        CHECK(!json_body(admin.request("GET", "/api/admin/session"))["authorised"].boolean(true));
        CHECK(admin.request("GET", "/api/admin/state").find("401") != std::string::npos);
        AdminClient again;
        CHECK(!again.sign_in(harness.port(), old_password));
        CHECK(again.sign_in(harness.port(), new_password));
    }
    std::string text;
    CHECK(fernsdr::read_text_file(path, text));
    // Written where the old hash was, the comment and the rest kept.
    CHECK(text.find("# the panel's password\npassword_hash = pbkdf2$100000$") != std::string::npos);
    CHECK(text.find("session_hours = 8") != std::string::npos);
    ::unlink(path.c_str());
    ::unlink((std::string(directory) + "/fernsdr-settings.json").c_str());
    ::rmdir(directory);
}
