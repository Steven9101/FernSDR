#include "../src/net/http.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using fernsdr::HttpParse;
using fernsdr::HttpRequest;
using fernsdr::StaticFiles;

namespace {

HttpParse parse(const std::string& raw, HttpRequest& out) {
    size_t consumed = 0;
    return fernsdr::parse_http_request(raw.data(), raw.size(), out, consumed);
}

// Builds a throwaway document root with a couple of files and a symlink that
// points outside it.
struct Sandbox {
    std::string root;
    std::string outside;

    Sandbox() {
        char pattern[] = "/tmp/fernsdr-http-XXXXXX";
        const char* made = mkdtemp(pattern);
        base = made ? made : "/tmp/fernsdr-http-fallback";
        root = base + "/www";
        outside = base + "/secret";
        mkdir(root.c_str(), 0755);
        mkdir((root + "/assets").c_str(), 0755);
        write(root + "/index.html", "<!doctype html><title>fernsdr</title>");
        write(root + "/assets/app.js", "console.log(1)");
        write(outside, "top secret");
        // Without the link the traversal tests would pass without testing it.
        CHECK(symlink(outside.c_str(), (root + "/escape.txt").c_str()) == 0);
    }

    ~Sandbox() {
        // Best effort; the sandbox lives under /tmp.
        std::string command = "rm -rf '" + base + "'";
        if (system(command.c_str()) != 0) { /* ignore */ }
    }

    static void write(const std::string& path, const std::string& body) {
        std::ofstream out(path, std::ios::binary);
        out << body;
    }

    std::string base;
};

}  // namespace

TEST_CASE(http_parses_a_request) {
    HttpRequest request;
    CHECK(parse("GET /index.html?a=1 HTTP/1.1\r\nHost: sdr.example\r\nUser-Agent: x\r\n\r\n", request) ==
          HttpParse::Ok);
    CHECK(request.method == "GET");
    CHECK(request.path == "/index.html");
    CHECK(request.query == "a=1");
    CHECK(request.header("host") == "sdr.example");
    CHECK(request.header("HOST") == "sdr.example");  // case-insensitive
    CHECK(request.keep_alive());
}

TEST_CASE(http_needs_more_until_the_header_block_is_complete) {
    const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    HttpRequest request;
    for (size_t prefix = 0; prefix < raw.size(); prefix++) {
        size_t consumed = 0;
        CHECK(fernsdr::parse_http_request(raw.data(), prefix, request, consumed) == HttpParse::NeedMore);
    }
    size_t consumed = 0;
    CHECK(fernsdr::parse_http_request(raw.data(), raw.size(), request, consumed) == HttpParse::Ok);
    CHECK_EQ(consumed, static_cast<long long>(raw.size()));
}

TEST_CASE(http_rejects_malformed_requests) {
    HttpRequest request;
    CHECK(parse("GARBAGE\r\n\r\n", request) == HttpParse::Error);
    CHECK(parse("GET /\r\n\r\n", request) == HttpParse::Error);
    CHECK(parse("GET / HTTP/9.9\r\n\r\n", request) == HttpParse::Error);
    CHECK(parse("GET / HTTP/1.1\r\nBadHeader\r\n\r\n", request) == HttpParse::Error);
    CHECK(parse("GET nonabsolute HTTP/1.1\r\n\r\n", request) == HttpParse::Error);
}

TEST_CASE(http_rejects_ambiguous_framing_before_reading_a_body) {
    HttpRequest request;
    for (const char* headers : {
        "Content-Length: 0\r\nContent-Length: 5\r\n",
        "Transfer-Encoding: chunked\r\n",
        "Content-Length: +1\r\n", "Content-Length: -1\r\n",
        "Content-Length:\r\n", "Content-Length : 1\r\n",
        "Host: one\r\nHost: two\r\n", " folded: yes\r\n",
        "X-Test: value\001more\r\n"
    }) {
        CHECK(parse(std::string("POST / HTTP/1.1\r\n") + headers + "\r\n", request) == HttpParse::Error);
    }
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 9999999999999999999999999\r\n\r\n", request) == HttpParse::TooLarge);
}

TEST_CASE(http_separates_a_body_that_is_too_large_from_a_bad_request) {
    // The two are answered differently: one is a client that sent nonsense,
    // the other is somebody whose photo is bigger than the limit and needs to
    // be told that rather than "bad request".
    std::string raw = "POST /api/admin/upload HTTP/1.1\r\nContent-Length: 200000\r\n\r\n";
    HttpRequest request;
    size_t consumed = 0;
    CHECK(fernsdr::parse_http_request(raw.data(), raw.size(), request, consumed) == HttpParse::TooLarge);

    // Raise the ceiling and the same request is merely incomplete.
    CHECK(fernsdr::parse_http_request(raw.data(), raw.size(), request, consumed, 1024 * 1024) ==
          HttpParse::NeedMore);

    // A Content-Length that is not a number stays an error, not a size.
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: soon\r\n\r\n", request) == HttpParse::Error);
}

TEST_CASE(http_rejects_oversized_headers) {
    // Otherwise a client can make the server buffer without bound.
    std::string raw = "GET / HTTP/1.1\r\n";
    raw += "X-Big: " + std::string(64 * 1024, 'a') + "\r\n\r\n";
    HttpRequest request;
    CHECK(parse(raw, request) == HttpParse::Error);
}

TEST_CASE(http_detects_upgrade_tokens_within_header_lists) {
    HttpRequest request;
    CHECK(parse("GET /ws HTTP/1.1\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n\r\n",
                request) == HttpParse::Ok);
    CHECK(request.header_contains("connection", "upgrade"));
    CHECK(request.header_contains("upgrade", "websocket"));
    // Must not match a token that is merely a substring.
    CHECK(!request.header_contains("upgrade", "websock"));
}

TEST_CASE(http_percent_decoding) {
    std::string out;
    CHECK(fernsdr::percent_decode("/a%20b%2Fc", out));
    CHECK(out == "/a b/c");
    CHECK(!fernsdr::percent_decode("/a%zz", out));
    CHECK(!fernsdr::percent_decode("/a%2", out));
    // An encoded NUL would truncate the path in any filesystem call.
    CHECK(!fernsdr::percent_decode("/a%00b", out));
}

TEST_CASE(http_static_serves_files_from_the_root) {
    Sandbox sandbox;
    StaticFiles files(sandbox.root);
    CHECK(files.enabled());

    HttpRequest request;
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", request) == HttpParse::Ok);
    std::string response;
    CHECK(files.serve(request, response));
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("text/html") != std::string::npos);
    CHECK(response.find("<title>fernsdr</title>") != std::string::npos);
}

TEST_CASE(http_static_refuses_to_escape_the_document_root) {
    // The one thing in this file that absolutely must not be wrong.
    Sandbox sandbox;
    StaticFiles files(sandbox.root);

    const char* attempts[] = {
        "/../secret",
        "/../../etc/passwd",
        "/assets/../../secret",
        "/./../secret",
        "/%2e%2e/secret",
        "/%2e%2e%2fsecret",
        "/....//secret",
        "/escape.txt",  // symlink pointing outside the root
    };
    for (const char* attempt : attempts) {
        std::string absolute;
        const bool resolved = files.resolve(attempt, absolute);
        if (resolved) {
            // If it resolved at all it must still be inside the root.
            CHECK(absolute.compare(0, files.root().size(), files.root()) == 0);
            CHECK(absolute.find("secret") == std::string::npos);
            CHECK(absolute.find("passwd") == std::string::npos);
        } else {
            CHECK(!resolved);
        }
    }
}

TEST_CASE(http_static_serves_a_precompressed_sidecar_when_offered) {
    Sandbox sandbox;
    Sandbox::write(sandbox.root + "/assets/app.js.gz", "not really gzip, but distinct");
    StaticFiles files(sandbox.root);

    HttpRequest plain;
    CHECK(parse("GET /assets/app.js HTTP/1.1\r\nHost: x\r\n\r\n", plain) == HttpParse::Ok);
    std::string response;
    CHECK(files.serve(plain, response));
    CHECK(response.find("Content-Encoding") == std::string::npos);
    CHECK(response.find("console.log(1)") != std::string::npos);

    HttpRequest compressed;
    CHECK(parse("GET /assets/app.js HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, deflate\r\n\r\n",
                compressed) == HttpParse::Ok);
    CHECK(files.serve(compressed, response));
    CHECK(response.find("Content-Encoding: gzip") != std::string::npos);
    CHECK(response.find("not really gzip") != std::string::npos);
}

TEST_CASE(http_static_keeps_a_sidecar_inside_the_root) {
    // The file asked for is inside; a sidecar that links outside is not
    // served in its place, and the file itself is, uncompressed.
    Sandbox sandbox;
    CHECK(symlink(sandbox.outside.c_str(), (sandbox.root + "/assets/app.js.gz").c_str()) == 0);
    StaticFiles files(sandbox.root);
    HttpRequest compressed;
    CHECK(parse("GET /assets/app.js HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n", compressed) ==
          HttpParse::Ok);
    std::string response;
    CHECK(files.serve(compressed, response));
    CHECK(response.find("top secret") == std::string::npos);
    CHECK(response.find("Content-Encoding") == std::string::npos);
    CHECK(response.find("console.log(1)") != std::string::npos);
}

TEST_CASE(http_static_answers_conditional_requests) {
    Sandbox sandbox;
    StaticFiles files(sandbox.root);

    HttpRequest request;
    CHECK(parse("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", request) == HttpParse::Ok);
    std::string response;
    CHECK(files.serve(request, response));

    const size_t etag_pos = response.find("ETag: ");
    CHECK(etag_pos != std::string::npos);
    const size_t etag_end = response.find("\r\n", etag_pos);
    const std::string etag = response.substr(etag_pos + 6, etag_end - etag_pos - 6);

    HttpRequest conditional;
    CHECK(parse("GET /index.html HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + "\r\n\r\n",
                conditional) == HttpParse::Ok);
    CHECK(files.serve(conditional, response));
    CHECK(response.find("304") != std::string::npos);
}

TEST_CASE(http_static_returns_false_for_missing_files) {
    Sandbox sandbox;
    StaticFiles files(sandbox.root);
    HttpRequest request;
    CHECK(parse("GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n", request) == HttpParse::Ok);
    std::string response;
    CHECK(!files.serve(request, response));
}

TEST_CASE(http_mime_types) {
    CHECK(fernsdr::mime_type_for("/a/b.html").find("text/html") == 0);
    CHECK(fernsdr::mime_type_for("app.js").find("text/javascript") == 0);
    CHECK(fernsdr::mime_type_for("x.woff2") == "font/woff2");
    CHECK(fernsdr::mime_type_for("unknown.zzz") == "application/octet-stream");
}

TEST_CASE(http_pages_carry_a_content_security_policy_and_frame_rules) {
    Sandbox box;
    Sandbox::write(box.root + "/admin.html", "<!doctype html><title>admin</title>");
    StaticFiles files(box.root);
    const auto get = [&files](const std::string& path) {
        HttpRequest request;
        std::string response;
        CHECK(parse("GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n", request) == HttpParse::Ok);
        CHECK(files.serve(request, response));
        return response;
    };

    const std::string page = get("/");
    CHECK(page.find("Content-Security-Policy: default-src 'self'; script-src 'self';") != std::string::npos);
    CHECK(page.find("frame-ancestors 'self'\r\n") != std::string::npos);
    CHECK(page.find("X-Frame-Options: SAMEORIGIN\r\n") != std::string::npos);

    // The admin page is never framed, whatever the listener's page allows.
    files.set_frame_ancestors("https://club.example.org");
    const std::string admin = get("/admin");
    CHECK(admin.find("frame-ancestors 'none'\r\n") != std::string::npos);
    CHECK(admin.find("X-Frame-Options: DENY\r\n") != std::string::npos);
    // A listener's page an operator lets another site frame has no
    // X-Frame-Options, which cannot name another site.
    const std::string framed = get("/index.html");
    CHECK(framed.find("frame-ancestors https://club.example.org\r\n") != std::string::npos);
    CHECK(framed.find("X-Frame-Options") == std::string::npos);

    // Scripts and the rest are not pages.
    CHECK(get("/assets/app.js").find("Content-Security-Policy") == std::string::npos);
}

TEST_CASE(http_frame_ancestors_take_origins_and_nothing_else) {
    std::string sources, error;
    const auto accepts = [&](const std::string& text, const std::string& expected) {
        error.clear();
        const bool ok = fernsdr::parse_frame_ancestors(text, sources, error);
        CHECK(ok);
        CHECK(sources == expected);
    };
    accepts("", "'self'");
    accepts("self", "'self'");
    accepts("'self'", "'self'");
    accepts("none", "'none'");
    accepts("self https://club.example.org  https://*.example.net:8443", "'self' https://club.example.org https://*.example.net:8443");
    accepts("http://192.0.2.1:8080", "http://192.0.2.1:8080");
    // As copied from a browser's address bar.
    accepts("https://club.example.org/", "https://club.example.org");
    for (const char* text : {"https://a.example; script-src *", "https://a.example,https://b.example", "none self",
                             "javascript:alert(1)", "ftp://example.org", "*", "https://", "example.org",
                             "https://ex\"ample.org", "https://a.*.example", "'unsafe-inline'", "https:///",
                             "https://club.example.org/page", "https://club.example.org//"}) {
        CHECK(!fernsdr::parse_frame_ancestors(text, sources, error));
        CHECK(error.find("frame_ancestors") != std::string::npos);
    }
}

TEST_CASE(http_static_files_know_what_lies_inside_them) {
    Sandbox box;
    const StaticFiles files(box.root);
    CHECK(files.contains(box.root));
    CHECK(files.contains(box.root + "/index.html"));
    CHECK(files.contains(box.root + "/assets/../index.html"));
    // Not made yet, as an archive is before its first line.
    CHECK(files.contains(box.root + "/fernsdr-history-20m.wfa"));
    CHECK(!files.contains(box.outside));
    CHECK(!files.contains(box.root + "/../secret"));
    // Symlinks are followed: a link in the root leading out is outside, a
    // link outside leading in is inside.
    CHECK(!files.contains(box.root + "/escape.txt"));
    CHECK(symlink(box.root.c_str(), (box.base + "/door").c_str()) == 0);
    CHECK(files.contains(box.base + "/door/index.html"));
    CHECK(files.contains(box.base + "/door/not-made-yet"));
    // A sibling whose name only starts the same.
    mkdir((box.root + "2").c_str(), 0755);
    Sandbox::write(box.root + "2/index.html", "");
    CHECK(!files.contains(box.root + "2/index.html"));
    CHECK(!StaticFiles("").contains(box.root));
}

