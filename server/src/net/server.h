// Single-threaded epoll server: HTTP for the client bundle, WebSocket for
// everything live.
//
// All socket I/O happens on this one thread.  Band threads never touch a
// socket; they fill their listeners' outboxes and call wake(), and this loop
// drains them.  That keeps the concurrency story small enough to reason
// about: one thread per band doing DSP, one thread doing I/O, and two
// mutex-protected handoffs between them.
#pragma once
#include <cstdint>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "http.h"
#include "output_queue.h"
#include "queue_delay.h"
#include "websocket.h"

namespace fernsdr {

class Server;

// A live WebSocket connection.  Methods are only safe on the server thread.
// The largest message a WebSocket client may send. A listener's messages are
// small: a chat line is at most 400 characters, the longest setting eight
// notches. 64 kB only gave a hostile client more room for expensive input.
inline constexpr size_t kMaxClientMessageBytes = 8 * 1024;

class Connection {
public:
    uint64_t id() const { return id_; }
    // The client, as named by a trusted proxy when there is one.
    const std::string& remote_address() const { return remote_address_; }
    // Whoever is at the other end of the socket, which may be that proxy.
    const std::string& peer_address() const { return peer_address_; }

    void send_text(const std::string& text);
    // `bulk` lets later audio pass this message while it waits; see
    // OutputQueue::frame.
    void send_binary(const std::vector<uint8_t>& payload, bool bulk = false);
    void close(uint16_t code = 1000, const std::string& reason = "");

    size_t pending_bytes() const { return out_.size(); }
    // Lets the response being queued run past ServerConfig::max_output_bytes,
    // up to `bytes`, until the queue next empties. That limit is how the
    // server tells a listener who stopped reading, and it keeps what anyone
    // can make it hold small; a signed-in operator's backup is larger, and a
    // slow link to them is no reason to cut it off.
    void allow_output(size_t bytes) { output_allowance_ = bytes; }
    size_t receive_capacity() const { return in_.capacity(); }
    // An IPPROTO_TCP option of the socket, or -1 if it cannot be read. For
    // tests of what the server sets on a listener's connection.
    int tcp_option(int name) const;
    size_t transport_backlog();
    // The queue in the network beyond this socket, in milliseconds, as of the
    // last transport_backlog().
    int queue_delay_ms() const { return queue_delay_.ms(); }
    bool has_expired_binary(int64_t now_ms, int64_t age_ms) const {
        return out_.has_expired_binary(now_ms, age_ms);
    }
    size_t prune_binary(int64_t now_ms, int64_t age_ms, const OutputQueue::KeepBinary& keep) {
        return out_.prune_binary(now_ms, age_ms, keep);
    }

    // Session state, owned by the handler.
    void* user_data = nullptr;

private:

    friend class Server;

    enum class State { Http, WebSocket, Closing };

    Connection(uint64_t id, int fd, std::string remote_address)
        : id_(id), fd_(fd), peer_address_(remote_address), remote_address_(std::move(remote_address)) {}

    uint64_t id_;
    int fd_;
    const std::string peer_address_;
    std::string remote_address_;
    State state_ = State::Http;
    // A close frame is queued. Only the first close is sent: a WebSocket
    // endpoint may send one, and the first caller's reason is the true one.
    bool close_queued_ = false;
    // This connection is sending a body too large for the ordinary limit, and
    // has been counted against the ceiling on those. See process_http.
    bool oversized_ = false;
    // The request has already been answered (with a refusal) while the client
    // is still sending it. Anything more that arrives is read and thrown away,
    // so the client's send completes and it gets to read the answer instead of
    // a reset connection. See refuse_and_drain.
    bool draining_ = false;
    // When to give up waiting for a draining client to finish sending.
    int64_t drain_deadline_ms_ = 0;

    std::vector<uint8_t> in_;
    OutputQueue out_;
    size_t output_allowance_ = 0;
    // network_key() and wide_network_key() of the peer, worked out once at
    // accept for the per-address connection limits.
    std::string network_;
    std::string wide_network_;

    ws::MessageAssembler assembler_{kMaxClientMessageBytes};
    double message_credit_ = 480;
    int64_t credit_updated_ms_ = 0;
    int64_t backlog_updated_ms_ = 0;
    size_t kernel_backlog_ = 0;
    QueueDelay queue_delay_;
    // Recorded separately from state_: drop() moves the connection to
    // Closing, and the retire path still has to know whether a disconnect
    // callback is owed.
    bool handshake_completed_ = false;
    bool disconnect_delivered_ = false;
    bool want_write_ = false;
    bool close_after_flush_ = false;
    int64_t last_activity_ms_ = 0;
    int64_t last_pong_ms_ = 0;
    int64_t request_started_ms_ = 0;
    bool request_headers_complete_ = false;
    double http_credit_ = 60;
    int64_t http_credit_updated_ms_ = 0;
};

class ServerHandler {
public:
    virtual ~ServerHandler() = default;

    // A WebSocket connection has completed its handshake.  Return false to
    // reject it (over capacity, blocked, and so on).
    virtual bool on_connect(Connection& connection) = 0;
    virtual void on_text(Connection& connection, const std::string& text) = 0;
    virtual void on_disconnect(Connection& connection) = 0;
    // The handler may expire complete unsent media. It runs before each
    // write, including writable events that precede the next producer flush.
    virtual void on_before_write(Connection& connection) {}

    // Called whenever a producer thread signals new data, so latency tracks
    // the DSP block rate rather than a polling interval.
    virtual void on_flush() = 0;
    // Called at a slow fixed rate for telemetry, keepalives and reaping.
    virtual void on_tick() = 0;

    // Whether a request may send a body larger than the ordinary limit, asked
    // with only its head, before the body arrives. The one path that takes a
    // large body is the admin panel's image upload, and a body refused only
    // once it has all arrived lets anyone hold the few slots for those with
    // a slow trickle. `head.secure` is already set.
    virtual bool may_upload(Connection& connection, const HttpRequest& head) { return false; }

    // Answers an HTTP request that is not a WebSocket upgrade and not a
    // static file.  Return false to let the server reply 404.
    // The connection is passed so a handler can rate-limit or log by address.
    virtual bool on_http(Connection& connection, const HttpRequest& request, std::string& response) { return false; }
};

struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    int port = 8073;
    std::string document_root;
    // Where uploaded images are kept and served from. Deliberately not inside
    // the document root: that directory is build output, and a client rebuild
    // empties it, which would take an operator's uploaded pictures with it.
    std::string uploads_root;
    // CSP frame-ancestors sources for the listener's page; see
    // parse_frame_ancestors.
    std::string frame_ancestors = "'self'";
    int max_connections = 400;
    // Connections one address (network_key) may hold at once, counted as they
    // are accepted. Without it one address could take every slot and keep
    // them, and no listener or operator could get in. Not applied to
    // trusted_proxies, from which everyone arrives; a proxy has to limit its
    // own clients. 0 is no limit.
    int max_connections_per_address = 32;
    // A connection that has not been heard from in this long is dropped.
    int idle_timeout_seconds = 120;
    // A kept-alive HTTP connection with no request under way and nothing left
    // to send is closed after this long. Browsers open a new one when they
    // need it; holding it for the full idle timeout only let anyone park a
    // connection slot for two minutes with one short request.
    int keepalive_idle_ms = 10000;
    int header_timeout_ms = 15000;
    int request_timeout_ms = 120000;
    // Beyond this much unsent data the client is not keeping up at all.
    size_t max_output_bytes = 2 * 1024 * 1024;
    // WebSocket endpoint path.
    std::string websocket_path = "/ws";
    // CIDRs whose X-Forwarded-For / X-Real-IP headers are believed.  Anyone
    // can send those headers, so believing them from an arbitrary peer would
    // let a listener write whatever address they liked into the log and slip
    // past a per-address limit.  Believed only from the addresses named here.
    std::vector<std::string> trusted_proxies;
};

// True when `address` falls inside `cidr` ("127.0.0.1", "10.0.0.0/8",
// "loopback", or "any").  Exposed for testing.
bool address_matches_cidr(const std::string& address, const std::string& cidr);

// What a limit, a lockout or a mute applies to: an IPv4 address, or the /64 an
// IPv6 address belongs to, as "2001:db8:1:2::/64", or "loopback" for every
// address of this machine itself. A host is usually given a whole /64 and can
// pick a fresh address in it for every connection, so the address alone would
// let one listener be many. Text that is not an address comes back unchanged,
// so a key passed in again stays the same key.
std::string network_key(const std::string& address);
// The /48 an IPv6 address is in, or empty for an IPv4 one. A home connection
// is commonly given a /56 or a /48, hundreds of /64s, so a limit per /64 alone
// is a limit a household could multiply.
std::string wide_network_key(const std::string& address);

// True for a name the receiver could have written to its uploads directory:
// sixteen hex digits and a known image extension. The panel's delete takes a
// name from outside, and /uploads/ would otherwise serve whatever else is in
// that directory, so only this shape is accepted at all. Sanitising instead is
// a game of finding every way to write "..\/".
bool is_an_upload_name(const std::string& name);

// The address to attribute a request to, given the peer it arrived from and
// the forwarding headers it carried.
std::string resolve_client_address(const std::string& peer, const HttpRequest& request,
                                   const std::vector<std::string>& trusted_proxies);
bool request_is_secure(const std::string& peer, const HttpRequest& request,
                       const std::vector<std::string>& trusted_proxies);
bool is_trusted_proxy(const std::string& peer, const std::vector<std::string>& trusted_proxies);

class Server {
public:
    Server(ServerConfig config, ServerHandler& handler);
    ~Server();

    bool start(std::string& error);
    // Runs until stop() is called.
    void run();
    void stop();

    // Safe to call from any thread; makes run() iterate promptly.
    void wake();

    int connection_count() const { return static_cast<int>(connections_.size()); }
    // The port actually bound, which matters when the config asked for 0 and
    // the kernel chose one.
    int bound_port() const { return bound_port_; }
    const ServerConfig& config() const { return config_; }

    // Iterates live WebSocket connections.  Server thread only.
    void for_each_connection(const std::function<void(Connection&)>& fn);

private:
    // How many connections are currently sending a body larger than the
    // ordinary limit. See kMaxConcurrentUploads.
    int uploads_in_flight_ = 0;

    void accept_pending();
    void handle_readable(Connection& connection);
    void handle_writable(Connection& connection);
    // Answer a request that will not be served, without cutting off a client
    // that is still sending it.
    // Serves /uploads/... out of the uploads directory.
    bool serve_upload(const HttpRequest& request, std::string& response) const;
    void refuse_and_drain(Connection& connection, int status, const std::string& message);
    void process_http(Connection& connection);
    void process_websocket(Connection& connection);
    bool complete_handshake(Connection& connection, const HttpRequest& request);
    void queue(Connection& connection, const uint8_t* data, size_t size);
    void update_interest(Connection& connection);
    size_t output_limit(const Connection& connection) const;
    void drop(Connection& connection, const char* reason);
    void reap_idle();

    ServerConfig config_;
    ServerHandler& handler_;
    bool refused_loopback_logged_ = false;
    StaticFiles static_files_;
    StaticFiles uploads_;

    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    std::atomic<bool> running_{false};

    uint64_t next_connection_id_ = 1;
    std::map<int, std::unique_ptr<Connection>> connections_;
    std::vector<int> to_close_;
    int64_t last_tick_ms_ = 0;
    int bound_port_ = 0;
    // False when the IPv6 socket could not be created and IPv4 was used.
    bool dual_stack_ = true;
};

// Milliseconds from a monotonic clock.
int64_t monotonic_ms();

}  // namespace fernsdr
