#include "admin.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <tuple>

#include "../util/log.h"
#include "../util/password.h"
#include "../net/server.h"

namespace fernsdr {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Five tries in five minutes, then a minute's lockout, and after that every
// wrong guess locks the network out again for twice as long as the last time,
// up to 64 minutes. Slow enough that an online guess of even a poor password is
// hopeless, generous enough that a mistyped password is not a lockout.
//
// The level is kept for a day after the last failure, not for the window. When
// it went with the window, a script that waited out its lockouts until the
// window closed started again at one minute: about 120 guesses an hour from a
// single address, enough to spend the global budget below on its own.
constexpr int kMaxAttempts = 5;
constexpr int64_t kAttemptWindowMs = 5 * 60 * 1000;
constexpr int64_t kBaseLockoutMs = 60 * 1000;
constexpr int kMaxDoublings = 6;
constexpr int64_t kForgetLockoutsMs = 24 * 60 * 60 * 1000;

// A challenge is worth answering for a minute. Long enough for a slow phone to
// grind through 200 000 PBKDF2 rounds, short enough that a nonce captured off
// the wire is worthless by the time it is used.
constexpr int64_t kNonceLifetimeMs = 60 * 1000;
constexpr size_t kMaxAttemptsAddresses = 4096;
constexpr size_t kMaxSessions = 64;
// Across all networks. Five tries per network would otherwise mean five per
// /64 for anyone rotating through a /48. While this many proofs have failed in
// the last hour, only networks an administrator has signed in from since the
// receiver started may try, and a direct connection from the machine itself:
// after a restart nobody is known, and the operator needs a way in.
constexpr size_t kGlobalFailures = 100;
constexpr int64_t kGlobalWindowMs = 60 * 60 * 1000;
constexpr size_t kKnownNetworks = 64;

}  // namespace

namespace {

/**
 * The host a Host header names, lower-cased, without its port, brackets or
 * trailing dot. False when the header is not host[:port] at all, which no
 * browser sends.
 */
bool host_of(const std::string& header, std::string& host, bool& bracketed) {
    std::string text = header;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string port;
    bool has_port = false;
    bracketed = !text.empty() && text.front() == '[';
    if (bracketed) {
        const size_t close = text.find(']');
        if (close == std::string::npos) return false;
        host = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':') return false;
            has_port = true;
            port = text.substr(close + 2);
        }
    } else {
        const size_t colon = text.find(':');
        if (colon != std::string::npos) {
            has_port = true;
            port = text.substr(colon + 1);
            text.erase(colon);
        }
        if (!text.empty() && text.back() == '.') text.pop_back();
        host = text;
    }
    if (has_port && (port.empty() || port.size() > 5 ||
                     !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))) {
        return false;
    }
    return !host.empty();
}

/** A Host header naming this machine: localhost, *.localhost or a loopback literal, any port. */
bool names_loopback(const std::string& header) {
    std::string host;
    bool bracketed = false;
    if (!host_of(header, host, bracketed)) return false;
    if (!bracketed && (host == "localhost" ||
                       (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0))) {
        return true;
    }
    return address_matches_cidr(host, "loopback");
}

/** Whether the request says a proxy or an HTTP-aware tunnel forwarded it. */
bool forwarded(const HttpRequest& request) {
    for (const char* header : {"forwarded", "x-forwarded-for", "x-forwarded-proto", "x-forwarded-host", "x-real-ip"}) {
        if (!request.header(header).empty()) return true;
    }
    return false;
}

/**
 * An address from the ranges home and office networks number themselves
 * from, which the internet does not route, IPv4-mapped forms included. Not
 * 100.64/10: behind carrier-grade NAT, the far side of that range is the
 * provider's other customers.
 */
bool home_network_address(const std::string& address) {
    for (const char* range :
         {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16", "fc00::/7", "fe80::/10"}) {
        if (address_matches_cidr(address, range)) return true;
    }
    return false;
}

/**
 * A Host header only a browser at home sends: this machine's own names, an
 * address from the home ranges, or a name nobody can register on the
 * internet, of one label or under .local, .home.arpa or .internal. Endings
 * that are merely unused there today, like .lan, or already registered, like
 * .fritz.box, could be pointed at the receiver's address by any page.
 */
bool names_home_host(const std::string& header) {
    if (names_loopback(header)) return true;
    std::string host;
    bool bracketed = false;
    if (!host_of(header, host, bracketed)) return false;
    if (bracketed) return home_network_address(host);
    if (std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) {
        return home_network_address(host);
    }
    size_t labels = 0;
    for (size_t begin = 0; begin <= host.size(); labels++) {
        size_t end = host.find('.', begin);
        if (end == std::string::npos) end = host.size();
        if (end == begin || end - begin > 63) return false;
        for (size_t i = begin; i < end; i++) {
            const char c = host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
        begin = end + 1;
    }
    if (labels == 1) return true;
    for (const char* suffix : {".local", ".home.arpa", ".internal"}) {
        const size_t length = std::strlen(suffix);
        if (host.size() > length && host.compare(host.size() - length, length, suffix) == 0) return true;
    }
    return false;
}

}  // namespace

bool direct_local_connection(const HttpRequest& request, const std::string& peer) {
    return address_matches_cidr(peer, "loopback") && !forwarded(request);
}

std::string admin_transport_refusal(const HttpRequest& request, const std::string& peer, bool home_network,
                                    bool anywhere) {
    if (request.secure || anywhere) return "";
    const std::string host = request.header("host");
    const bool this_machine = direct_local_connection(request, peer);
    // Plain HTTP only for a browser on this machine, or one reaching it
    // through an SSH tunnel: a direct local connection, addressed to this
    // machine by name. A reverse proxy or an HTTP-aware tunnel says where a
    // request came from and how; a raw TCP tunnel carries the public name its
    // user typed. A stranger can fake all of this through a raw tunnel, and
    // gains nothing: the password is still needed, and everyone coming
    // through the tunnel shares the loopback address's lockout. What it
    // protects is an honest administrator's session from crossing the
    // internet in the clear.
    if (this_machine && names_loopback(host)) return "";
    // With home_network, also a browser at home, whose requests never cross
    // the internet: this machine under a home name (Debian and Raspberry Pi
    // OS give the hostname 127.0.1.1), or a peer in a private range that no
    // forwarding header and no listed proxy stands behind, asking for a name
    // or address that only means something at home. A router that rewrites
    // the source of connections it forwards from the internet makes strangers
    // look like neighbours, but their browsers ask for the public name; one
    // who fakes the Host header by hand gets what a stranger gets through a
    // raw tunnel above, a password prompt. The name also turns away a page
    // that points its own DNS name at the receiver's address.
    const bool at_home = this_machine ||
                         (home_network_address(peer) && !forwarded(request) && !request.via_trusted_proxy);
    const bool home_host = names_home_host(host);
    if (home_network && at_home && home_host) return "";

    std::string refusal =
        "Use HTTPS to administer this receiver, or plain HTTP from the machine itself, for example through ssh -L.";
    const char* const home_names = "by its address, or by a name of one word or ending in .local.";
    if (this_machine) {
        refusal += " Open it as localhost on this machine or through ssh -L.";
    } else if (at_home && !home_network) {
        refusal += " For plain HTTP from the home network, set home_network = yes under [admin] and restart the "
                   "receiver";
        refusal += home_host ? "." : std::string(", then open it ") + home_names;
    } else if (at_home) {
        refusal += std::string(" From the home network, open it ") + home_names;
    }
    return refusal;
}

void AdminAuth::session_key(const uint8_t derived[32], const std::string& context, uint8_t out[32]) {
    const std::string label = "fernsdr-admin-session-v3\n" + context;
    hmac_sha256(derived, 32, reinterpret_cast<const uint8_t*>(label.data()), label.size(), out);
}

std::string AdminAuth::signing_context(const std::string& token) {
    Sha256 hash;
    hash.update("fernsdr-admin-v2\n" + token);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, sizeof(digest));
}

std::string cookie_value(const std::string& cookie_header, const std::string& name) {
    size_t position = 0;
    while (position < cookie_header.size()) {
        size_t end = cookie_header.find(';', position);
        if (end == std::string::npos) end = cookie_header.size();
        std::string pair = cookie_header.substr(position, end - position);

        size_t begin = pair.find_first_not_of(" \t");
        if (begin != std::string::npos) {
            pair = pair.substr(begin);
            const size_t equals = pair.find('=');
            if (equals != std::string::npos && pair.substr(0, equals) == name) {
                return pair.substr(equals + 1);
            }
        }
        position = end + 1;
    }
    return "";
}

int64_t AdminAuth::now() const {
    return clock_ ? clock_() : now_ms();
}

bool AdminAuth::locked_out(const std::string& network, AdminCaller caller, int64_t now, int& retry_after) {
    const auto found = attempts_.find(network);
    if (found != attempts_.end() && found->second.locked_until_ms > now) {
        retry_after = static_cast<int>((found->second.locked_until_ms - now + 999) / 1000);
        LOG_WARN("admin", "login from %s refused: locked out for %d more seconds", network.c_str(),
                 retry_after);
        return true;
    }
    while (!recent_failures_.empty() && now - recent_failures_.front() > kGlobalWindowMs) {
        recent_failures_.pop_front();
    }
    if (recent_failures_.size() >= kGlobalFailures && known_networks_.count(network) == 0 &&
        caller != AdminCaller::ThisMachine) {
        retry_after = static_cast<int>((recent_failures_.front() + kGlobalWindowMs - now + 999) / 1000);
        LOG_WARN("admin", "login from %s refused: %zu failed logins in the last hour", network.c_str(),
                 recent_failures_.size());
        return true;
    }
    return false;
}

void AdminAuth::note_failure(const std::string& network, int64_t now, int& retry_after) {
    recent_failures_.push_back(now);
    if (recent_failures_.size() > kGlobalFailures * 2) recent_failures_.pop_front();
    if (attempts_.find(network) == attempts_.end() && attempts_.size() >= kMaxAttemptsAddresses) {
        // Room is made from records that are not locking anybody out: evicting
        // one that is would let rotating addresses erase their own lockout.
        auto oldest = attempts_.end();
        for (auto it = attempts_.begin(); it != attempts_.end(); ++it) {
            if (it->second.locked_until_ms > now) continue;
            if (oldest == attempts_.end() || it->second.last_failure_ms < oldest->second.last_failure_ms) {
                oldest = it;
            }
        }
        if (oldest == attempts_.end()) return;
        attempts_.erase(oldest);
    }
    Attempts& record = attempts_[network];
    if (now - record.window_started_ms > kAttemptWindowMs) {
        record.count = 0;
        record.window_started_ms = now;
    }
    record.count++;
    record.last_failure_ms = now;
    if (record.count >= kMaxAttempts || record.lockouts > 0) {
        const int64_t lockout = kBaseLockoutMs << std::min(record.lockouts, kMaxDoublings);
        record.lockouts++;
        record.locked_until_ms = now + lockout;
        retry_after = static_cast<int>(lockout / 1000);
    }
    LOG_WARN("admin", "failed login from %s (%d in this window, lockout %d)", network.c_str(), record.count,
             record.lockouts);
}

std::string AdminAuth::issue_session(const std::string& network, const std::string& address, int64_t now) {
    attempts_.erase(network);
    if (known_networks_.count(network) == 0 && known_networks_.size() >= kKnownNetworks) {
        known_networks_.erase(std::min_element(known_networks_.begin(), known_networks_.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; }));
    }
    known_networks_[network] = now;

    const std::string token = random_hex(32);
    if (token.empty()) {
        LOG_ERROR("admin", "no source of randomness; refusing to issue a guessable session");
        return "";
    }
    if (sessions_.size() >= kMaxSessions) {
        auto oldest = std::min_element(sessions_.begin(), sessions_.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        counters_.erase(oldest->first);
        sessions_.erase(oldest);
    }
    sessions_[token] = now + static_cast<int64_t>(config_.session_hours) * 3600 * 1000;
    LOG_INFO("admin", "%s signed in", address.c_str());
    return token;
}

std::string AdminAuth::challenge_tag(const std::string& body, const std::string& address) const {
    std::vector<uint8_t> key;
    if (!from_hex(challenge_key_, key)) return "";
    const std::string message = "fernsdr-challenge\n" + address + "\n" + body;
    uint8_t mac[32];
    hmac_sha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(message.data()), message.size(), mac);
    return to_hex(mac, sizeof(mac));
}

// A challenge is 16 bytes, sent as the 32 hex digits the panel checks for:
// when it expires, in seconds after the key was drawn (4 bytes), 4 random
// bytes, and the first 8 bytes of the tag over those and the caller's
// address. The proof covers every digit, so a changed digit needs a new proof
// as well as a new tag; the tag keeps a challenge to its caller, and 8 bytes
// cannot be matched by trying addresses within the minute it lives.
bool AdminAuth::challenge_genuine(const std::string& nonce, const std::string& address,
                                  int64_t& expires_ms) const {
    std::vector<uint8_t> bytes;
    if (challenge_key_.empty() || nonce.size() != 32 || !from_hex(nonce, bytes) || bytes.size() != 16) return false;
    const std::string expected = challenge_tag(nonce.substr(0, 16), address);
    if (expected.size() < 16 || !constant_time_equal(expected.substr(0, 16), nonce.substr(16))) return false;
    const uint32_t seconds = static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
                             static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
    expires_ms = key_drawn_ms_ + static_cast<int64_t>(seconds) * 1000;
    return true;
}

bool AdminAuth::issue_challenge(const std::string& address, AdminChallenge& out, int& retry_after,
                                AdminCaller caller) {
    retry_after = 0;
    if (!enabled()) return false;

    const int64_t now = this->now();
    if (locked_out(network_key(address), caller, now, retry_after)) return false;

    std::string derived_hex;
    if (!parse_password_hash(config_.password_hash, out.iterations, out.salt, derived_hex)) {
        LOG_ERROR("admin", "the stored password hash is not readable; no challenge can be issued");
        return false;
    }

    if (challenge_key_.empty()) {
        challenge_key_ = random_hex(32);
        key_drawn_ms_ = now;
    }
    const std::string random = random_hex(4);
    if (challenge_key_.empty() || random.empty()) {
        LOG_ERROR("admin", "no source of randomness; refusing to issue a guessable challenge");
        return false;
    }
    // Counted from when the key was drawn: the clock itself counts from when
    // the machine booted, which is nobody else's business. Rounded up, so a
    // challenge lasts at least its minute.
    const int64_t seconds = (now + kNonceLifetimeMs - key_drawn_ms_ + 999) / 1000;
    const uint8_t expiry[4] = {static_cast<uint8_t>(seconds >> 24), static_cast<uint8_t>(seconds >> 16),
                               static_cast<uint8_t>(seconds >> 8), static_cast<uint8_t>(seconds)};
    const std::string body = to_hex(expiry, sizeof(expiry)) + random;
    out.nonce = body + challenge_tag(body, address).substr(0, 16);
    return true;
}

std::string AdminAuth::login_with_proof(const std::string& nonce, const std::string& proof,
                                        const std::string& address, int& retry_after, AdminCaller caller) {
    retry_after = 0;
    if (!enabled()) return "";

    const int64_t now = this->now();
    const std::string network = network_key(address);
    if (locked_out(network, caller, now, retry_after)) return "";

    // A forged, expired, answered or somebody else's challenge tested no
    // password, so it is refused without counting as a failure.
    int64_t expires_ms = 0;
    if (!challenge_genuine(nonce, address, expires_ms) || expires_ms <= now) return "";
    // Spent on sight, right or wrong: a challenge that survives a wrong answer
    // is a challenge an attacker can grind against offline and then use. What
    // is kept here is bounded by the sign-in limits, since every entry is a
    // sign-in or a counted failure.
    if (!spent_.emplace(nonce, expires_ms).second) return "";

    int iterations = 0;
    std::string salt, derived_hex;
    if (!parse_password_hash(config_.password_hash, iterations, salt, derived_hex)) return "";
    std::vector<uint8_t> key;
    if (!from_hex(derived_hex, key) || key.size() != 32) return "";

    uint8_t mac[32];
    hmac_sha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(),
                mac);
    if (!constant_time_equal(to_hex(mac, sizeof(mac)), proof)) {
        note_failure(network, now, retry_after);
        return "";
    }
    return issue_session(network, address, now);
}

std::string AdminAuth::token_from(const HttpRequest& request) const {
    return cookie_value(request.header("cookie"), "fernsdr_admin");
}

bool AdminAuth::authorised(const HttpRequest& request) const {
    if (!enabled()) return false;
    const std::string token = token_from(request);
    if (token.empty()) return false;
    const auto found = sessions_.find(token);
    if (found == sessions_.end()) return false;
    return found->second > now();
}

void AdminAuth::logout(const HttpRequest& request) {
    const std::string token = token_from(request);
    if (!token.empty()) {
        sessions_.erase(token);
        counters_.erase(token);
    }
}

bool AdminAuth::signature_valid(const HttpRequest& request, const std::string& body) {
    if (!enabled()) return false;
    const std::string token = token_from(request);
    if (!authorised(request)) return false;

    const std::string counter_text = request.header("x-fernsdr-counter");
    const std::string signature = request.header("x-fernsdr-signature");
    if (counter_text.empty() || signature.size() != 64) return false;

    uint64_t counter = 0;
    const auto parsed = std::from_chars(counter_text.data(), counter_text.data() + counter_text.size(), counter);
    if (parsed.ec != std::errc{} || parsed.ptr != counter_text.data() + counter_text.size() ||
        counter == 0 || counter > 9007199254740991ULL || counter_text.front() == '0') return false;
    // Strictly increasing, so a request captured off the wire cannot be sent
    // again. A gap is fine: it only means a request was abandoned.
    auto seen = counters_.find(token);
    if (seen != counters_.end() && counter <= seen->second) return false;

    int iterations = 0;
    std::string salt, derived_hex;
    if (!parse_password_hash(config_.password_hash, iterations, salt, derived_hex)) return false;
    std::vector<uint8_t> derived;
    if (!from_hex(derived_hex, derived) || derived.size() != 32) return false;
    const std::string context = signing_context(token);
    uint8_t key[32];
    session_key(derived.data(), context, key);

    // The method and path are covered as well as the body, or a captured
    // signature could be lifted onto a different endpoint.
    const std::string message =
        "fernsdr-admin-v3\n" + context + "\n" + counter_text + "\n" +
        request.method + "\n" + request.target + "\n" + body;
    uint8_t mac[32];
    hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(message.data()), message.size(), mac);
    if (!constant_time_equal(to_hex(mac, sizeof(mac)), signature)) return false;

    counters_[token] = counter;
    return true;
}

void AdminAuth::expire(int64_t now) {
    if (now - last_expiry_ms_ < 1000) return;
    last_expiry_ms_ = now;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = it->second <= now ? sessions_.erase(it) : std::next(it);
    }
    for (auto it = attempts_.begin(); it != attempts_.end();) {
        const Attempts& record = it->second;
        const bool stale = record.locked_until_ms < now &&
                           now - record.last_failure_ms > (record.lockouts > 0 ? kForgetLockoutsMs : kAttemptWindowMs);
        it = stale ? attempts_.erase(it) : std::next(it);
    }
    for (auto it = spent_.begin(); it != spent_.end();) {
        it = it->second <= now ? spent_.erase(it) : std::next(it);
    }
    // A counter outlives its session by nothing: the token is the key.
    for (auto it = counters_.begin(); it != counters_.end();) {
        it = sessions_.find(it->first) == sessions_.end() ? counters_.erase(it) : std::next(it);
    }
}

const char* const kHiddenCredential = "(kept on the machine)";

namespace {

bool is_credential(const std::string& key) { return key == "password_hash" || key == "password"; }

/**
 * Calls `rewrite(key, raw, value)` for each credential line in an [admin]
 * section and splices in what it returns in place of the raw value, which
 * includes any quotes; `value` is without them. Sections, comments and line
 * endings are found the way Config::parse finds them.
 */
template <class Rewrite>
std::string rewrite_admin_credentials(const std::string& text, Rewrite rewrite) {
    std::string out;
    out.reserve(text.size());
    std::string section;
    size_t start = 0;
    while (start < text.size()) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(start, end - start);

        size_t comment = line.size();
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"') in_quotes = !in_quotes;
            if (!in_quotes && (line[i] == '#' || line[i] == ';')) {
                comment = i;
                break;
            }
        }
        const std::string content = line.substr(0, comment);
        const std::string trimmed = trim(content);
        const size_t equals = content.find('=');
        if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim(trimmed.substr(1, trimmed.size() - 2));
        } else if (section == "admin" && equals != std::string::npos &&
                   is_credential(trim(content.substr(0, equals)))) {
            const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            size_t value_begin = equals + 1;
            while (value_begin < content.size() && blank(content[value_begin])) value_begin++;
            size_t value_end = content.size();
            while (value_end > value_begin && blank(content[value_end - 1])) value_end--;
            const std::string raw = content.substr(value_begin, value_end - value_begin);
            std::string value = raw;
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            line = content.substr(0, value_begin) + rewrite(trim(content.substr(0, equals)), raw, value) +
                   line.substr(value_end);
        }
        out += line;
        if (newline != std::string::npos) out += '\n';
        start = end + 1;
    }
    return out;
}

/** A setting the receiver reads as a path on this machine. */
bool names_a_file(const std::string& section, const std::string& key) {
    if (section.rfind("band:", 0) == 0) return key == "path" || key == "history_path";
    return section == "site" && key == "theme_file";
}

}  // namespace

std::string hide_admin_credentials(const std::string& text) {
    return rewrite_admin_credentials(text, [](const std::string&, const std::string& raw, const std::string& value) {
        return value.empty() ? raw : std::string(kHiddenCredential);
    });
}

std::string restore_admin_credentials(const std::string& text, const ConfigSection& current) {
    return rewrite_admin_credentials(text, [&current](const std::string& key, const std::string& raw,
                                                      const std::string& value) {
        return value == kHiddenCredential ? current.get(key, "") : raw;
    });
}

std::string machine_only_change(const Config& current, const Config& candidate) {
    // Every section of the name, in order: only the first is read, but a
    // second one turning up is a change as well.
    const auto every = [](const Config& config, const std::string& name) {
        std::vector<std::map<std::string, std::string>> found;
        for (const ConfigSection& section : config.sections()) {
            if (section.name() == name) found.push_back(section.values());
        }
        return found;
    };
    static const std::pair<const char*, const char*> kSections[] = {
        {"admin", "it decides who may administer this receiver. For a new password, run fernsdr --hash-password "
                  "there and put its line under [admin]."},
        {"modules", "it decides which programs this receiver may download and run."},
        {"server", "it decides what this receiver serves and whose forwarded addresses it believes, and only "
                   "a restart applies it."},
    };
    for (const auto& [name, reason] : kSections) {
        if (every(candidate, name) != every(current, name)) {
            return std::string("The [") + name + "] section can only be changed in the file on the machine: " +
                   reason;
        }
    }

    // Keyed by the section and by which one of that name it is, so a value
    // moved to another band counts as new.
    using Where = std::tuple<std::string, int, std::string>;  // section, which of that name, key
    const auto files = [](const Config& config) {
        std::map<Where, std::string> found;
        std::map<std::string, int> seen;
        for (const ConfigSection& section : config.sections()) {
            const int nth = seen[section.name()]++;
            for (const auto& [key, value] : section.values()) {
                if (names_a_file(section.name(), key)) found[Where{section.name(), nth, key}] = value;
            }
        }
        return found;
    };
    const auto before = files(current);
    for (const auto& [where, value] : files(candidate)) {
        const auto was = before.find(where);
        if (was == before.end() || was->second != value) {
            return "[" + std::get<0>(where) + "] " + std::get<2>(where) +
                   " names a file on the machine, so it can only be set in the file there.";
        }
    }

    // A UDP input takes samples from whoever sends them, and it would stay
    // open after the session that asked for it. The panel may close one, not
    // open or move one, nor let more senders in. The defaults count: without
    // `bind` it listens on every interface, and without `senders` it takes
    // everyone's samples.
    const auto inputs = [](const Config& config) {
        std::map<std::string, std::string> found;  // band -> where it listens
        for (const ConfigSection* band : config.sections_with_prefix("band")) {
            if (band->get("source", "test") != "udp") continue;
            found[band->name()] =
                band->get("bind", "0.0.0.0") + " " + band->get("port", "") + " " + band->get("multicast", "") +
                " " + band->get("senders", "");
        }
        return found;
    };
    const auto listening = inputs(current);
    for (const auto& [band, where] : inputs(candidate)) {
        const auto was = listening.find(band);
        if (was == listening.end() || was->second != where) {
            return "[" + band + "] takes its samples over the network, so where it listens can only be set in "
                   "the file on the machine.";
        }
    }
    return "";
}

}  // namespace fernsdr
