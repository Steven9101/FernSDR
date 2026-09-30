#include "app.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "hardware.h"
#include "protocol.h"
#include "../util/json.h"
#include "../util/log.h"
#include "../util/password.h"
#include "../util/sha1.h"
#include "../version.h"

namespace fernsdr {

namespace {

// What stops the station being listed, in words for the admin panel; empty
// when nothing does.
std::string listing_problem(const SiteInfo& site) {
    if (site.grid_square.empty()) return "add the grid square, such as JO62";
    if (!valid_grid_locator(site.grid_square)) return "the grid square must look like JO62 or JO62qm";
    if (site.public_host.empty()) return "add the address listeners use, such as sdr.example.org";
    if (!valid_public_host(site.public_host)) {
        return "the public address must be a host name or IPv4 address, without http:// or a port";
    }
    if (site.public_port < 0 || site.public_port > 65535) {
        return "the public port must be between 1 and 65535, or 0 for the receiver's own";
    }
    return "";
}

}  // namespace

Application::Application(Radio& radio, const Config& config)
    : instance_id_(random_hex(8)), radio_(radio), max_users_(radio.site().max_users) {
    const ConfigSection& section = config.section("admin");
    AdminConfig admin;
    admin.password_hash = section.get("password_hash", "");
    admin.session_hours = static_cast<int>(section.get_int("session_hours", 8));
    admin.config_path = config.path();
    admin.home_network = section.get_bool("home_network", false);
    admin.plain_http_anywhere = section.get_bool("plain_http_anywhere", false);
    // Enabled only with a password hash present: an admin panel with a default
    // password is a back door with a login page in front of it.
    admin.enabled = section.get_bool("enabled", true) && !admin.password_hash.empty();
    if (!section.get("password", "").empty()) {
        LOG_WARN("admin",
                 "[admin] password= is ignored: store a hash instead. "
                 "Run 'fernsdr --set-password %s' to set one.", admin.config_path.c_str());
    }
    if (admin.enabled) {
        LOG_INFO("admin", "admin panel enabled at /admin%s",
                 admin.home_network ? ", over plain HTTP from the home network too" : "");
        if (admin.plain_http_anywhere) {
            LOG_WARN("admin",
                     "plain_http_anywhere = yes: the admin panel answers plain HTTP from the internet. Anyone on "
                     "the path can read it and change the page to catch the password. Use HTTPS instead.");
        }
    } else if (!section.get("password_hash", "").empty()) {
        LOG_INFO("admin", "admin panel disabled by config");
    } else {
        LOG_INFO("admin", "no admin password, so no admin panel: run 'fernsdr --set-password %s' to set one",
                 admin.config_path.c_str());
    }
    admin_ = std::make_unique<AdminAuth>(std::move(admin));

    // After an update, whether this program is the new version on trial: the
    // updater's record says so, in /var/lib/fernsdr-update (FERNSDR_UPDATE_DIR
    // in tests), and the request it answered is beside the configuration.
    const std::string& path = config.path();
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        updates_ = std::make_unique<UpdateService>(UpdateService::system(path.substr(0, slash)));
        const char* update_directory = std::getenv("FERNSDR_UPDATE_DIR");
        update_trial_ = std::make_unique<UpdateTrial>(
            path.substr(0, slash), update_directory && *update_directory ? update_directory : UpdateLayout::standard().update,
            kVersion);
        if (update_trial_->on_trial()) {
            LOG_INFO("update", "%s is on trial: it says it works once it has served a minute with every band that ran "
                               "before",
                     kVersion);
        } else {
            update_trial_.reset();
        }
    }
    refresh_space_weather();
}

bool Application::may_upload(Connection& connection, const HttpRequest& head) {
    // The upload itself is still checked in full once it has arrived; this
    // only decides whether the body may be sent at all.
    return admin_ && admin_->enabled() &&
           admin_transport_allowed(head, connection.peer_address(), admin_->config().home_network,
                                   admin_->config().plain_http_anywhere) &&
           admin_->authorised(head);
}

bool Application::on_connect(Connection& connection) {
    if (static_cast<int>(sessions_.size()) >= max_users_) {
        LOG_INFO("app", "rejecting %s: at capacity (%d users)", connection.remote_address().c_str(),
                 max_users_);
        return false;
    }
    const int per_address = radio_.site().max_users_per_address;
    if (per_address > 0 && server_) {
        const std::string network = network_key(connection.remote_address());
        // An IPv6 /48 may hold four times as many, as the connection limit
        // allows it: one household with several /64s is normal, and a /48 is
        // what a tunnel broker hands anyone for free.
        const std::string wide = wide_network_key(connection.remote_address());
        int from_there = 0, from_wide = 0;
        server_->for_each_connection([&](Connection& other) {
            if (!other.user_data) return;
            if (network_key(other.remote_address()) == network) from_there++;
            if (!wide.empty() && wide_network_key(other.remote_address()) == wide) from_wide++;
        });
        if (from_there >= per_address || from_wide >= 4 * per_address) {
            // Said plainly in the log, because the usual cause is not a greedy
            // listener but a proxy missing from trusted_proxies, which makes
            // everyone arrive from the proxy's own address.
            LOG_WARN("app",
                     "rejecting %s: %d listeners from %s already (max_users_per_address); if that is a "
                     "proxy, list it in trusted_proxies",
                     connection.remote_address().c_str(), from_there, network.c_str());
            connection.close(1013, "too many listeners from this address");
            return false;
        }
    }

    auto session = std::make_unique<Session>(next_session_id_++, radio_);
    Session* raw = session.get();
    raw->set_address(connection.remote_address());
    connection.user_data = raw;
    sessions_.emplace(raw->id(), std::move(session));
    session_count_.store(static_cast<int>(sessions_.size()), std::memory_order_relaxed);

    raw->begin();
    pump(connection);
    LOG_INFO("app", "user %llu connected from %s (%d online)", static_cast<unsigned long long>(raw->id()),
             connection.remote_address().c_str(), session_count());
    return true;
}

void Application::on_text(Connection& connection, const std::string& text) {
    Session* session = session_for(connection);
    if (!session) return;
    session->handle_text(text);
    pump(connection);
}

// Moves one session's queued output onto its connection.  Only ever called
// with a connection the server currently holds, which is what makes the
// lifetime safe.
void Application::pump(Connection& connection) {
    Session* session = session_for(connection);
    if (!session) return;
    const size_t backlog = connection.transport_backlog();
    session->set_transport_backlog(backlog, connection.queue_delay_ms());

    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> binaries;
    session->collect(texts, binaries);
    for (const auto& text : texts) connection.send_text(text);

    // Keep a byte bound as well as the age bound enforced before writes.
    // Older clients do not understand an audio discontinuity, so their audio
    // still uses this admission limit while their waterfall can expire.
    const bool discard_media = connection.pending_bytes() > 16 * 1024;
    auto& delivery = session->delivery();
    for (auto& payload : binaries) {
        // Waterfall rows are the bulk of a slow link's queue, and the one
        // thing that can wait: audio queued behind them goes ahead.
        const bool bulk = !payload.empty() && payload[0] == proto::kStreamWaterfall;
        if (delivery.prepare(payload, discard_media)) connection.send_binary(payload, bulk);
        else stale_frames_++;
    }
    if (delivery.needs_keyframe()) session->request_waterfall_keyframe();
}

void Application::on_before_write(Connection& connection) {
    const int64_t now = monotonic_ms();
    // Most writes have no old frames. Check before building the pruning
    // callback, whose captures may otherwise allocate for every audio packet.
    if (!connection.has_expired_binary(now, StreamDelivery::kMaxQueueAgeMs)) return;
    Session* session = session_for(connection);
    if (!session) return;
    auto& delivery = session->delivery();
    StreamDelivery::Repair repair;
    bool expired_media = false;
    stale_frames_ += connection.prune_binary(now, StreamDelivery::kMaxQueueAgeMs,
        [&](uint8_t* payload, size_t size, int64_t age_ms) {
            const bool expired = age_ms >= StreamDelivery::kMaxQueueAgeMs;
            const bool keep = delivery.retain(payload, size, expired, repair);
            expired_media |= expired && !keep;
            return keep;
        });
    delivery.finish_expiry(repair);
    if (delivery.needs_keyframe()) session->request_waterfall_keyframe();
    if (expired_media) session->note_media_expiry();
}

void Application::on_disconnect(Connection& connection) {
    Session* session = session_for(connection);
    if (!session) return;
    const uint64_t id = session->id();
    connection.user_data = nullptr;
    sessions_.erase(id);
    session_count_.store(static_cast<int>(sessions_.size()), std::memory_order_relaxed);
    LOG_INFO("app", "user %llu disconnected (%d online)", static_cast<unsigned long long>(id),
             session_count());
}

void Application::on_flush() {
    if (!server_) return;
    server_->for_each_connection([this](Connection& connection) { pump(connection); });
}

void Application::follow_schedule() {
    const int64_t utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    if (radio_.apply_schedule(utc_ms).empty() || !server_) return;
    server_->for_each_connection([this](Connection& connection) {
        Session* session = session_for(connection);
        if (!session) return;
        session->follow_schedule();
        pump(connection);
    });
}

void Application::on_tick() {
    if (!server_) return;

    const int64_t now = monotonic_ms();
    if (restart_at_ms_ > 0 && now >= restart_at_ms_) {
        restart_at_ms_ = 0;
        server_->stop();
        return;
    }
    const SiteInfo& site = radio_.site();
    const std::string problem = site.sdr_list ? listing_problem(site) : "";
    // Said once in the log, for a receiver set up in the file whose panel,
    // and the reason it shows, may be switched off.
    if (problem != listing_problem_) {
        if (!problem.empty()) LOG_WARN("directory", "not listed on sdr-list.xyz: %s", problem.c_str());
        listing_problem_ = problem;
    }
    directory_.set_enabled(site.sdr_list && problem.empty(), now, listing_delay_ms_);
    directory_.tick(now, [this] { return directory_report_json(listing_report()); });
    if (now - last_band_status_ms_ >= 1000) {
        last_band_status_ms_ = now;
        follow_schedule();
        if (update_trial_) {
            // A band its hours took off the air since the update was asked
            // for counts as working: it was stopped on purpose.
            std::vector<std::string> running;
            for (const auto& band : radio_.bands()) {
                if (band->online() || !band->on_air()) running.push_back(band->id());
            }
            if (update_trial_->tick(now, running, update_settle_ms_.load(std::memory_order_relaxed))) {
                LOG_INFO("update", "%s works: the updater can keep it", kVersion);
                update_trial_.reset();
            }
        }
        Json status = Json::make_object();
        status.set("type", "band-status");
        Json bands = Json::make_array();
        for (const auto& band : radio_.bands()) {
            Json entry = Json::make_object();
            entry.set("id", band->id());
            entry.set("listeners", band->listener_count());
            // Receiving, not merely started: a band whose dongle is unplugged
            // shows as down while it waits to try again.
            entry.set("running", band->online());
            entry.set("history", band->history_access());
            entry.set("on_air", band->on_air());
            entry.set("next_change", static_cast<double>(band->next_change_ms()));
            bands.push_back(entry);
        }
        status.set("bands", bands);
        const std::string text = status.serialize();
        if (text != last_band_status_) {
            last_band_status_ = text;
            // Counts belong to a named band, not to whichever band a browser
            // happens to show when a meter arrives. Build once for all users
            // and send only changes; the audio meter carries the site total.
            broadcast_control(text);
        }
    }

    // Collect anything posted to the chat this tick, then fan it out. Done in
    // two passes because a session must never reach into another session; the
    // application owns the connection list and is the only thing that may.
    std::vector<ChatMessage> posted;
    const int timeout_minutes = radio_.site().listener_timeout_minutes;
    const int64_t steady_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
    server_->for_each_connection([this, &posted, timeout_minutes, steady_now](Connection& connection) {
        Session* session = session_for(connection);
        if (!session) return;
        ChatMessage message;
        if (session->take_chat(message)) posted.push_back(message);
        session->tick();
        pump(connection);
        if (session->check_inactivity(timeout_minutes * 60000LL, steady_now) == Session::Inactivity::Expired) {
            LOG_INFO("app", "user %llu disconnected after %d minute%s without activity",
                     static_cast<unsigned long long>(session->id()), timeout_minutes, timeout_minutes == 1 ? "" : "s");
            connection.close(4001, "no activity for " + std::to_string(timeout_minutes) +
                                       (timeout_minutes == 1 ? " minute" : " minutes"));
        }
    });

    for (const ChatMessage& message : posted) {
        Json out = Json::make_object();
        out.set("type", "chat");
        out.set("id", static_cast<double>(message.id));
        out.set("name", message.name);
        out.set("text", message.text);
        out.set("at", static_cast<double>(message.at_ms));
        const std::string text = out.serialize();
        broadcast_control(text);
    }
}

Session* Application::session_for(Connection& connection) {
    return static_cast<Session*>(connection.user_data);
}

namespace {


std::string json_response(int status, const std::string& body, bool keep_alive,
                          std::vector<std::pair<std::string, std::string>> headers = {}) {
    headers.push_back({"Cache-Control", "no-store"});
    return build_http_response(status, "application/json; charset=utf-8", body, headers, keep_alive);
}

/** One value from a query string, percent-decoded, or `fallback`. */
std::string query_value(const std::string& query, const std::string& key,
                        const std::string& fallback = "") {
    size_t position = 0;
    while (position < query.size()) {
        const size_t amp = query.find('&', position);
        const std::string item = query.substr(position, amp == std::string::npos ? std::string::npos
                                                                                 : amp - position);
        const size_t equals = item.find('=');
        if (equals != std::string::npos && item.substr(0, equals) == key) {
            std::string decoded;
            if (!percent_decode(item.substr(equals + 1), decoded)) return fallback;
            return decoded;
        }
        if (amp == std::string::npos) break;
        position = amp + 1;
    }
    return fallback;
}

/**
 * Why the panel cannot restart the receiver, or empty when it can: only
 * where something starts it again once it exits, systemd (the shipped unit
 * has Restart=always) or fernsdr --supervise. A receiver started by hand,
 * or a container whose restart policy nobody here knows, would stay down.
 */
std::string restart_refusal() {
    if (std::getenv("INVOCATION_ID") || std::getenv("FERNSDR_SUPERVISED")) return "";
    if (std::getenv("FERNSDR_CONTAINER")) return "This receiver runs in a container: restart the container.";
    return "Nothing would start this receiver again, since it was started by hand: restart it the way it was "
           "started.";
}

/** Wall-clock milliseconds, which is what a mute's expiry is measured in. */
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string json_error(const std::string& message) {
    Json out = Json::make_object();
    out.set("error", message);
    return out.serialize();
}

}  // namespace

bool Application::handle_history(Connection& connection, const HttpRequest& request, std::string& response) {
    if (request.path != "/api/history") return false;

    const std::string band_id = query_value(request.query, "band");
    Band* band = radio_.band(band_id);
    if (!band) {
        response = json_response(404, json_error("no such band"), request.keep_alive());
        return true;
    }

    const std::string access = band->history_access();
    if (access == "off") {
        response =
            json_response(404, json_error("this band keeps no history"), request.keep_alive());
        return true;
    }
    // Up to a megabyte an answer for a request of a hundred bytes, read from
    // disk: a budget per address (an IPv6 /48 as one), which a page looking
    // back through a day never meets, and a script asking without pause does
    // within seconds. The operator is not held to it.
    const bool operator_asks = admin_ && admin_->enabled() && admin_->authorised(request);
    const std::string wide = wide_network_key(connection.remote_address());
    const std::string who = wide.empty() ? network_key(connection.remote_address()) : wide;
    const int64_t steady = monotonic_ms();
    // Addresses whose budget has filled again are forgotten, before this one
    // is looked up, so the map holds only those still paying off.
    if (history_budgets_.size() > 4096) {
        for (auto it = history_budgets_.begin(); it != history_budgets_.end();) {
            const int64_t refilled = it->second.bytes + (steady - it->second.updated_ms) * kHistoryBytesPerMs;
            it = refilled >= kHistoryBurstBytes ? history_budgets_.erase(it) : std::next(it);
        }
    }
    HistoryBudget& budget = history_budgets_[who];
    if (budget.updated_ms == 0) budget = {kHistoryBurstBytes, steady};
    budget.bytes = std::min(kHistoryBurstBytes, budget.bytes + (steady - budget.updated_ms) * kHistoryBytesPerMs);
    budget.updated_ms = steady;
    if (!operator_asks && budget.bytes <= 0) {
        response = build_http_response(429, "application/json", json_error("too much history too quickly; wait a moment"),
                                       {{"Retry-After", "5"}, {"Cache-Control", "no-store"}}, request.keep_alive());
        return true;
    }
    // "private" means the operator wants the record kept but not published, so
    // it is behind the same door as the rest of the admin panel and a link to
    // it is worth nothing on its own.
    if (access == "private" && !(admin_ && admin_->enabled() && admin_->authorised(request))) {
        response = json_response(403, json_error("this receiver's history is not public"),
                                 request.keep_alive());
        return true;
    }

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto timestamp = [](const std::string& text, int64_t& value) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= 0;
    };
    int64_t to_ms = 0;
    int64_t from_ms = 0;
    if (!timestamp(query_value(request.query, "to", std::to_string(now_ms)), to_ms) ||
        !timestamp(query_value(request.query, "from", std::to_string(std::max<int64_t>(0, to_ms - 3600000))), from_ms) ||
        from_ms > to_ms) {
        response = json_response(400, json_error("from and to must be ordered Unix timestamps in milliseconds"),
                                 request.keep_alive());
        return true;
    }

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    size_t bins = 0;
    double row_ms = 0;
    // What the page can show: rows for the picture's height and bins for its
    // width. Optional, bounded, and never more than the usual limit.
    const auto small_count = [&](const char* key) -> size_t {
        const std::string text = query_value(request.query, key, "0");
        if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return 0;
        }
        return static_cast<size_t>(std::stoul(text));
    };
    const size_t max_rows = small_count("rows");
    const size_t width = small_count("width");
    if (!band->read_history(from_ms, to_ms, rows, times, bins, &row_ms, max_rows, width)) {
        response =
            json_response(500, json_error("the history could not be read"), request.keep_alive());
        return true;
    }

    // The rows go out as bytes rather than as JSON numbers: an hour of a
    // thousand-bin archive is 3.6 MB raw and about four times that as text,
    // over a link that is already carrying audio to everyone else. A
    // length-prefixed JSON header describes what follows it.
    Json out = Json::make_object();
    out.set("band", band_id);
    out.set("bins", static_cast<double>(bins));
    out.set("low_hz", band->spectrum_low_hz());
    out.set("high_hz", band->spectrum_high_hz());
    out.set("row_interval_ms", row_ms);
    const auto info = band->info();
    out.set("oldest_ms", static_cast<double>(info.history_oldest_ms));
    out.set("newest_ms", static_cast<double>(info.history_newest_ms));
    out.set("floor_db", static_cast<double>(WaterfallArchive::kFloorDb));
    out.set("ceiling_db", static_cast<double>(WaterfallArchive::kCeilingDb));
    Json stamps = Json::make_array();
    for (int64_t time : times) stamps.push_back(Json(static_cast<double>(time)));
    out.set("times", stamps);
    const std::string header = out.serialize();

    std::string body;
    body.reserve(4 + header.size() + rows.size());
    const uint32_t header_length = static_cast<uint32_t>(header.size());
    for (int shift = 0; shift < 32; shift += 8) {
        body.push_back(static_cast<char>((header_length >> shift) & 0xff));
    }
    body += header;
    body.append(reinterpret_cast<const char*>(rows.data()), rows.size());

    if (!operator_asks) budget.bytes -= static_cast<int64_t>(body.size());
    response = build_http_response(200, "application/octet-stream", body,
                                   {{"Cache-Control", "no-store"}}, request.keep_alive());
    return true;
}

bool Application::handle_decodes(const HttpRequest& request, std::string& response) {
    if (request.path != "/api/decodes") return false;
    if (request.method != "GET") {
        response = json_response(405, json_error("GET only"), request.keep_alive());
        return true;
    }
    // The operator sees every decoder's; a listener only those the operator
    // made public. A receiver that publishes none answers as if it had none,
    // so the endpoint says nothing about decoders kept private.
    const bool operator_asks = admin_ && admin_->enabled() && admin_->authorised(request);
    // A copy, so the filter below runs inside the store's lock without
    // taking the decoders' lock as well.
    const std::map<std::string, uint64_t> listed = radio_.public_decoders();
    if (!operator_asks && listed.empty()) {
        response = json_response(404, json_error("this receiver publishes no decodes"), request.keep_alive());
        return true;
    }
    const auto number = [&](const char* key, uint64_t fallback, uint64_t& value) {
        const std::string text = query_value(request.query, key);
        if (text.empty()) {
            value = fallback;
            return true;
        }
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };
    uint64_t since = 0;
    uint64_t limit = 500;
    if (!number("since", 0, since) || !number("limit", 500, limit) || limit < 1 || limit > 2000) {
        response = json_response(400, json_error("since must be a decode number and limit 1 to 2000"),
                                 request.keep_alive());
        return true;
    }
    const DecodeStore::Filter keep = [&](const Decode& decode) {
        if (operator_asks) return true;
        const auto found = listed.find(decode.decoder);
        return found != listed.end() && decode.sequence > found->second;
    };
    uint64_t through = 0;
    const std::vector<Decode> decodes = query_value(request.query, "since").empty()
                                            ? radio_.decodes().latest(limit, keep, through)
                                            : radio_.decodes().since(since, limit, keep, through);
    Json list = Json::make_array();
    for (const Decode& decode : decodes) list.push_back(decode.to_json());
    Json out = Json::make_object();
    out.set("epoch", radio_.decodes().epoch());
    out.set("through", static_cast<double>(through));
    out.set("decodes", list);
    response = build_http_response(200, "application/json; charset=utf-8", out.serialize(),
                                   {{"Cache-Control", "no-store"}}, request.keep_alive());
    return true;
}

bool Application::handle_admin(Connection& connection, const HttpRequest& request,
                               std::string& response) {
    if (request.path.rfind("/api/admin/", 0) != 0) return false;
    const std::string& address = connection.remote_address();

    if (!admin_ || !admin_->enabled()) {
        response = json_response(404, json_error("the admin panel is not enabled on this receiver"),
                                 request.keep_alive());
        return true;
    }

    const std::string refusal =
        admin_transport_refusal(request, connection.peer_address(), admin_->config().home_network,
                                admin_->config().plain_http_anywhere);
    if (!refusal.empty()) {
        response = json_response(403, json_error(refusal), request.keep_alive());
        return true;
    }

    admin_->expire(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    const AdminCaller caller = direct_local_connection(request, connection.peer_address())
                                   ? AdminCaller::ThisMachine
                                   : AdminCaller::Remote;

    if (request.path == "/api/admin/challenge") {
        if (request.method != "POST") {
            response = json_response(405, json_error("POST only"), request.keep_alive());
            return true;
        }
        AdminChallenge challenge;
        int retry_after = 0;
        if (!admin_->issue_challenge(address, challenge, retry_after, caller)) {
            Json out = Json::make_object();
            out.set("error", "sign-in failed");
            if (retry_after > 0) out.set("retry_after", retry_after);
            response =
                json_response(retry_after > 0 ? 429 : 401, out.serialize(), request.keep_alive());
            return true;
        }
        Json out = Json::make_object();
        out.set("salt", challenge.salt);
        out.set("iterations", static_cast<double>(challenge.iterations));
        out.set("nonce", challenge.nonce);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/login") {
        if (request.method != "POST") {
            response = json_response(405, json_error("POST only"), request.keep_alive());
            return true;
        }
        Json body;
        if (!Json::parse(request.body, body)) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        int retry_after = 0;
        std::string token;
        if (body.has("proof")) {
            token = admin_->login_with_proof(body["nonce"].string(), body["proof"].string(), address,
                                             retry_after, caller);
        } else {
            response = json_response(
                400,
                json_error("send a proof from /api/admin/challenge"),
                request.keep_alive());
            return true;
        }
        if (token.empty()) {
            Json out = Json::make_object();
            // The same message whether the password was wrong or the account is
            // locked out, so a guess cannot be confirmed by the reply alone.
            out.set("error", "sign-in failed");
            if (retry_after > 0) out.set("retry_after", retry_after);
            response = json_response(retry_after > 0 ? 429 : 401, out.serialize(), request.keep_alive());
            return true;
        }

        // HttpOnly prevents reading the token, not using a logged-in browser.
        // SameSite restricts cross-site requests. Only a trusted proxy may
        // report TLS and enable Secure on the cookie.
        std::string cookie = "fernsdr_admin=" + token +
                             "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                             std::to_string(admin_->config().session_hours * 3600);
        if (request.secure) cookie += "; Secure";

        Json out = Json::make_object();
        out.set("ok", true);
        out.set("signing_context", AdminAuth::signing_context(token));
        response = json_response(200, out.serialize(), request.keep_alive(), {{"Set-Cookie", cookie}});
        return true;
    }

    if (request.path == "/api/admin/logout") {
        if (request.method != "POST" || !admin_->signature_valid(request, request.body)) {
            response = json_response(401, json_error("sign out with a signed POST request"), request.keep_alive());
            return true;
        }
        admin_->logout(request);
        Json out = Json::make_object();
        out.set("ok", true);
        response = json_response(200, out.serialize(), request.keep_alive(),
                                 {{"Set-Cookie", "fernsdr_admin=; HttpOnly; SameSite=Strict; "
                                                 "Path=/; Max-Age=0"}});
        return true;
    }

    if (request.path == "/api/admin/session") {
        Json out = Json::make_object();
        out.set("authorised", admin_->authorised(request));
        // Let in only because of plain_http_anywhere: the panel warns on
        // every page, the sign-in first of all.
        out.set("exposed", !admin_transport_allowed(request, connection.peer_address(), admin_->config().home_network));
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // Everything past here needs a session.
    if (!admin_->authorised(request)) {
        response = json_response(401, json_error("sign in first"), request.keep_alive());
        return true;
    }

    // Mutation signatures also bind the request to this particular session.
    if (request.method != "GET" && !admin_->signature_valid(request, request.body)) {
        response = json_response(
            401, json_error("this request was not signed; sign in again"), request.keep_alive());
        return true;
    }

    // Once a backup is restored, the files hold it and this receiver still
    // runs on what it read at its start: a change made now would be made to
    // the old settings and written over the backup. Installing the backup's
    // modules and restarting is what is left to do.
    if (restored_ && request.method != "GET" && request.path != "/api/admin/restart" &&
        request.path != "/api/admin/restore" && request.path != "/api/admin/logout" &&
        request.path.rfind("/api/admin/modules", 0) != 0) {
        response = json_response(409, json_error("a backup was restored; restart FernSDR to take it first"),
                                 request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/state") {
        response = json_response(200, admin_state_json(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/config") {
        if (request.method == "GET") {
            Json out = Json::make_object();
            out.set("path", admin_->config().config_path);
            // For the operator, not the public: an exact version tells a
            // stranger which published fixes this receiver is missing.
            out.set("version", kVersion);
            std::string text;
            if (!read_text_file(admin_->config().config_path, text)) {
                response = json_response(500, json_error("cannot read " + admin_->config().config_path),
                                         request.keep_alive());
                return true;
            }
            out.set("text", hide_admin_credentials(text));
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
        if (request.method == "POST") {
            Json body;
            if (!Json::parse(request.body, body)) {
                response = json_response(400, json_error("malformed request"), request.keep_alive());
                return true;
            }
            Config current;
            std::string error;
            if (!current.load(admin_->config().config_path, error)) {
                response = json_response(500, json_error(error), request.keep_alive());
                return true;
            }
            // The editor was sent the credentials hidden; what comes back
            // hidden is what the file already holds.
            const std::string text = restore_admin_credentials(body["text"].string(), current.section("admin"));

            // Parse and configure a throwaway Radio before touching the file.
            // Writing a config that will not start, and only finding out at the
            // next restart, is how a receiver goes quietly offline overnight.
            Config candidate;
            if (!candidate.parse(text, error)) {
                response = json_response(400, json_error(error), request.keep_alive());
                return true;
            }
            const std::string kept = machine_only_change(current, candidate);
            if (!kept.empty()) {
                response = json_response(400, json_error(kept), request.keep_alive());
                return true;
            }
            Radio probe;
            if (!probe.configure(candidate, error)) {
                response = json_response(400, json_error(error), request.keep_alive());
                return true;
            }
            if (!write_text_file(admin_->config().config_path, text, error, FileAccess::OwnerOnly)) {
                response = json_response(500, json_error(error), request.keep_alive());
                return true;
            }
            LOG_INFO("admin", "configuration written to %s", admin_->config().config_path.c_str());

            // Apply what can be applied to a running receiver, and be precise
            // about the rest. "Restart to apply" as a blanket answer sends an
            // operator to reboot a receiver full of listeners because they
            // changed the notice text.
            radio_.apply_site(candidate);
            max_users_ = radio_.site().max_users;
            // Hours apply at once: a band whose hours now have it off the
            // air stops and its listeners move, one coming on starts.
            radio_.apply_hours(candidate);
            follow_schedule();
            const std::vector<std::string> changed = radio_.bands_needing_restart(candidate);
            // Decoders take their sections at once: one that changed misses at
            // most the slot it was in.
            const DecoderChanges decoders = radio_.apply_decoders(candidate);
            // Listeners see a new notice, name, chat switch or public decoder
            // at once.
            broadcast_station();

            Json out = Json::make_object();
            out.set("ok", true);
            out.set("applied", true);
            Json bands = Json::make_array();
            for (const std::string& id : changed) bands.push_back(id);
            out.set("bands_changed", bands);
            Json decoder_ids = Json::make_array();
            for (const std::string& id : decoders.changed) decoder_ids.push_back(id);
            out.set("decoders_changed", decoder_ids);
            Json waiting = Json::make_object();
            for (const auto& [id, why] : decoders.waiting) waiting.set(id, why);
            out.set("decoders_waiting", waiting);
            // What a restart would actually apply, so the operator can weigh
            // it against dropping everyone listening to that band.
            Json details = Json::make_array();
            for (const auto& change : radio_.band_changes(candidate)) {
                Json entry = Json::make_object();
                entry.set("band", change.band);
                entry.set("key", change.key);
                entry.set("running", change.running);
                entry.set("configured", change.configured);
                entry.set("band_restart", change.band_restart);
                details.push_back(entry);
            }
            out.set("changes", details);
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
    }

    // A backup to move the receiver with: its configuration and the panel's
    // settings and look, the pictures it serves while they fit in what a
    // restore may send, and the modules to install again. The sections that
    // belong to the machine are left out, [admin] with the password's hash
    // among them, so the file signs nobody in.
    if (request.path == "/api/admin/backup" && request.method == "GET") {
        response = json_response(200, backup_json(), request.keep_alive());
        connection.allow_output(response.size());
        return true;
    }

    // A backup played back onto this receiver. The sections that belong to
    // this machine, [server], [modules] and [admin], stay as they are: the
    // new machine's address, its programs and its password. The rest must
    // pass what the configuration editor checks, so a backup can set no file
    // a stolen session could not. The receiver restarts to take it.
    if (request.path == "/api/admin/restore" && request.method == "POST") {
        Json body;
        std::string error;
        Json result;
        if (!Json::parse(request.body, body) || !body.is_object() || body["fernsdr_backup"].number(0) != 1) {
            response = json_response(400, json_error("that is not a FernSDR backup"), request.keep_alive());
        } else if (!restore_backup(body, result, error)) {
            response = json_response(400, json_error(error), request.keep_alive());
        } else {
            response = json_response(200, result.serialize(), request.keep_alive());
        }
        return true;
    }

    // A new admin password. The browser derives its key and makes the stored
    // hash itself, and proves the current password against a fresh
    // challenge, so neither password crosses the network, as at sign-in.
    // Written where the old hash was in the file, then every session ends:
    // the page signs in again with the new password.
    if (request.path == "/api/admin/password" && request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body) || !body.is_object()) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        const std::string hash = body["hash"].string();
        if (!AdminAuth::acceptable_password_hash(hash)) {
            response = json_response(400, json_error("the new password's hash is not one this receiver takes"),
                                     request.keep_alive());
            return true;
        }
        int retry_after = 0;
        if (!admin_->proof_valid(body["nonce"].string(), body["proof"].string(), address, retry_after, caller)) {
            Json out = Json::make_object();
            out.set("error", "the current password is not right");
            if (retry_after > 0) out.set("retry_after", retry_after);
            response = json_response(retry_after > 0 ? 429 : 403, out.serialize(), request.keep_alive());
            return true;
        }
        const std::string path = admin_->config().config_path;
        std::string text, error;
        Config written;
        if (!read_text_file(path, text)) {
            response = json_response(500, json_error("cannot read " + path), request.keep_alive());
            return true;
        }
        const std::string changed = with_admin_password_hash(text, hash);
        if (!written.parse(changed, error) || written.section("admin").get("password_hash") != hash ||
            !write_text_file(path, changed, error, FileAccess::OwnerOnly)) {
            response = json_response(500, json_error("the new password could not be written: " + error),
                                     request.keep_alive());
            return true;
        }
        admin_->replace_password_hash(hash);
        LOG_INFO("admin", "the admin password was changed from the panel");
        Json out = Json::make_object();
        out.set("ok", true);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // The radios plugged in, the TV drivers in their way, and whether the
    // receiver can be restarted from here: what setting up a radio needs.
    if (request.path == "/api/admin/hardware" && request.method == "GET") {
        Json out = Json::make_object();
        out.set("radios", usb_radios_json(find_usb_radios()));
        Json drivers = Json::make_array();
        for (const DriverInTheWay& driver : drivers_in_the_way()) {
            Json entry = Json::make_object();
            entry.set("driver", driver.driver);
            entry.set("radio", driver.radio);
            entry.set("module", driver.module);
            drivers.push_back(entry);
        }
        out.set("drivers", drivers);
        const std::string refusal = restart_refusal();
        out.set("can_restart", refusal.empty());
        if (!refusal.empty()) out.set("restart_note", refusal);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/restart" && request.method == "POST") {
        const std::string refusal = restart_refusal();
        if (!refusal.empty()) {
            response = json_response(409, json_error(refusal), request.keep_alive());
            return true;
        }
        // Answered first, then stopped at the next tick: the page learns the
        // restart was taken and waits for the receiver to come back.
        LOG_INFO("admin", "restarting the receiver at the operator's request");
        restart_at_ms_ = monotonic_ms() + 300;
        Json out = Json::make_object();
        out.set("ok", true);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/restart-band" && request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body)) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        const std::string id = body["band"].string();
        Json out = Json::make_object();
        Band* band = radio_.band(id);
        if (!band) {
            response = json_response(404, json_error("no band called " + id), request.keep_alive());
            return true;
        }
        // A module band takes the module settings saved since it started. The
        // file was checked when it was saved, and is read again here rather
        // than trusted from the request.
        const auto& built = band->configured_values();
        const auto source = built.find("source");
        const auto module = built.find("module");
        if (source != built.end() && source->second == "module" && module != built.end()) {
            Config file;
            std::string problem;
            const std::string name = "band:" + id;
            if (file.load(admin_->config().config_path, problem) && file.has_section(name) &&
                file.section(name).get("source") == "module" && file.section(name).get("module") == module->second &&
                !band->reconfigure_source(file.section(name), problem)) {
                response = json_response(400, json_error(problem), request.keep_alive());
                return true;
            }
        }
        // Once, not twice: restart() has a side effect, and asking it again to
        // build the message would start a second one.
        if (!band->on_air()) {
            out.set("ok", false);
            out.set("note", band->name() + " is " + band->status() + "; its hours are set on its page");
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
        const bool accepted = band->restart();
        out.set("ok", accepted);
        if (!accepted) out.set("note", "a restart is already in progress");
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/station") {
        if (request.method == "GET") {
            Json out = Json::make_object();
            out.set("station", radio_.site_json());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
        if (request.method == "POST") {
            Json body;
            if (!Json::parse(request.body, body)) {
                response = json_response(400, json_error("malformed request"), request.keep_alive());
                return true;
            }
            // Listing needs a grid square and an address; say which is
            // missing before saving, rather than listing nothing in silence.
            const Json& station = body["station"];
            SiteInfo next = radio_.site();
            if (station["sdr_list"].is_bool()) next.sdr_list = station["sdr_list"].boolean();
            if (station["grid"].is_string()) next.grid_square = station["grid"].string();
            if (station["public_host"].is_string()) next.public_host = station["public_host"].string();
            const std::string problem = next.sdr_list ? listing_problem(next) : "";
            if (!problem.empty()) {
                response = json_response(400, json_error("to be listed on sdr-list.xyz, " + problem),
                                         request.keep_alive());
                return true;
            }
            std::string error;
            if (!radio_.apply_site_json(station, error)) {
                response = json_response(400, json_error(error), request.keep_alive());
                return true;
            }
            max_users_ = radio_.site().max_users;
            // At once rather than at the next tick, so the page reading the
            // listing's state straight after saving sees the change.
            const SiteInfo& saved = radio_.site();
            directory_.set_enabled(saved.sdr_list && listing_problem(saved).empty(), monotonic_ms(),
                                   listing_delay_ms_);
            // Everyone already listening gets the new station details without
            // reloading, the same way a theme change reaches them.
            broadcast_station();
            LOG_INFO("admin", "station details updated");
            Json out = Json::make_object();
            out.set("ok", true);
            out.set("station", radio_.site_json());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
    }

    if (request.path == "/api/admin/directory" && request.method == "GET") {
        const SiteInfo& site = radio_.site();
        const DirectoryListing::Status status = directory_.status();
        const std::string problem = site.sdr_list ? listing_problem(site) : "";
        Json out = Json::make_object();
        out.set("enabled", site.sdr_list);
        out.set("state", problem.empty() ? status.state : std::string("off"));
        out.set("detail", problem.empty() ? status.detail : problem);
        out.set("listed_ms", static_cast<double>(status.listed_ms));
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/update" || request.path == "/api/admin/update/check" ||
        request.path == "/api/admin/update/start") {
        if (!updates_) {
            response = json_response(404, json_error("this receiver's configuration has no directory to update from"),
                                     request.keep_alive());
            return true;
        }
        std::string error;
        bool ok = true;
        if (request.path == "/api/admin/update/check" && request.method == "POST") {
            ok = updates_->check(error);
        } else if (request.path == "/api/admin/update/start" && request.method == "POST") {
            Json body;
            if (!Json::parse(request.body, body) || !body.is_object() || !body["version"].is_string()) {
                response = json_response(400, json_error("name the version to update to"), request.keep_alive());
                return true;
            }
            std::vector<std::string> running;
            for (const auto& band : radio_.bands()) {
                if (band->online()) running.push_back(band->id());
            }
            ok = updates_->start(body["version"].string(), running, error);
            if (ok) LOG_INFO("admin", "update to %s asked for", body["version"].string().c_str());
        } else if (request.method != "GET" || request.path != "/api/admin/update") {
            response = json_response(405, json_error("GET the state, POST to check or start"), request.keep_alive());
            return true;
        }
        response = ok ? json_response(request.method == "GET" ? 200 : 202, updates_->snapshot().serialize(),
                                      request.keep_alive())
                      : json_response(409, json_error(error), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/server" && request.method == "GET") {
        // Read-only, deliberately. These are read once when the socket is bound
        // and the threads are started; offering them as controls would be
        // offering a control that quietly does nothing.
        Json out = Json::make_object();
        Json values = Json::make_object();
        values.set("bind", server_config_.bind_address);
        values.set("port", static_cast<double>(server_config_.port));
        values.set("root", server_config_.document_root);
        values.set("websocket_path", server_config_.websocket_path);
        values.set("max_connections", static_cast<double>(server_config_.max_connections));
        values.set("idle_timeout", static_cast<double>(server_config_.idle_timeout_seconds));
        Json proxies = Json::make_array();
        for (const std::string& entry : server_config_.trusted_proxies) proxies.push_back(entry);
        values.set("trusted_proxies", proxies);
        out.set("server", values);
        out.set("config_path", admin_->config().config_path);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/decoders" && request.method == "GET") {
        Json out = Json::make_object();
        out.set("decoders", radio_.decoders_status());
        out.set("kept", static_cast<double>(radio_.decodes().size()));
        out.set("spots", radio_.spots_status());
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/decoders/restart" && request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body) || !radio_.restart_decoder(body["id"].string())) {
            response = json_response(404, json_error("no decoder with that id is running"), request.keep_alive());
            return true;
        }
        LOG_INFO("admin", "decoder '%s' restarted", body["id"].string().c_str());
        Json out = Json::make_object();
        out.set("ok", true);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/bands") {
        if (request.method == "GET") {
            Json out = Json::make_object();
            out.set("bands", radio_.bands_json());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
        if (request.method == "POST") {
            Json body;
            if (!Json::parse(request.body, body)) {
                response = json_response(400, json_error("malformed request"), request.keep_alive());
                return true;
            }
            std::string error;
            if (!radio_.apply_band_json(body["id"].string(), body["values"], error)) {
                response = json_response(400, json_error(error), request.keep_alive());
                return true;
            }
            LOG_INFO("admin", "band '%s' updated", body["id"].string().c_str());
            Json out = Json::make_object();
            out.set("ok", true);
            out.set("bands", radio_.bands_json());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
    }

    if (request.path == "/api/admin/theme") {
        if (request.method == "GET") {
            Json out = Json::make_object();
            out.set("theme", radio_.theme().snapshot());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
        if (request.method == "POST") {
            Json body;
            if (!Json::parse(request.body, body)) {
                response = json_response(400, json_error("malformed request"), request.keep_alive());
                return true;
            }
            std::string error;
            const bool ok = body["reset"].boolean(false) ? radio_.theme().reset(error)
                                                         : radio_.theme().replace(body["theme"], error);
            if (!ok) {
                response = json_response(400, json_error(error), request.keep_alive());
                return true;
            }
            broadcast_theme();
            LOG_INFO("admin", "appearance updated and pushed to %d listener(s)", session_count());
            Json out = Json::make_object();
            out.set("ok", true);
            out.set("theme", radio_.theme().snapshot());
            response = json_response(200, out.serialize(), request.keep_alive());
            return true;
        }
    }

    // What the receiver reads right now at one frequency, so a calibration
    // can be taken rather than worked out: feed in a known level, ask for
    // this, and the offset is the difference between them.
    if (request.path == "/api/admin/level" && request.method == "GET") {
        Band* band = radio_.band(query_value(request.query, "band"));
        if (!band) {
            response = json_response(404, json_error("no such band"), request.keep_alive());
            return true;
        }
        Json frequency, bandwidth;
        if (!Json::parse(query_value(request.query, "hz", "0"), frequency) || !frequency.is_number() ||
            !Json::parse(query_value(request.query, "width", "1000"), bandwidth) || !bandwidth.is_number() ||
            frequency.number() < band->low_hz() || frequency.number() > band->high_hz() ||
            bandwidth.number() <= 0 || bandwidth.number() > band->high_hz() - band->low_hz()) {
            response = json_response(400, json_error("choose a frequency and positive width inside this band"), request.keep_alive());
            return true;
        }
        const double hz = frequency.number();
        const double width = bandwidth.number();
        Json out = Json::make_object();
        out.set("hz", hz);
        out.set("dbfs", std::round(band->peak_dbfs(hz, width) * 10.0) / 10.0);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // The latest spectrum of a band, reduced for the panel's live view. A
    // separate endpoint rather than part of the state, because only a page
    // that draws it asks, and it asks more often than the state is polled.
    if (request.path == "/api/admin/spectrum" && request.method == "GET") {
        Band* band = radio_.band(query_value(request.query, "band"));
        if (!band) {
            response = json_response(404, json_error("no such band"), request.keep_alive());
            return true;
        }
        Json requested;
        size_t bins = 512;
        if (Json::parse(query_value(request.query, "bins", "512"), requested) && requested.is_number()) {
            bins = static_cast<size_t>(std::clamp(requested.number(), 32.0, 2048.0));
        }
        std::vector<float> levels;
        double low = 0.0, high = 0.0;
        Json out = Json::make_object();
        if (band->spectrum_snapshot(bins, levels, low, high)) {
            out.set("low", low);
            out.set("high", high);
            out.set("floor", std::round(band->info().noise_floor_dbfs * 10.0) / 10.0);
            Json values = Json::make_array();
            for (float level : levels) values.push_back(std::round(level * 2.0) / 2.0);
            out.set("levels", values);
        } else {
            out.set("levels", Json::make_array());
        }
        // Asked for once, when the panel opens a band: the last minute, as
        // bytes rather than as numbers, which would be ten times the size.
        if (query_value(request.query, "recent") == "1") {
            std::vector<uint8_t> rows;
            int64_t age_ms = -1;
            const size_t count = band->recent_spectrum(rows, age_ms);
            Json recent = Json::make_object();
            recent.set("width", static_cast<double>(Band::kRecentBins));
            recent.set("count", static_cast<double>(count));
            recent.set("interval_ms", static_cast<double>(Band::kRecentIntervalMs));
            recent.set("age_ms", static_cast<double>(age_ms));
            recent.set("floor_db", static_cast<double>(WaterfallArchive::kFloorDb));
            recent.set("ceiling_db", static_cast<double>(WaterfallArchive::kCeilingDb));
            recent.set("lines", base64_encode(rows.data(), rows.size()));
            out.set("recent", recent);
        }
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // Where a known carrier shows up on a band, for correcting its frequency
    // axis. One reading is instantaneous; the panel takes several and
    // averages them.
    if (request.path == "/api/admin/carrier" && request.method == "GET") {
        Band* band = radio_.band(query_value(request.query, "band"));
        if (!band) {
            response = json_response(404, json_error("no such band"), request.keep_alive());
            return true;
        }
        Json hz, span;
        const bool have_hz = Json::parse(query_value(request.query, "hz"), hz) && hz.is_number();
        const bool have_span = Json::parse(query_value(request.query, "span", "2000"), span) && span.is_number();
        if (!have_hz || !have_span || hz.number() < band->low_hz() || hz.number() > band->high_hz() ||
            span.number() < 50.0 || span.number() > 50000.0) {
            response = json_response(400, json_error("give a frequency on this band, and a span from 50 Hz to 50 kHz"),
                                     request.keep_alive());
            return true;
        }
        double found = 0.0;
        float level = -160.0f;
        Json out = Json::make_object();
        out.set("found", band->find_carrier(hz.number(), span.number(), found, level));
        if (out["found"].boolean()) {
            out.set("hz", std::round(found * 100.0) / 100.0);
            out.set("level_dbfs", std::round(level * 10.0) / 10.0);
        }
        out.set("floor_dbfs", std::round(band->info().noise_floor_dbfs * 10.0) / 10.0);
        out.set("ppm", std::round((band->rf_scale() - 1.0) * 1e9) / 1000.0);
        out.set("frequency_offset", band->frequency_offset_hz());
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // Uploading an image, so a background or a logo can be a file on the
    // operator's phone rather than a URL they have to host somewhere first.
    if (request.path == "/api/admin/upload" && request.method == "POST") {
        std::string url, problem;
        if (!store_upload(request.body, url, problem)) {
            response = json_response(400, json_error(problem), request.keep_alive());
            return true;
        }
        Json out = Json::make_object();
        out.set("url", url);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/log") {
        Json lines = Json::make_array();
        for (const std::string& line : LogRing::instance().snapshot()) lines.push_back(line);
        Json out = Json::make_object();
        out.set("lines", lines);
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/disconnect" && request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body)) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        const double number = body["id"].number();
        if (!body["id"].is_number() || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) {
            response = json_response(400, json_error("id must be a positive safe integer"), request.keep_alive());
            return true;
        }
        const uint64_t id = static_cast<uint64_t>(number);
        Json out = Json::make_object();
        out.set("ok", disconnect_session(id));
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    // The pictures on this receiver, and getting rid of one.
    //
    // Uploads are content-addressed, so replacing a background leaves the old
    // file on disk with nothing pointing at it. Without a way to see them an
    // operator's directory only ever grows, and the only way to find out what
    // is in it is to log in with a shell.
    if (request.path == "/api/admin/uploads" && request.method == "GET") {
        response = json_response(200, uploads_json(), request.keep_alive());
        return true;
    }

    if (request.path == "/api/admin/uploads/delete" && request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body)) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        std::string error;
        if (!delete_upload(body["name"].string(), error)) {
            response = json_response(400, json_error(error), request.keep_alive());
            return true;
        }
        response = json_response(200, uploads_json(), request.keep_alive());
        return true;
    }

    // Muting somebody, and lifting it. Both by address: a session ends when a
    // page is reloaded, and the point of a mute is that it does not.
    if ((request.path == "/api/admin/mute" || request.path == "/api/admin/unmute") &&
        request.method == "POST") {
        Json body;
        if (!Json::parse(request.body, body)) {
            response = json_response(400, json_error("malformed request"), request.keep_alive());
            return true;
        }
        const std::string who = body["address"].string();
        in6_addr parsed{};
        if (who.empty() || who.size() > 64 ||
            (::inet_pton(AF_INET, who.c_str(), &parsed) != 1 && ::inet_pton(AF_INET6, who.c_str(), &parsed) != 1)) {
            response = json_response(400, json_error("which address?"), request.keep_alive());
            return true;
        }
        const int64_t now = now_ms();
        if (request.path == "/api/admin/mute") {
            // Zero means until it is lifted. Anything longer than a week is a
            // ban with extra steps, and should be one.
            const double requested_minutes = body["minutes"].number(0);
            if (body.has("minutes") && (!body["minutes"].is_number() || requested_minutes < 0 ||
                                        std::floor(requested_minutes) != requested_minutes)) {
                response = json_response(400, json_error("minutes must be a non-negative whole number"), request.keep_alive());
                return true;
            }
            const int minutes = static_cast<int>(std::min(requested_minutes, 7 * 24 * 60.0));
            radio_.chat().mute(who, minutes, now);
            const std::string how_long =
                minutes > 0 ? " for " + std::to_string(minutes) + " min" : " until lifted";
            LOG_INFO("admin", "muted %s%s", who.c_str(), how_long.c_str());
        } else {
            radio_.chat().unmute(who);
            LOG_INFO("admin", "unmuted %s", who.c_str());
        }
        std::string error;
        if (!radio_.save_mutes(now, error)) {
            // The mute is in force either way; the operator needs to know it
            // will not survive a restart.
            response = json_response(500, json_error("muted, but not saved: " + error),
                                     request.keep_alive());
            return true;
        }
        Json out = Json::make_object();
        out.set("muted", radio_.chat().mutes_json(now));
        response = json_response(200, out.serialize(), request.keep_alive());
        return true;
    }

    if (request.path.rfind("/api/admin/modules", 0) == 0) {
        handle_modules(request, response);
        return true;
    }

    response = json_response(404, json_error("no such endpoint"), request.keep_alive());
    return true;
}

void Application::handle_modules(const HttpRequest& request, std::string& response) {
    ModuleManager* modules = radio_.modules();
    if (!modules) {
        response = json_response(503, json_error("modules are not available until the receiver has started"),
                                 request.keep_alive());
        return;
    }
    if (request.path == "/api/admin/modules" && request.method == "GET") {
        response = json_response(200, modules->snapshot(), request.keep_alive());
        return;
    }
    if (request.path == "/api/admin/modules/log" && request.method == "GET") {
        Band* band = radio_.band(query_value(request.query, "band"));
        if (!band) {
            response = json_response(404, json_error("no such band"), request.keep_alive());
            return;
        }
        Json out = Json::make_object();
        const Json details = band->source_details();
        out.set("log", details.is_object() && details["log"].is_array() ? details["log"] : Json::make_array());
        response = json_response(200, out.serialize(), request.keep_alive());
        return;
    }
    if (request.method != "POST") {
        response = json_response(405, json_error("POST only"), request.keep_alive());
        return;
    }
    Json body;
    if (!Json::parse(request.body, body) || !body.is_object()) {
        response = json_response(400, json_error("malformed request"), request.keep_alive());
        return;
    }
    std::string error;
    bool queued = false;
    if (request.path == "/api/admin/modules/refresh") {
        queued = modules->refresh(error);
    } else if (request.path == "/api/admin/modules/install") {
        queued = modules->install(body["repository"].string(), body["tag"].string(), body["asset"].string(),
                                  body["activate"].boolean(false), error);
    } else if (request.path == "/api/admin/modules/activate") {
        queued = modules->activate(body["id"].string(), body["version"].string(), error);
    } else if (request.path == "/api/admin/modules/enable") {
        if (!body["enabled"].is_bool()) {
            response = json_response(400, json_error("enabled must be true or false"), request.keep_alive());
            return;
        }
        queued = modules->set_enabled(body["id"].string(), body["enabled"].boolean(), error);
    } else if (request.path == "/api/admin/modules/remove") {
        queued = modules->remove(body["id"].string(), body["version"].string(), error);
    } else if (request.path == "/api/admin/modules/devices") {
        queued = modules->list_devices(body["id"].string(), error);
    } else if (request.path == "/api/admin/modules/set") {
        Band* band = radio_.band(body["band"].string());
        if (!band) {
            response = json_response(404, json_error("no such band"), request.keep_alive());
            return;
        }
        Json sent;
        if (!band->apply_source_settings(body["settings"], sent, error)) {
            response = json_response(400, json_error(error), request.keep_alive());
            return;
        }
        LOG_INFO("admin", "band '%s': sent %s", band->id().c_str(), sent["settings"].serialize().c_str());
        Json out = Json::make_object();
        out.set("ok", true);
        out.set("sent", sent);
        response = json_response(200, out.serialize(), request.keep_alive());
        return;
    } else {
        response = json_response(404, json_error("no such endpoint"), request.keep_alive());
        return;
    }
    if (!queued) {
        response = json_response(400, json_error(error), request.keep_alive());
        return;
    }
    // The job runs on its own thread; the panel follows it in the snapshot.
    response = json_response(202, modules->snapshot(), request.keep_alive());
}

/** Whether anything the operator has configured points at this file. */
bool Application::upload_in_use(const std::string& name) const {
    const std::string url = "/uploads/" + name;
    return radio_.theme().snapshot().serialize().find(url) != std::string::npos;
}

std::string Application::uploads_json() const {
    Json list = Json::make_array();
    uint64_t total = 0;
    DIR* directory = opendir(server_config_.uploads_root.c_str());
    if (directory) {
        while (dirent* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (!is_an_upload_name(name)) continue;
            struct stat info;
            const std::string path = server_config_.uploads_root + "/" + name;
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
            Json item = Json::make_object();
            item.set("name", name);
            item.set("url", "/uploads/" + name);
            item.set("bytes", static_cast<double>(info.st_size));
            item.set("modified", static_cast<double>(info.st_mtime) * 1000.0);
            item.set("in_use", upload_in_use(name));
            list.push_back(item);
            total += static_cast<uint64_t>(info.st_size);
        }
        closedir(directory);
    }
    Json out = Json::make_object();
    out.set("uploads", list);
    out.set("bytes", static_cast<double>(total));
    return out.serialize();
}

bool Application::delete_upload(const std::string& name, std::string& error) {
    if (!is_an_upload_name(name)) {
        error = "that is not the name of a picture on this receiver";
        return false;
    }
    if (upload_in_use(name)) {
        // Deleting it would leave a page pointing at a 404, and the operator
        // would find out from a listener rather than from here.
        error = "that picture is in use; take it off the page first";
        return false;
    }
    const std::string path = server_config_.uploads_root + "/" + name;
    if (std::remove(path.c_str()) != 0) {
        error = "could not delete it";
        return false;
    }
    LOG_INFO("admin", "deleted upload %s", name.c_str());
    return true;
}

bool Application::store_upload(const std::string& body, std::string& url,
                               std::string& error) const {
    if (server_config_.document_root.empty()) {
        error = "this receiver serves no files, so it cannot hold an upload";
        return false;
    }
    // Big enough for a photograph off a phone, small enough that a receiver
    // on a domestic uplink is not asked to serve something absurd behind
    // every page load.
    constexpr size_t kMaxBytes = 8 * 1024 * 1024;
    if (body.size() < 16) {
        error = "that file is empty";
        return false;
    }
    if (body.size() > kMaxBytes) {
        // To one decimal: a file of 8.4 MB reported as "8 MB" reads as the
        // limit refusing something that meets it.
        char size[32];
        std::snprintf(size, sizeof(size), "%.1f", static_cast<double>(body.size()) / (1024 * 1024));
        error = std::string("images must be under 8 MB; that one is ") + size + " MB";
        return false;
    }

    // The type is read from the bytes. A caller's content-type is a claim,
    // and the one thing that must not happen here is writing a file whose
    // extension says image and whose content says script.
    const auto* bytes = reinterpret_cast<const unsigned char*>(body.data());
    std::string extension;
    if (body.size() > 8 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0) extension = "png";
    else if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) extension = "jpg";
    else if (std::memcmp(bytes, "GIF8", 4) == 0) extension = "gif";
    else if (body.size() > 12 && std::memcmp(bytes, "RIFF", 4) == 0 &&
             std::memcmp(bytes + 8, "WEBP", 4) == 0) extension = "webp";
    if (extension.empty()) {
        // SVG is deliberately not on the list. It is a document that can carry
        // script, and served from this origin that script would run with the
        // receiver's own privileges.
        error = "that is not a PNG, JPEG, GIF or WebP";
        return false;
    }

    uint8_t digest[32];
    Sha256 hash;
    hash.update(body);
    hash.finish(digest);
    const std::string name = to_hex(digest, 8) + "." + extension;

    const std::string directory = server_config_.uploads_root;
    if (directory.empty()) {
        error = "this receiver has nowhere to keep uploads";
        return false;
    }
    ::mkdir(directory.c_str(), 0755);
    const std::string path = directory + "/" + name;

    // Written whole and moved into place, so a half-written file is never
    // served and an interrupted upload leaves nothing behind.
    const std::string temporary = path + ".part";
    std::FILE* file = std::fopen(temporary.c_str(), "wbe");
    if (!file) {
        error = "cannot write into " + directory;
        return false;
    }
    const bool written = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    if (!written || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        error = "the upload could not be saved";
        return false;
    }

    LOG_INFO("admin", "uploaded %s, %zu kB", name.c_str(), body.size() / 1024);
    url = "/uploads/" + name;
    return true;
}

namespace {

// What a backup may carry of pictures, before base64: a restore's body is
// limited to 9 MB like an upload's.
constexpr size_t kBackupPictureBytes = 6 * 1024 * 1024;
constexpr size_t kBackupPictures = 64;
const char* const kMachineSections[] = {"server", "modules", "admin"};
const char* const kMachineSettings[] = {"directory_id", "muted"};

Json without_machine_settings(const Json& settings) {
    Json out = Json::make_object();
    for (const auto& [key, value] : settings.members()) {
        if (std::find(std::begin(kMachineSettings), std::end(kMachineSettings), key) == std::end(kMachineSettings)) {
            out.set(key, value);
        }
    }
    return out;
}

}  // namespace

std::string Application::backup_json() const {
    Json out = Json::make_object();
    out.set("fernsdr_backup", 1);
    out.set("version", kVersion);
    out.set("created_ms", static_cast<double>(now_ms()));
    out.set("station", radio_.site().name);
    Json files = Json::make_object();
    std::string text;
    if (read_text_file(admin_->config().config_path, text)) {
        files.set("fernsdr.conf", without_sections(text, {std::begin(kMachineSections), std::end(kMachineSections)}));
    }
    Json settings;
    if (!radio_.overlay_path().empty() && read_text_file(radio_.overlay_path(), text) && Json::parse(text, settings) &&
        settings.is_object()) {
        files.set("fernsdr-settings.json", without_machine_settings(settings).serialize());
    }
    if (!radio_.theme().path().empty() && read_text_file(radio_.theme().path(), text)) files.set("fernsdr-theme.json", text);
    out.set("files", files);
    // The pictures in use first, then the rest, as long as they fit.
    Json pictures = Json::make_array();
    Json left_out = Json::make_array();
    size_t total = 0;
    const Json listed = [&] { Json parsed; Json::parse(uploads_json(), parsed); return parsed["uploads"]; }();
    for (const bool used : {true, false}) {
        for (size_t i = 0; i < listed.size(); i++) {
            const Json& item = listed[i];
            if (item["in_use"].boolean(false) != used) continue;
            std::string bytes;
            if (!read_text_file(server_config_.uploads_root + "/" + item["name"].string(), bytes)) continue;
            if (pictures.size() >= kBackupPictures || total + bytes.size() > kBackupPictureBytes) {
                left_out.push_back(item["name"].string());
                continue;
            }
            total += bytes.size();
            Json picture = Json::make_object();
            picture.set("name", item["name"].string());
            picture.set("data", base64_encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
            pictures.push_back(picture);
        }
    }
    out.set("pictures", pictures);
    if (left_out.size() > 0) out.set("pictures_left_out", left_out);
    Json modules = Json::make_array();
    if (radio_.module_store()) {
        for (const auto& module : radio_.module_store()->list()) {
            Json entry = Json::make_object();
            entry.set("id", module.id);
            entry.set("version", module.active);
            entry.set("origin", module.origin);
            modules.push_back(entry);
        }
    }
    out.set("modules", modules);
    return out.serialize();
}

bool Application::restore_backup(const Json& backup, Json& result, std::string& error) {
    const Json& files = backup["files"];
    const std::string restored = files["fernsdr.conf"].string();
    if (restored.empty() || restored.size() > 1024 * 1024) {
        error = "the backup holds no configuration";
        return false;
    }
    const std::string path = admin_->config().config_path;
    std::string current_text;
    Config current;
    if (!read_text_file(path, current_text) || !current.load(path, error)) {
        error = "cannot read " + path;
        return false;
    }
    const std::vector<std::string> machine(std::begin(kMachineSections), std::end(kMachineSections));
    // Where the old machine kept its waterfall history and look is its own;
    // here the defaults do, and the look comes with the backup anyway. Other
    // files a band reads, and network inputs, are refused below as the
    // configuration editor refuses them: the old paths would name nothing here.
    std::string merged = without_keys(without_sections(restored, machine), [](const std::string& section, const std::string& key) {
        return (section.rfind("band:", 0) == 0 && key == "history_path") ||
               (section == "site" && (key == "theme_file" || key == "spot_server"));
    });
    if (!merged.empty() && merged.back() != '\n') merged += '\n';
    merged += "\n" + sections_of(current_text, machine);
    Config candidate;
    if (!candidate.parse(merged, error)) return false;
    const std::string refused = machine_only_change(current, candidate);
    if (!refused.empty()) {
        error = "the backup cannot be played back here: " + refused;
        return false;
    }
    {
        Radio probe;
        if (!probe.configure(candidate, error)) {
            error = "the backup's configuration does not start here: " + error;
            return false;
        }
    }
    // The panel's settings and look, checked before anything is written: a
    // theme the receiver would refuse at its start would otherwise turn into
    // the built-in look without a word.
    Json settings, theme;
    const std::string settings_text = files["fernsdr-settings.json"].string();
    const std::string theme_text = files["fernsdr-theme.json"].string();
    if (!settings_text.empty() && (!Json::parse(settings_text, settings) || !settings.is_object())) {
        error = "the backup's settings are damaged";
        return false;
    }
    if (!theme_text.empty() && (!Json::parse(theme_text, theme) || !validate_theme(theme, error))) {
        error = "the backup's look cannot be used here: " + error;
        return false;
    }
    std::vector<std::string> pictures;
    const Json& listed = backup["pictures"];
    for (size_t i = 0; i < listed.size() && i < kBackupPictures; i++) {
        std::string bytes;
        if (!base64_decode(listed[i]["data"].string(), bytes)) {
            error = "a picture in the backup is damaged";
            return false;
        }
        pictures.push_back(std::move(bytes));
    }

    // The listing id and the chat's mutes stay with this machine, as they
    // were never in a backup: the id is what proves a listing is this
    // receiver's, and the mutes are listeners' addresses.
    const std::string directory = path.substr(0, path.find_last_of('/') + 1);
    const std::string settings_path = directory + "fernsdr-settings.json";
    const std::string theme_path = directory + "fernsdr-theme.json";
    if (!settings_text.empty()) {
        Json kept;
        std::string text;
        if (read_text_file(settings_path, text)) Json::parse(text, kept);
        settings = without_machine_settings(settings);
        for (const char* key : kMachineSettings) {
            if (kept.is_object() && kept.has(key)) settings.set(key, kept[key]);
        }
    }

    // Every file is written beside its place first and renamed over it only
    // once all of them are written, so a full disk leaves this receiver as
    // it was. The configuration goes first: once it is in place, the backup
    // is taken, and what follows it is said to be missing if it fails.
    struct Staged {
        std::string path;
        std::string text;
        FileAccess access;
    };
    std::vector<Staged> staged = {{path, merged, FileAccess::OwnerOnly}};
    if (!settings_text.empty()) staged.push_back({settings_path, settings.serialize(), FileAccess::OwnerOnly});
    if (!theme_text.empty()) staged.push_back({theme_path, theme_text, FileAccess::Default});
    const auto discard = [&] {
        for (const Staged& file : staged) std::remove((file.path + ".restore").c_str());
    };
    error.clear();
    for (const Staged& file : staged) {
        if (!write_text_file(file.path + ".restore", file.text, error, file.access)) {
            discard();
            return false;
        }
    }
    for (size_t i = 0; i < staged.size(); i++) {
        if (std::rename((staged[i].path + ".restore").c_str(), staged[i].path.c_str()) != 0) {
            error = i == 0 ? "cannot replace " + path
                           : "the configuration was restored, but " + staged[i].path +
                                 " could not be replaced; restart FernSDR and set it again in the panel";
            discard();
            if (i == 0) return false;
            break;
        }
        if (i == 0) {
            restored_ = true;
            radio_.hold_overlay();
        }
    }
    if (!error.empty()) return false;

    Json not_restored = Json::make_array();
    for (const std::string& bytes : pictures) {
        std::string url, problem;
        if (!store_upload(bytes, url, problem)) {
            LOG_WARN("admin", "a picture from the backup was not restored: %s", problem.c_str());
            not_restored.push_back(problem);
        }
    }
    // What to install again: the backup's modules this receiver lacks.
    Json missing = Json::make_array();
    const Json& modules = backup["modules"];
    for (size_t i = 0; i < modules.size() && i < 32; i++) {
        const std::string id = modules[i]["id"].string();
        ModuleStore::Module found;
        if (!valid_module_id(id) || (radio_.module_store() && radio_.module_store()->find(id, found))) continue;
        Json entry = Json::make_object();
        entry.set("id", id);
        entry.set("origin", modules[i]["origin"].string());
        missing.push_back(entry);
    }
    result = Json::make_object();
    result.set("ok", true);
    result.set("modules", missing);
    const size_t skipped = listed.size() > kBackupPictures ? listed.size() - kBackupPictures : 0;
    result.set("pictures_not_restored", static_cast<double>(not_restored.size() + skipped));
    LOG_INFO("admin", "a backup of %s from FernSDR %s was played back; the receiver restarts to take it",
             backup["station"].string().c_str(), backup["version"].string().c_str());
    return true;
}

std::string Application::admin_state_json() const {
    Json out = Json::make_object();
    out.set("site", radio_.site().name);
    out.set("users", session_count());
    out.set("max_users", max_users_);
    if (restored_) out.set("restored", true);

    Json bands = Json::make_array();
    for (const auto& band : radio_.bands()) {
        const BandInfo info = band->info();
        Json entry = Json::make_object();
        entry.set("id", info.id);
        entry.set("name", info.name);
        entry.set("low", info.low_hz);
        entry.set("high", info.high_hz);
        entry.set("listeners", info.listeners);
        // The floor is what an operator checks first when a band sounds wrong:
        // a jump means a new local noise source, a collapse means the antenna
        // has come adrift.
        entry.set("noise_floor", std::round(info.noise_floor_dbfs * 10.0) / 10.0);
        entry.set("running", band->running());
        entry.set("online", info.online);
        entry.set("status", info.status);
        entry.set("on_air", info.on_air);
        entry.set("hours", info.hours);
        entry.set("next_change", static_cast<double>(info.next_change_ms));
        const double retry = band->retry_in_seconds();
        if (retry >= 0) entry.set("retry_in", std::round(retry * 10.0) / 10.0);
        entry.set("restarting", band->restarting());
        entry.set("error", band->last_error());
        // What the front end corrections are finding, while they run.
        if (info.dc_remove || info.iq_balance) {
            Json input = Json::make_object();
            if (info.dc_remove) input.set("dc_offset_dbfs", std::round(info.dc_offset_dbfs * 10.0) / 10.0);
            if (info.iq_balance) {
                input.set("gain_error_db", std::round(info.gain_error_db * 100.0) / 100.0);
                input.set("phase_error_degrees", std::round(info.phase_error_degrees * 100.0) / 100.0);
                input.set("image_rejection_db", std::round(info.image_rejection_db * 10.0) / 10.0);
            }
            entry.set("input", input);
        }
        Json details = band->source_details();
        if (details.is_object()) {
            // The whole log has its own endpoint; this is polled every few
            // seconds and only needs the latest lines.
            Json recent = Json::make_array();
            const Json& log = details["log"];
            for (size_t i = log.size() > 3 ? log.size() - 3 : 0; i < log.size(); i++) recent.push_back(log[i]);
            details.set("log", recent);
            entry.set("module", details);
        }
        bands.push_back(entry);
    }
    out.set("bands", bands);

    Json listeners = Json::make_array();
    const int64_t now = now_ms();
    if (server_) {
        server_->for_each_connection([&](Connection& connection) {
            Session* session = static_cast<Session*>(connection.user_data);
            if (!session) return;
            const Session::Summary summary = session->summarise();
            Json entry = Json::make_object();
            entry.set("id", static_cast<double>(summary.id));
            entry.set("address", connection.remote_address());
            entry.set("band", summary.band);
            entry.set("frequency", summary.frequency_hz);
            entry.set("mode", summary.mode);
            entry.set("bandwidth", summary.bandwidth_hz);
            entry.set("audio_bitrate", summary.audio_bitrate);
            entry.set("waterfall_bitrate", summary.waterfall_bitrate);
            entry.set("connected_seconds", static_cast<double>(summary.connected_seconds));
            // Worked out here: a mute covers an IPv6 listener's whole /64,
            // which the panel cannot see from the address alone.
            entry.set("muted", radio_.chat().muted(connection.remote_address(), now));
            listeners.push_back(entry);
        });
    }
    out.set("listeners", listeners);
    out.set("muted", radio_.chat().mutes_json(now));
    return out.serialize();
}

void Application::broadcast_station() {
    refresh_space_weather();
    if (!server_) return;
    const SiteInfo& site = radio_.site();
    Json message = Json::make_object();
    message.set("type", "station");
    Json site_json = Json::make_object();
    site_json.set("name", site.name);
    site_json.set("operator", site.operator_name);
    site_json.set("location", site.location);
    site_json.set("grid", site.grid_square);
    site_json.set("band_plan", site.band_plan);
    site_json.set("antenna", site.antenna);
    site_json.set("contact", site.contact);
    site_json.set("website", site.website);
    site_json.set("notice", site.notice);
    site_json.set("source_url", site.source_url);
    site_json.set("chat", site.chat);
    message.set("site", site_json);
    message.set("decoders", radio_.listed_decoders_json());
    const std::string text = message.serialize();
    broadcast_control(text);
}

// The widget decides whether space weather is fetched at all, and the
// station's locator where it is about.
void Application::refresh_space_weather() {
    bool shown = false;
    const Json theme = radio_.theme().snapshot();
    for (const Json& widget : theme["widgets"].elements())
        if (widget["type"].string() == "space") shown = true;
    double lat = std::nan(""), lon = std::nan("");
    locator_centre(radio_.site().grid_square, lat, lon);
    space_weather_.configure(shown, lat, lon);
}

void Application::broadcast_theme() {
    refresh_space_weather();
    if (!server_) return;
    Json message = Json::make_object();
    message.set("type", "theme");
    message.set("theme", radio_.theme().snapshot());
    const std::string text = message.serialize();
    broadcast_control(text);
}

void Application::broadcast_control(const std::string& text) {
    if (!server_) return;
    const size_t bytes = text.size() + (text.size() < 126 ? 2 : text.size() <= 65535 ? 4 : 10);
    server_->for_each_connection([&](Connection& connection) {
        if (Session* session = session_for(connection)) {
            connection.send_text(text);
            session->account_control_bytes(bytes);
        }
    });
}

bool Application::disconnect_session(uint64_t id) {
    if (!server_) return false;
    bool found = false;
    server_->for_each_connection([&](Connection& connection) {
        Session* session = static_cast<Session*>(connection.user_data);
        if (!session || session->id() != id) return;
        LOG_INFO("admin", "disconnecting listener %llu at operator request",
                 static_cast<unsigned long long>(id));
        connection.close(1000, "disconnected by the operator");
        found = true;
    });
    return found;
}

bool Application::on_http(Connection& connection, const HttpRequest& request,
                          std::string& response) {
    if (handle_admin(connection, request, response)) return true;
    if (handle_history(connection, request, response)) return true;
    if (handle_decodes(request, response)) return true;
    // A machine-readable status endpoint: operators monitor these receivers,
    // and "is it up and how loaded is it" should not require scraping HTML.
    if (request.path == "/api/status") {
        response = build_http_response(200, "application/json; charset=utf-8", status_json(),
                                       {{"Cache-Control", "no-store"},
                                        {"Access-Control-Allow-Origin", "*"}},
                                       request.keep_alive());
        return true;
    }
    // For the space weather widget: what the receiver last fetched. Public,
    // as the page it feeds is; the same for everyone for a minute.
    if (request.path == "/api/space-weather") {
        const std::string body = space_weather_.json();
        response = build_http_response(200, "application/json; charset=utf-8", body.empty() ? "{}" : body,
                                       {{"Cache-Control", "public, max-age=60"}}, request.keep_alive());
        return true;
    }
    if (request.path == "/metrics") {
        response = build_http_response(200, "text/plain; version=0.0.4; charset=utf-8", metrics_text(),
                                       {{"Cache-Control", "no-store"}}, request.keep_alive());
        return true;
    }
    if (request.path == "/api/health") {
        bool healthy = true;
        for (const auto& band : radio_.bands()) healthy = healthy && band->healthy();
        response = build_http_response(healthy ? 200 : 503, "text/plain", healthy ? "ok" : "band stopped",
                                       {{"Cache-Control", "no-store"}}, request.keep_alive());
        return true;
    }
    return false;
}

/**
 * The receiver in Prometheus' text format.
 *
 * Deliberately unauthenticated and deliberately thin: how many people are
 * listening, how much is going out, and whether each band is alive. Nothing
 * here says who anybody is - no addresses, no frequencies, nothing that would
 * turn a scrape into a record of what a named listener was doing. An operator
 * who wants it private can refuse /metrics at their proxy, which is where they
 * would do it anyway.
 */
std::string Application::metrics_text() const {
    std::string out;
    auto line = [&out](const std::string& name, const std::string& labels, double value) {
        out += name;
        if (!labels.empty()) out += "{" + labels + "}";
        char formatted[64];
        std::snprintf(formatted, sizeof(formatted), " %.6g\n", value);
        out += formatted;
    };

    out += "# HELP fernsdr_listeners Listeners connected right now.\n";
    out += "# TYPE fernsdr_listeners gauge\n";
    line("fernsdr_listeners", "", session_count());
    out += "# HELP fernsdr_listeners_max The configured ceiling on listeners.\n";
    out += "# TYPE fernsdr_listeners_max gauge\n";
    line("fernsdr_listeners_max", "", max_users_);

    double audio_bps = 0.0;
    double waterfall_bps = 0.0;
    if (server_) {
        server_->for_each_connection([&](Connection& connection) {
            Session* session = static_cast<Session*>(connection.user_data);
            if (!session) return;
            const Session::Summary summary = session->summarise();
            audio_bps += summary.audio_bitrate;
            waterfall_bps += summary.waterfall_bitrate;
        });
    }
    out += "# HELP fernsdr_stream_bits_per_second What is going out to listeners.\n";
    out += "# TYPE fernsdr_stream_bits_per_second gauge\n";
    line("fernsdr_stream_bits_per_second", "kind=\"audio\"", audio_bps);
    line("fernsdr_stream_bits_per_second", "kind=\"waterfall\"", waterfall_bps);

    out += "# HELP fernsdr_frames_dropped_total Frames not sent because a socket was too far behind.\n";
    out += "# TYPE fernsdr_frames_dropped_total counter\n";
    line("fernsdr_frames_dropped_total", "", static_cast<double>(stale_frames_));

    out += "# HELP fernsdr_band_running Whether a band is receiving.\n";
    out += "# TYPE fernsdr_band_running gauge\n";
    out += "# HELP fernsdr_band_on_air Whether a band's hours have it on the air; off the air it is stopped on purpose.\n";
    out += "# TYPE fernsdr_band_on_air gauge\n";
    out += "# HELP fernsdr_band_listeners Listeners on a band.\n";
    out += "# TYPE fernsdr_band_listeners gauge\n";
    out += "# HELP fernsdr_band_noise_floor_dbfs The level between the signals.\n";
    out += "# TYPE fernsdr_band_noise_floor_dbfs gauge\n";
    for (const auto& band : radio_.bands()) {
        const BandInfo info = band->info();
        // The id is the operator's own, from their config file, and goes into
        // a label. Quotes and backslashes in it would produce a scrape nobody
        // can parse, so they are dropped rather than escaped: a band called
        // 20m" is a typo, not a requirement.
        std::string id;
        for (char c : info.id) {
            if (c != '"' && c != '\\' && c != '\n') id += c;
        }
        const std::string labels = "band=\"" + id + "\"";
        line("fernsdr_band_running", labels, info.online ? 1 : 0);
        line("fernsdr_band_on_air", labels, info.on_air ? 1 : 0);
        line("fernsdr_band_listeners", labels, info.listeners);
        line("fernsdr_band_noise_floor_dbfs", labels, info.noise_floor_dbfs);
    }
    return out;
}

std::string Application::status_json() const {
    Json out = Json::make_object();
    const SiteInfo& site = radio_.site();
    out.set("name", site.name);
    out.set("operator", site.operator_name);
    out.set("location", site.location);
    out.set("grid", site.grid_square);
    out.set("antenna", site.antenna);
    out.set("users", session_count());
    out.set("max_users", max_users_);

    Json bands = Json::make_array();
    for (const auto& band : radio_.bands()) {
        const BandInfo info = band->info();
        Json entry = Json::make_object();
        entry.set("id", info.id);
        entry.set("name", info.name);
        entry.set("center", info.center_hz);
        entry.set("low", info.low_hz);
        entry.set("high", info.high_hz);
        entry.set("listeners", info.listeners);
        entry.set("running", info.online);
        entry.set("on_air", info.on_air);
        entry.set("source", info.source_kind);
        bands.push_back(entry);
    }
    out.set("bands", bands);
    return out.serialize();
}

DirectoryReport Application::listing_report() {
    const SiteInfo& site = radio_.site();
    DirectoryReport report;
    report.receiver_id = radio_.directory_id();
    report.instance_id = instance_id_;
    report.host = site.public_host;
    report.port = site.public_port > 0 ? site.public_port : server_->bound_port();
    report.grid = site.grid_square;
    report.name = site.name;
    report.antenna = site.antenna;
    report.users = session_count();
    report.max_users = max_users_;
    // Only the bands on the air: two bands sharing one input by their hours
    // are one receiver, and the directory counts what can be heard now.
    for (const auto& band : radio_.bands()) {
        if (!band->on_air()) continue;
        const BandInfo info = band->info();
        report.bands.emplace_back(info.low_hz, info.high_hz);
    }
    return report;
}

}  // namespace fernsdr
