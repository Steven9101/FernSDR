// Glues the network server to the radio: one Session per WebSocket
// connection, plus the small HTTP API used for status pages and monitoring.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "../net/server.h"
#include "../update/service.h"
#include "../update/updater.h"
#include "admin.h"
#include "directory.h"
#include "space_weather.h"
#include "radio.h"
#include "session.h"

namespace fernsdr {

class Application : public ServerHandler {
public:
    Application(Radio& radio, const Config& config);

    // The server is constructed after the application, so it is wired in
    // afterwards.  Output is pushed by walking the server's live connections,
    // never by holding a reference to one.
    void set_server(Server* server) { server_ = server; }

    // Kept so the admin panel can show what the receiver was started with.
    void set_server_config(ServerConfig config) { server_config_ = std::move(config); }

    bool on_connect(Connection& connection) override;
    void on_text(Connection& connection, const std::string& text) override;
    void on_disconnect(Connection& connection) override;
    void on_before_write(Connection& connection) override;
    void on_flush() override;
    void on_tick() override;
    /** What a report to sdr-list.xyz says now; network thread only. */
    DirectoryReport listing_report();
    DirectoryListing& directory() { return directory_; }
    /** How long after the listing is switched on the first report goes out; tests shorten it. */
    void set_listing_delay_ms(int64_t ms) { listing_delay_ms_ = ms; }
    // For tests: how long a new version on trial serves before saying it
    // works, a minute otherwise.
    void set_update_settle_ms(int64_t ms) { update_settle_ms_ = ms; }
    // For tests: what the Updates page talks to.
    void set_update_service(std::unique_ptr<UpdateService> service) { updates_ = std::move(service); }
    bool on_http(Connection& connection, const HttpRequest& request, std::string& response) override;
    bool may_upload(Connection& connection, const HttpRequest& head) override;

    // Readable from any thread: the map itself belongs to the network thread.
    int session_count() const { return session_count_.load(std::memory_order_relaxed); }

private:
    Session* session_for(Connection& connection);
    void pump(Connection& connection);
    std::string status_json() const;

    // --- admin ---
    bool handle_admin(Connection& connection, const HttpRequest& request, std::string& response);
    /** /api/admin/modules and below, once the caller is signed in. */
    void handle_modules(const HttpRequest& request, std::string& response);
    /** The waterfall archive. Public or admin-only, as the operator chose. */
    bool handle_history(const HttpRequest& request, std::string& response);
    bool handle_decodes(const HttpRequest& request, std::string& response);

    /**
     * Writes an uploaded image into the document root and returns the path it
     * is served from.
     *
     * The type comes from the bytes rather than from anything the caller
     * says, and the name from a hash of the content rather than from anything
     * the caller sends: between them there is no filename to sanitise, no
     * extension to spoof, and the same image uploaded twice is one file.
     */
    /** The pictures on this receiver, as JSON, newest first by nothing at all. */
    /** The receiver in Prometheus' text format, for whoever is watching it. */
    std::string metrics_text() const;
    std::string uploads_json() const;
    std::string backup_json() const;
    // Plays a backup back; `result` says which modules to install again and
    // how many pictures could not be restored.
    bool restore_backup(const Json& backup, Json& result, std::string& error);
    bool upload_in_use(const std::string& name) const;
    bool delete_upload(const std::string& name, std::string& error);

    bool store_upload(const std::string& body, std::string& url, std::string& error) const;
    // Pushes the new theme to everyone already listening, so an operator's
    // change lands on open pages rather than on the next reload.
    void broadcast_theme();
    void broadcast_station();
    // Brings the bands in line with their hours and moves the listeners of
    // any that went off the air to the band now on its input.
    void follow_schedule();
    void broadcast_control(const std::string& text);
    std::string admin_state_json() const;
    bool disconnect_session(uint64_t id);

    std::unique_ptr<AdminAuth> admin_;
    ServerConfig server_config_;
    // Reports to sdr-list.xyz while the station asks to be listed there.
    DirectoryListing directory_;
    // Fetches space weather while the page has the widget for it.
    SpaceWeather space_weather_;
    void refresh_space_weather();
    const std::string instance_id_;
    int64_t listing_delay_ms_ = DirectoryListing::kFirstReportMs;
    // Set when this program is the new version of an update on trial.
    std::unique_ptr<UpdateTrial> update_trial_;
    int64_t update_settle_ms_ = 60 * 1000;
    // The Updates page: absent when the configuration has no directory.
    std::unique_ptr<UpdateService> updates_;
    std::string listing_problem_;

    Radio& radio_;
    /** Frames not sent because the socket was too far behind to want them. */
    uint64_t stale_frames_ = 0;
    Server* server_ = nullptr;
    int max_users_;
    uint64_t next_session_id_ = 1;
    int64_t last_band_status_ms_ = 0;
    // When the receiver stops so that its service starts it again, after
    // the panel asked; 0 while nobody has.
    int64_t restart_at_ms_ = 0;
    std::atomic<bool> restored_{false};
    std::string last_band_status_;
    std::map<uint64_t, std::unique_ptr<Session>> sessions_;
    std::atomic<int> session_count_{0};  // sessions_.size(), kept alongside
};

}  // namespace fernsdr
