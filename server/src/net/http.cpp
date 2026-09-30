#include "http.h"

#include "../util/config.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace fernsdr {

namespace {

// Bounds on what a client may send before the request is even understood.
constexpr size_t kMaxRequestLine = 8 * 1024;
constexpr size_t kMaxHeaderBytes = 32 * 1024;
constexpr size_t kMaxHeaderCount = 100;

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ascii(const std::string& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) begin++;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    return s.substr(begin, end - begin);
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        // Better a bare number than a status line that says "OK" next to a 500.
        default: return status < 300 ? "OK" : status < 400 ? "Redirect"
                 : status < 500 ? "Client Error" : "Server Error";
    }
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
    const std::string key = lowercase(name);
    for (const auto& h : headers) {
        if (h.first == key) return h.second;
    }
    return {};
}

bool HttpRequest::header_contains(const std::string& name, const std::string& token) const {
    const std::string value = lowercase(header(name));
    const std::string needle = lowercase(token);
    size_t pos = value.find(needle);
    while (pos != std::string::npos) {
        // Must be a whole token, not a substring of a longer one.
        const bool left_ok = pos == 0 || value[pos - 1] == ' ' || value[pos - 1] == ',';
        const size_t after = pos + needle.size();
        const bool right_ok = after == value.size() || value[after] == ' ' || value[after] == ',' ||
                              value[after] == ';';
        if (left_ok && right_ok) return true;
        pos = value.find(needle, pos + 1);
    }
    return false;
}

bool HttpRequest::keep_alive() const {
    if (header_contains("connection", "close")) return false;
    if (version == "HTTP/1.0") return header_contains("connection", "keep-alive");
    return true;
}

bool percent_decode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size()) return false;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(in[i + 1]);
        const int lo = nibble(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        const char decoded = static_cast<char>((hi << 4) | lo);
        // A percent-encoded NUL would truncate the path in any C API it
        // reaches; refuse rather than silently mangle.
        if (decoded == '\0') return false;
        out += decoded;
        i += 2;
    }
    return true;
}

HttpParse parse_http_request(const char* data, size_t size, HttpRequest& out, size_t& consumed,
                             size_t max_body) {
    // Find the end of the header block.
    const char* end = nullptr;
    for (size_t i = 0; i + 3 < size; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            end = data + i + 4;
            break;
        }
    }
    if (!end) {
        return size > kMaxHeaderBytes ? HttpParse::Error : HttpParse::NeedMore;
    }

    const size_t header_bytes = static_cast<size_t>(end - data);
    if (header_bytes > kMaxHeaderBytes) return HttpParse::Error;

    std::istringstream stream(std::string(data, header_bytes));
    std::string line;

    if (!std::getline(stream, line)) return HttpParse::Error;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > kMaxRequestLine) return HttpParse::Error;

    // "METHOD target VERSION"
    const size_t first_space = line.find(' ');
    if (first_space == std::string::npos) return HttpParse::Error;
    const size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) return HttpParse::Error;

    out = HttpRequest{};
    out.method = line.substr(0, first_space);
    out.target = line.substr(first_space + 1, second_space - first_space - 1);
    out.version = line.substr(second_space + 1);
    if (out.method.empty() || out.target.empty()) return HttpParse::Error;
    if (out.version != "HTTP/1.0" && out.version != "HTTP/1.1") return HttpParse::Error;

    const size_t question = out.target.find('?');
    std::string raw_path = out.target;
    if (question != std::string::npos) {
        raw_path = out.target.substr(0, question);
        out.query = out.target.substr(question + 1);
    }
    if (!percent_decode(raw_path, out.path)) return HttpParse::Error;
    if (out.path.empty() || out.path[0] != '/') return HttpParse::Error;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (out.headers.size() >= kMaxHeaderCount) return HttpParse::Error;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) return HttpParse::Error;
        const std::string name = lowercase(line.substr(0, colon));
        if (name.empty()) return HttpParse::Error;
        for (unsigned char c : name) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  std::strchr("!#$%&'*+-.^_`|~", c))) return HttpParse::Error;
            if (c == 0) return HttpParse::Error;
        }
        const std::string value = trim_ascii(line.substr(colon + 1));
        for (unsigned char c : value) {
            if ((c < 32 && c != '\t') || c == 127) return HttpParse::Error;
        }
        // Reject ambiguous framing instead of disagreeing with a reverse proxy.
        if (name == "transfer-encoding") return HttpParse::Error;
        if (name == "content-length" || name == "host") {
            for (const auto& header : out.headers) if (header.first == name) return HttpParse::Error;
            if (value.empty()) return HttpParse::Error;
        }
        out.headers.emplace_back(name, value);
    }

    // A body, if the request declares one. Only Content-Length is supported:
    // nothing here needs chunked uploads, and a parser for them would be
    // attack surface bought with nothing.
    const std::string length_header = out.header("content-length");
    if (!length_header.empty()) {
        size_t declared = 0;
        for (char c : length_header) {
            if (c < '0' || c > '9') return HttpParse::Error;
        }
        for (char c : length_header) {
            const size_t digit = static_cast<size_t>(c - '0');
            if (digit > max_body || declared > (max_body - digit) / 10) return HttpParse::TooLarge;
            declared = declared * 10 + digit;
        }
        if (size - header_bytes < declared) return HttpParse::NeedMore;
        out.body.assign(data + header_bytes, static_cast<size_t>(declared));
        consumed = header_bytes + static_cast<size_t>(declared);
        return HttpParse::Ok;
    }

    consumed = header_bytes;
    return HttpParse::Ok;
}

std::string build_http_response(int status, const std::string& content_type, const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& extra_headers,
                                bool keep_alive) {
    std::string out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
    if (!content_type.empty()) out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& h : extra_headers) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    out += body;
    return out;
}

std::string mime_type_for(const std::string& path) {
    static const std::pair<const char*, const char*> kTypes[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".woff", "font/woff"},
        {".txt", "text/plain; charset=utf-8"},
        {".wasm", "application/wasm"},
        {".map", "application/json; charset=utf-8"},
    };
    for (const auto& entry : kTypes) {
        const size_t length = std::strlen(entry.first);
        if (path.size() >= length && path.compare(path.size() - length, length, entry.first) == 0) {
            return entry.second;
        }
    }
    return "application/octet-stream";
}

bool parse_frame_ancestors(const std::string& text, std::string& sources, std::string& error) {
    std::istringstream stream(text);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    if (tokens.empty()) tokens.push_back("self");
    sources.clear();
    for (std::string& item : tokens) {
        if (item == "self" || item == "'self'") {
            item = "'self'";
        } else if (item == "none" || item == "'none'") {
            if (tokens.size() > 1) {
                error = "frame_ancestors: none cannot be combined with other sources";
                return false;
            }
            item = "'none'";
        } else {
            // A scheme and host, the host possibly starting with a wildcard,
            // and a port. Nothing that could end the header or add a
            // directive: no quotes, semicolons, commas or control characters.
            // An address copied from a browser ends in a slash, which says
            // nothing more and is dropped.
            if (item.back() == '/' && item.find("://") != std::string::npos &&
                item.find("://") + 3 < item.size() - 1) {
                item.pop_back();
            }
            const size_t scheme = item.find("://");
            const std::string rest = scheme == std::string::npos ? "" : item.substr(scheme + 3);
            const bool known_scheme = item.compare(0, scheme, "https") == 0 || item.compare(0, scheme, "http") == 0;
            bool valid = scheme != std::string::npos && known_scheme && !rest.empty();
            for (size_t i = 0; valid && i < rest.size(); i++) {
                const char c = rest[i];
                valid = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' ||
                        (c == '*' && i == 0 && rest.size() > 2 && rest[1] == '.');
            }
            if (!valid) {
                error = "frame_ancestors: '" + item + "' is not self, none or an origin such as https://example.org";
                return false;
            }
        }
        if (!sources.empty()) sources += ' ';
        sources += item;
    }
    return true;
}

std::string page_security_policy(const std::string& frame_ancestors) {
    // Scripts, styles and fonts come from this receiver only. Pictures and
    // embedded pages may come from wherever the operator's theme points,
    // which the theme's own validation restricts to web addresses and data:
    // images. Styles allow inline attributes because the pages set them.
    return "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
           "img-src 'self' data: https: http:; font-src 'self'; connect-src 'self'; "
           "frame-src https: http:; object-src 'none'; base-uri 'self'; form-action 'self'; "
           "frame-ancestors " + frame_ancestors;
}

StaticFiles::StaticFiles(std::string root) {
    if (root.empty()) return;
    char resolved[PATH_MAX];
    if (!realpath(root.c_str(), resolved)) return;  // leaves serving disabled
    root_ = resolved;
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

bool StaticFiles::resolve(const std::string& url_path, std::string& absolute) const {
    if (root_.empty()) return false;
    if (url_path.empty() || url_path[0] != '/') return false;
    // A NUL anywhere would truncate the path for the filesystem call.
    if (url_path.find('\0') != std::string::npos) return false;

    std::string relative = url_path == "/" ? "/index.html" : url_path;
    // Directory requests get the index.
    if (relative.back() == '/') relative += "index.html";
    // The admin panel is a second page in the same bundle. Serving it from a
    // bare /admin rather than /admin.html is worth one line here: it is the
    // address an operator will type from memory.
    if (relative == "/admin") relative = "/admin.html";

    // Normalise the path ourselves rather than trusting realpath alone: this
    // way a traversal attempt is rejected even if the target does not exist.
    std::vector<std::string> parts;
    std::string segment;
    std::istringstream stream(relative);
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (parts.empty()) return false;  // escapes the root
            parts.pop_back();
            continue;
        }
        parts.push_back(segment);
    }
    if (parts.empty()) return false;

    std::string candidate = root_;
    for (const auto& part : parts) candidate += "/" + part;

    // Second line of defence: resolve symlinks and re-check containment, so a
    // symlink inside the root cannot point outside it.
    char resolved[PATH_MAX];
    if (!realpath(candidate.c_str(), resolved)) return false;
    std::string real = resolved;
    if (real.compare(0, root_.size(), root_) != 0) return false;
    if (real.size() > root_.size() && real[root_.size()] != '/') return false;

    absolute = real;
    return true;
}

bool StaticFiles::contains(const std::string& path) const {
    if (root_.empty() || path.empty()) return false;
    char resolved[PATH_MAX];
    std::string real;
    if (realpath(path.c_str(), resolved)) {
        real = resolved;
    } else {
        const size_t slash = path.find_last_of('/');
        const std::string parent = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        if (!realpath(parent.c_str(), resolved)) return false;
        real = std::string(resolved) + "/" + (slash == std::string::npos ? path : path.substr(slash + 1));
    }
    return root_ == "/" || real == root_ || real.compare(0, root_.size() + 1, root_ + "/") == 0;
}

bool StaticFiles::serve(const HttpRequest& request, std::string& response) const {
    if (!enabled()) return false;
    if (request.method != "GET" && request.method != "HEAD") return false;

    std::string absolute;
    if (!resolve(request.path, absolute)) return false;

    struct stat info;
    if (stat(absolute.c_str(), &info) != 0) return false;
    if (!S_ISREG(info.st_mode)) return false;

    std::vector<std::pair<std::string, std::string>> headers;

    // Precompressed sidecars matter here: the sites this runs on are often on
    // a few tens of megabits total, and shipping the bundle gzipped costs the
    // server nothing at request time.
    std::string source = absolute;
    if (request.header_contains("accept-encoding", "gzip")) {
        struct stat gz;
        const std::string gz_path = absolute + ".gz";
        // Held inside the root like the file itself: stat() follows a link,
        // and a sidecar linked elsewhere would be served in the file's place.
        if (stat(gz_path.c_str(), &gz) == 0 && S_ISREG(gz.st_mode) && contains(gz_path)) {
            source = gz_path;
            info = gz;
            headers.emplace_back("Content-Encoding", "gzip");
            headers.emplace_back("Vary", "Accept-Encoding");
        }
    }

    char etag[64];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"", static_cast<unsigned long long>(info.st_mtime),
             static_cast<unsigned long long>(info.st_size));

    const bool keep_alive = request.keep_alive();
    if (request.header("if-none-match") == etag) {
        response = build_http_response(304, "", "", {{"ETag", etag}}, keep_alive);
        return true;
    }
    headers.emplace_back("ETag", etag);
    // The bundle is content-hashed by the build; index.html must not be.
    const bool immutable = absolute.find("/assets/") != std::string::npos;
    headers.emplace_back("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache");
    // The pages carry the policy. The admin page is never framed; the
    // listener's may be, where the operator says so.
    if (mime_type_for(absolute).rfind("text/html", 0) == 0) {
        const bool admin = absolute.size() >= 11 && absolute.compare(absolute.size() - 11, 11, "/admin.html") == 0;
        const std::string ancestors = admin ? "'none'" : frame_ancestors_;
        headers.emplace_back("Content-Security-Policy", page_security_policy(ancestors));
        if (ancestors == "'none'") headers.emplace_back("X-Frame-Options", "DENY");
        else if (ancestors == "'self'") headers.emplace_back("X-Frame-Options", "SAMEORIGIN");
    }

    std::string body;
    if (!read_text_file(source, body)) return false;

    if (request.method == "HEAD") {
        // Content-Length must still describe the real body.
        std::string full = build_http_response(200, mime_type_for(absolute), body, headers, keep_alive);
        response = full.substr(0, full.size() - body.size());
        return true;
    }

    response = build_http_response(200, mime_type_for(absolute), body, headers, keep_alive);
    return true;
}

}  // namespace fernsdr
