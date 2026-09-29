// HTTP/1.1: just enough to serve the client bundle and carry the WebSocket
// upgrade.  Everything security-sensitive lives here (path containment,
// request size limits), so it is kept small and directly testable.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace fernsdr {

struct HttpRequest {
    bool secure = false;  // assigned by the server after checking the socket peer
    // Assigned by the server: the socket peer is one of trusted_proxies, so
    // the request speaks for someone else, whatever it carries.
    bool via_trusted_proxy = false;
    std::string method;
    std::string target;  // as sent, e.g. "/index.html?x=1"
    std::string path;    // percent-decoded, query stripped
    std::string query;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;  // keys lowercased
    std::string body;  // present only for requests that declare a Content-Length

    // Case-insensitive lookup; returns an empty string when absent.
    std::string header(const std::string& name) const;
    bool header_contains(const std::string& name, const std::string& token) const;
    bool keep_alive() const;
};

enum class HttpParse {
    Ok,
    NeedMore,
    Error,
    // The request declares a body larger than `max_body`. Separate from Error
    // so the caller can say so, rather than answering a phone photo with
    // "bad request".
    TooLarge,
};

// Parses a request from the front of a buffer.  A body is consumed only when
// the request declares a Content-Length, and only up to `max_body`; anything
// larger is an error rather than something to buffer, because an unbounded
// body from an unauthenticated peer is a way to exhaust memory.
HttpParse parse_http_request(const char* data, size_t size, HttpRequest& out, size_t& consumed,
                             size_t max_body = 64 * 1024);

// Percent-decoding; returns false on a malformed escape.
bool percent_decode(const std::string& in, std::string& out);

// Builds a complete response.  `extra_headers` entries must not include CRLF.
std::string build_http_response(int status, const std::string& content_type, const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
                                bool keep_alive = true);

// Content-Type for a filename extension.
std::string mime_type_for(const std::string& path);

/**
 * Reads the [server] frame_ancestors setting: who may show the listener's page
 * in a frame. `self` (the default), `none`, or a list of origins such as
 * `https://club.example.org`, whose host may start with a `*.` wildcard.
 * Fills `sources` with the CSP source list, or `error` with why the text is
 * not one.
 */
bool parse_frame_ancestors(const std::string& text, std::string& sources, std::string& error);

/** The Content-Security-Policy for one of the client's pages. */
std::string page_security_policy(const std::string& frame_ancestors);

class StaticFiles {
public:
    // `root` is the directory to serve.  An empty root disables serving.
    explicit StaticFiles(std::string root);

    // The CSP frame-ancestors sources for the listener's page. The admin page
    // is never framed.
    void set_frame_ancestors(std::string sources) { frame_ancestors_ = std::move(sources); }

    bool enabled() const { return !root_.empty(); }
    const std::string& root() const { return root_; }

    // Fills `response` and returns true when the request maps to a file.
    // Returns false when it does not, so the caller can answer 404 itself.
    bool serve(const HttpRequest& request, std::string& response) const;

    // Resolves a URL path to an absolute filesystem path inside the root.
    // Returns false for anything that escapes it.  Exposed for testing
    // because path containment is the one thing here that must not be wrong.
    bool resolve(const std::string& url_path, std::string& absolute) const;

    // Whether `path`, a filesystem path, is the root or lies under it once
    // symlinks are followed. A file not made yet is judged by the directory
    // it would be made in.
    bool contains(const std::string& path) const;

private:
    std::string root_;  // canonical, no trailing slash
    std::string frame_ancestors_ = "'self'";
};

}  // namespace fernsdr
