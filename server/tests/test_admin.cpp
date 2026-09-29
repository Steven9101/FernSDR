#include "../src/core/admin.h"
#include "../src/util/password.h"
#include "test_util.h"

#include <functional>
#include <string>
#include <vector>

using fernsdr::AdminAuth;
using fernsdr::AdminCaller;
using fernsdr::AdminChallenge;
using fernsdr::AdminConfig;

namespace {

const char* kPassword = "correct horse battery staple";

AdminAuth make_auth(std::function<int64_t()> clock = {}) {
    AdminConfig config;
    config.enabled = true;
    // A low iteration count: this is testing the protocol, not the cost
    // parameter, and 200 000 rounds per assertion would make the suite crawl.
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    config.session_hours = 8;
    return AdminAuth(config, std::move(clock));
}

/** One wrong proof: the lockout it earned in seconds, 0 for none, -1 when no challenge was given. */
int fail_once(AdminAuth& auth, const std::string& address) {
    int retry = 0;
    AdminChallenge challenge;
    if (!auth.issue_challenge(address, challenge, retry)) return -1;
    auth.login_with_proof(challenge.nonce, std::string(64, '0'), address, retry);
    return retry;
}

/** What an honest browser computes: HMAC of the nonce under the derived key. */
std::string proof_for(const AdminChallenge& challenge, const std::string& password) {
    uint8_t key[32];
    fernsdr::pbkdf2_sha256(password, challenge.salt, challenge.iterations, key, sizeof(key));
    uint8_t mac[32];
    fernsdr::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(challenge.nonce.data()),
                         challenge.nonce.size(), mac);
    return fernsdr::to_hex(mac, sizeof(mac));
}

}  // namespace

TEST_CASE(admin_challenge_describes_the_stored_hash) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    CHECK(auth.issue_challenge("10.0.0.1", challenge, retry));
    // The browser cannot derive the same key without both of these.
    CHECK(challenge.iterations == 1000);
    CHECK(!challenge.salt.empty());
    AdminChallenge again;
    CHECK(auth.issue_challenge("10.0.0.1", again, retry));
    CHECK(again.nonce != challenge.nonce);
    // What the panel checks before it answers (api.ts): 32 lowercase hex
    // digits. A panel still open from before an upgrade checks the same.
    for (const AdminChallenge* issued : {&challenge, &again}) {
        CHECK(issued->nonce.size() == 32);
        CHECK(issued->nonce.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
}

TEST_CASE(admin_a_correct_proof_signs_in) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    CHECK(auth.issue_challenge("10.0.0.1", challenge, retry));
    const std::string token = auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword),
                                                    "10.0.0.1", retry);
    CHECK(!token.empty());
}

TEST_CASE(admin_a_proof_cannot_be_replayed) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    auth.issue_challenge("10.0.0.1", challenge, retry);
    const std::string proof = proof_for(challenge, kPassword);

    CHECK(!auth.login_with_proof(challenge.nonce, proof, "10.0.0.1", retry).empty());
    // Someone who captured the exchange off the wire replays it verbatim.
    CHECK(auth.login_with_proof(challenge.nonce, proof, "10.0.0.1", retry).empty());
}

TEST_CASE(admin_a_challenge_belongs_to_the_caller_who_asked_for_it) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    auth.issue_challenge("10.0.0.1", challenge, retry);
    CHECK(auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword), "10.0.0.9", retry)
              .empty());
}

TEST_CASE(admin_a_wrong_proof_spends_the_challenge) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    auth.issue_challenge("10.0.0.1", challenge, retry);

    CHECK(auth.login_with_proof(challenge.nonce, std::string(64, '0'), "10.0.0.1", retry).empty());
    // The right answer afterwards must not work either: a challenge that
    // survives a wrong guess is one an attacker can grind offline and then use.
    CHECK(auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword), "10.0.0.1", retry)
              .empty());
}

TEST_CASE(admin_the_wrong_password_does_not_sign_in) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    auth.issue_challenge("10.0.0.1", challenge, retry);
    CHECK(auth.login_with_proof(challenge.nonce, proof_for(challenge, "hunter2"), "10.0.0.1", retry)
              .empty());
}

TEST_CASE(admin_proofs_are_rate_limited_like_passwords) {
    AdminAuth auth = make_auth();
    int retry = 0;
    // Five wrong answers, each with its own fresh challenge, is the point at
    // which the lockout starts - a guessing script must not escape it simply
    // by using the proof endpoint instead of the password one.
    for (int i = 0; i < 5; i++) {
        AdminChallenge challenge;
        auth.issue_challenge("10.0.0.2", challenge, retry);
        auth.login_with_proof(challenge.nonce, std::string(64, '0'), "10.0.0.2", retry);
    }
    CHECK(retry > 0);

    AdminChallenge challenge;
    CHECK(!auth.issue_challenge("10.0.0.2", challenge, retry));
    CHECK(retry > 0);
}

TEST_CASE(admin_asking_for_challenges_crowds_nobody_out) {
    AdminAuth auth = make_auth();
    int retry = 0;
    AdminChallenge before;
    CHECK(auth.issue_challenge("192.0.2.10", before, retry));
    // An unauthenticated caller decides how often to ask and from how many
    // networks. None of it may cost memory or keep an administrator out: here
    // hundreds from one address, then one each from thousands of /64s.
    const auto ask = [&](const std::string& address) {
        AdminChallenge challenge;
        return auth.issue_challenge(address, challenge, retry);
    };
    for (int i = 0; i < 500; i++) CHECK(ask("10.0.0.3"));
    for (int network = 0; network < 3000; network++) CHECK(ask("2001:db8:" + std::to_string(network) + "::1"));
    AdminChallenge after;
    CHECK(auth.issue_challenge("198.51.100.20", after, retry));
    CHECK(!auth.login_with_proof(after.nonce, proof_for(after, kPassword), "198.51.100.20", retry).empty());
    CHECK(!auth.login_with_proof(before.nonce, proof_for(before, kPassword), "192.0.2.10", retry).empty());
}

TEST_CASE(admin_a_challenge_cannot_be_altered_or_outlive_its_minute) {
    int64_t now = 123456789000;
    AdminAuth auth = make_auth([&now] { return now; });
    int retry = 0;
    AdminChallenge challenge;
    CHECK(auth.issue_challenge("10.0.0.1", challenge, retry));
    // It says when it expires, counted from when the key was drawn rather
    // than by the machine's clock, which counts from when it booted: the first
    // four bytes read the same whatever the clock says.
    int64_t other_clock = 5;
    AdminAuth other = make_auth([&other_clock] { return other_clock; });
    AdminChallenge elsewhere;
    CHECK(other.issue_challenge("10.0.0.1", elsewhere, retry));
    CHECK(challenge.nonce.substr(0, 8) == elsewhere.nonce.substr(0, 8));
    // Any change to what it says, a later expiry included, makes it not ours:
    // refused, not counted as a guess, and the genuine one is left unspent.
    for (size_t i = 0; i < challenge.nonce.size(); i += 5) {
        AdminChallenge altered = challenge;
        altered.nonce[i] = altered.nonce[i] == '1' ? '2' : '1';
        CHECK(auth.login_with_proof(altered.nonce, proof_for(altered, kPassword), "10.0.0.1", retry).empty());
        CHECK_EQ(retry, 0);
    }
    now += 59 * 1000;
    CHECK(!auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword), "10.0.0.1", retry).empty());

    AdminChallenge late;
    CHECK(auth.issue_challenge("10.0.0.1", late, retry));
    now += 61 * 1000;
    CHECK(auth.login_with_proof(late.nonce, proof_for(late, kPassword), "10.0.0.1", retry).empty());
    CHECK_EQ(retry, 0);

    // A receiver that restarted draws a new key.
    AdminChallenge old;
    CHECK(auth.issue_challenge("10.0.0.1", old, retry));
    AdminAuth restarted = make_auth([&now] { return now; });
    AdminChallenge unrelated;
    CHECK(restarted.issue_challenge("10.0.0.1", unrelated, retry));
    CHECK(restarted.login_with_proof(old.nonce, proof_for(old, kPassword), "10.0.0.1", retry).empty());
}

TEST_CASE(admin_no_challenge_when_the_panel_is_off) {
    AdminConfig config;
    config.enabled = false;
    AdminAuth auth(config);
    AdminChallenge challenge;
    int retry = 0;
    CHECK(!auth.issue_challenge("10.0.0.1", challenge, retry));
    CHECK(auth.login_with_proof("deadbeef", std::string(64, '0'), "10.0.0.1", retry).empty());
}

namespace {

/** A request as the browser would send it, signed or not. */
fernsdr::HttpRequest signed_request(const std::string& token, const std::string& method,
                                    const std::string& path, const std::string& body,
                                    uint64_t counter, const std::string& password,
                                    const std::string& hash, bool corrupt = false) {
    fernsdr::HttpRequest request;
    request.method = method;
    request.path = path;
    request.target = path;
    request.body = body;
    request.headers.push_back({"cookie", "fernsdr_admin=" + token});

    int iterations = 0;
    std::string salt, derived;
    fernsdr::parse_password_hash(hash, iterations, salt, derived);
    uint8_t derived_key[32], key[32];
    fernsdr::pbkdf2_sha256(password, salt, iterations, derived_key, sizeof(derived_key));
    AdminAuth::session_key(derived_key, AdminAuth::signing_context(token), key);

    const std::string counter_text = std::to_string(counter);
    const std::string message = "fernsdr-admin-v3\n" + AdminAuth::signing_context(token) + "\n" +
        counter_text + "\n" + method + "\n" + path + "\n" + body;
    uint8_t mac[32];
    fernsdr::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(message.data()),
                         message.size(), mac);
    std::string signature = fernsdr::to_hex(mac, sizeof(mac));
    if (corrupt) signature[0] = signature[0] == 'a' ? 'b' : 'a';

    request.headers.push_back({"x-fernsdr-counter", counter_text});
    request.headers.push_back({"x-fernsdr-signature", signature});
    return request;
}

std::string sign_in(AdminAuth& auth, const std::string& password) {
    AdminChallenge challenge;
    int retry = 0;
    auth.issue_challenge("10.0.0.1", challenge, retry);
    return auth.login_with_proof(challenge.nonce, proof_for(challenge, password), "10.0.0.1", retry);
}

}  // namespace

TEST_CASE(admin_a_signed_request_is_accepted) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);
    CHECK(!token.empty());

    auto request = signed_request(token, "POST", "/api/admin/bands", "{\"id\":\"20m\"}", 1,
                                  kPassword, config.password_hash);
    CHECK(auth.signature_valid(request, request.body));
}

TEST_CASE(admin_signs_with_a_key_that_dies_with_the_session) {
    // What the browser keeps is the session's key, not the password's derived
    // key, which signs in by itself and never expires. A change signed with
    // the derived key is refused, and one session's key signs nothing for
    // another session.
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string first = sign_in(auth, kPassword);
    const std::string second = sign_in(auth, kPassword);

    int iterations = 0;
    std::string salt, derived_hex;
    fernsdr::parse_password_hash(config.password_hash, iterations, salt, derived_hex);
    uint8_t derived[32];
    fernsdr::pbkdf2_sha256(kPassword, salt, iterations, derived, sizeof(derived));
    const auto signed_with = [&](const std::string& token, const uint8_t* key, uint64_t counter) {
        fernsdr::HttpRequest request;
        request.method = "POST";
        request.path = request.target = "/api/admin/bands";
        request.body = "{\"id\":\"20m\"}";
        request.headers.push_back({"cookie", "fernsdr_admin=" + token});
        const std::string message = "fernsdr-admin-v3\n" + AdminAuth::signing_context(token) + "\n" +
                                    std::to_string(counter) + "\nPOST\n/api/admin/bands\n" + request.body;
        uint8_t mac[32];
        fernsdr::hmac_sha256(key, 32, reinterpret_cast<const uint8_t*>(message.data()), message.size(), mac);
        request.headers.push_back({"x-fernsdr-counter", std::to_string(counter)});
        request.headers.push_back({"x-fernsdr-signature", fernsdr::to_hex(mac, sizeof(mac))});
        return request;
    };

    auto with_derived = signed_with(first, derived, 1);
    CHECK(!auth.signature_valid(with_derived, with_derived.body));

    uint8_t first_key[32];
    AdminAuth::session_key(derived, AdminAuth::signing_context(first), first_key);
    auto on_second = signed_with(second, first_key, 2);
    CHECK(!auth.signature_valid(on_second, on_second.body));
    auto on_first = signed_with(first, first_key, 3);
    CHECK(auth.signature_valid(on_first, on_first.body));
}

TEST_CASE(admin_a_stolen_cookie_alone_changes_nothing) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);

    // Someone who read the cookie off the wire has the session and nothing
    // else. That is the whole point of signing.
    fernsdr::HttpRequest request;
    request.method = "POST";
    request.path = "/api/admin/bands";
    request.body = "{\"id\":\"20m\"}";
    request.headers.push_back({"cookie", "fernsdr_admin=" + token});
    CHECK(auth.authorised(request));
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_captured_request_cannot_be_replayed) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);

    auto request = signed_request(token, "POST", "/api/admin/bands", "{\"id\":\"20m\"}", 7,
                                  kPassword, config.password_hash);
    CHECK(auth.signature_valid(request, request.body));
    // The same bytes again, exactly as an eavesdropper would send them.
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_signature_cannot_be_moved_to_another_request) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);

    auto request = signed_request(token, "POST", "/api/admin/bands", "{\"id\":\"20m\"}", 3,
                                  kPassword, config.password_hash);
    // Same signature, different target: the method and path are covered too,
    // or a harmless request's signature could be lifted onto a dangerous one.
    request.path = "/api/admin/restart-band";
    request.target = request.path;
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_tampered_body_is_refused) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);

    auto request = signed_request(token, "POST", "/api/admin/bands", "{\"id\":\"20m\"}", 4,
                                  kPassword, config.password_hash);
    request.body = "{\"id\":\"40m\"}";
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_wrong_signature_is_refused) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    const std::string token = sign_in(auth, kPassword);
    auto request = signed_request(token, "POST", "/api/admin/bands", "{}", 1, kPassword,
                                  config.password_hash, /*corrupt=*/true);
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_signature_without_a_session_is_refused) {
    AdminConfig config;
    config.enabled = true;
    config.password_hash = fernsdr::hash_password(kPassword, 1000);
    AdminAuth auth(config);
    auto request = signed_request("deadbeef", "POST", "/api/admin/bands", "{}", 1, kPassword,
                                  config.password_hash);
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_signature_cannot_be_replayed_with_another_session_cookie) {
    AdminAuth auth = make_auth();
    const std::string first = sign_in(auth, kPassword);
    const std::string second = sign_in(auth, kPassword);
    auto request = signed_request(first, "POST", "/api/admin/bands", "{}", 100,
                                  kPassword, auth.config().password_hash);
    request.headers[0].second = "fernsdr_admin=" + second;
    CHECK(auth.authorised(request));
    CHECK(!auth.signature_valid(request, request.body));
    request.headers[0].second = "fernsdr_admin=" + first;
    CHECK(auth.signature_valid(request, request.body));
}

TEST_CASE(admin_a_signature_covers_query_parameters) {
    AdminAuth auth = make_auth();
    const std::string token = sign_in(auth, kPassword);
    auto request = signed_request(token, "POST", "/api/admin/bands?dry_run=1", "{}", 100,
                                  kPassword, auth.config().password_hash);
    request.target = "/api/admin/bands?dry_run=0";
    CHECK(!auth.signature_valid(request, request.body));
}

TEST_CASE(admin_remote_requests_require_a_trusted_secure_transport) {
    // HTTPS, from any peer; request.secure is only set when a trusted proxy said so.
    fernsdr::HttpRequest secure;
    secure.secure = true;
    CHECK(fernsdr::admin_transport_allowed(secure, "192.0.2.4", false));

    const auto local = [](const std::string& host, const std::vector<std::pair<std::string, std::string>>& extra = {}) {
        fernsdr::HttpRequest request;
        if (!host.empty()) request.headers.push_back({"host", host});
        for (const auto& header : extra) request.headers.push_back(header);
        return request;
    };
    // Plain HTTP from this machine, or through ssh -L, which keeps the name.
    CHECK(fernsdr::admin_transport_allowed(local("localhost:8073"), "127.0.0.1", false));
    CHECK(fernsdr::admin_transport_allowed(local("LOCALHOST"), "127.0.0.1", false));
    CHECK(fernsdr::admin_transport_allowed(local("127.0.0.1:9000"), "127.0.0.1", false));
    CHECK(fernsdr::admin_transport_allowed(local("[::1]:8073"), "::1", false));
    CHECK(fernsdr::admin_transport_allowed(local("radio.localhost:8073"), "::1", false));
    // Plain HTTP from anywhere else is refused, unless the operator said
    // plain_http_anywhere = yes in the file.
    CHECK(!fernsdr::admin_transport_allowed(local("localhost:8073"), "192.0.2.4", false));
    CHECK(fernsdr::admin_transport_allowed(local("sdr.example.org"), "192.0.2.4", false, true));
    CHECK(fernsdr::admin_transport_allowed(local(""), "192.0.2.4", false, true));
    // A raw TCP tunnel arrives from loopback but carries the public name.
    CHECK(!fernsdr::admin_transport_allowed(local("sdr.example.org"), "127.0.0.1", false));
    CHECK(!fernsdr::admin_transport_allowed(local("127.0.0.1.example.org"), "127.0.0.1", false));
    CHECK(!fernsdr::admin_transport_allowed(local("notlocalhost"), "127.0.0.1", false));
    CHECK(!fernsdr::admin_transport_allowed(local(""), "127.0.0.1", false));
    // Nor what no browser sends: text after the brackets, an empty or odd port.
    for (const char* host : {"[::1]evil.example.org", "[::1]x8073", "[::1]:", "localhost:", "localhost:80x",
                             "localhost:123456", "[::1"}) {
        CHECK(!fernsdr::admin_transport_allowed(local(host), "127.0.0.1", false));
    }
    // Anything that says it was forwarded is judged by the proxy's word on the
    // scheme, which request.secure carries, not by the loopback peer.
    for (const auto& header : std::vector<std::pair<std::string, std::string>>{{"x-forwarded-for", "198.51.100.7"},
                                                                              {"x-forwarded-proto", "https"},
                                                                              {"x-real-ip", "198.51.100.7"},
                                                                              {"forwarded", "for=198.51.100.7"},
                                                                              {"x-forwarded-host", "sdr.example.org"}}) {
        CHECK(!fernsdr::admin_transport_allowed(local("localhost", {header}), "127.0.0.1", false));
    }
}

// home_network = yes: plain HTTP from a browser in the home network too, told
// apart by the socket's peer and by the name the browser asked for.
TEST_CASE(admin_home_network_allows_plain_http_from_private_ranges_by_local_names) {
    const auto plain = [](const std::string& host, const std::vector<std::pair<std::string, std::string>>& extra = {}) {
        fernsdr::HttpRequest request;
        if (!host.empty()) request.headers.push_back({"host", host});
        for (const auto& header : extra) request.headers.push_back(header);
        return request;
    };
    const auto allowed = [&](const std::string& host, const std::string& peer) {
        return fernsdr::admin_transport_allowed(plain(host), peer, true);
    };
    // Off, which is the default, a neighbour is a stranger.
    CHECK(!fernsdr::admin_transport_allowed(plain("192.168.1.20:8073"), "192.168.1.5", false));
    CHECK(allowed("192.168.1.20:8073", "192.168.1.5"));

    // Each range at both ends, the IPv4-mapped form, and just outside each;
    // 100.64/10 is carrier-grade NAT, whose far side is other customers.
    for (const char* peer : {"10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255", "192.168.0.0",
                             "192.168.255.255", "169.254.0.1", "169.254.255.254", "fc00::", "fdff:ffff::1",
                             "fe80::1", "febf:ffff::1", "::ffff:192.168.1.5", "::ffff:10.1.2.3"}) {
        CHECK(allowed("fernsdr:8073", peer));
    }
    for (const char* peer : {"9.255.255.255", "11.0.0.0", "172.15.255.255", "172.32.0.0", "192.167.255.255",
                             "192.169.0.0", "169.253.255.255", "169.255.0.0", "100.64.0.1", "100.127.255.254",
                             "fbff:ffff::1", "fec0::1", "2001:db8::1", "::ffff:198.51.100.7", "198.51.100.7"}) {
        CHECK(!allowed("fernsdr:8073", peer));
    }

    // Names and addresses that mean something at home alone: one label, the
    // endings nobody can register (.local, .home.arpa, .internal), home
    // literals and this machine's names.
    for (const char* host : {"fernsdr", "fernsdr:8073", "FernSDR", "web-sdr", "sdr.local", "SDR.LOCAL:8073",
                             "sdr.local.", "pi.shack.local", "sdr.home.arpa", "sdr.internal", "192.168.1.20",
                             "10.0.0.2:8073", "[fd00::20]:8073", "[fe80::20]", "localhost:8073"}) {
        CHECK(allowed(host, "192.168.1.5"));
    }
    // The public name, which a router forwarding from the internet passes on;
    // public and carrier-grade NAT literals; endings a page could point at
    // the receiver, registered (.fritz.box) or merely unused today (.lan,
    // .home, .localdomain); and what no browser sends.
    for (const char* host : {"sdr.example.org", "sdr.example.org:8073", "203.0.113.7", "203.0.113.7:8073",
                             "[2001:db8::7]:8073", "100.64.0.1", "10.0.0.2.nip.io", "sdr.local.example.org",
                             "raspberrypi.fritz.box", "fritz.box", "sdr.lan", "sdr.home", "sdr.localdomain",
                             "sdr_1", "a..local", ".local", "sdr.local:", "sdr.local:80a", "[fd00::20]evil.example.org",
                             "", ":8073", "[fd00::20"}) {
        CHECK(!allowed(host, "192.168.1.5"));
    }

    // Anything that says it was forwarded is judged by request.secure alone,
    // and so is a listed proxy that says nothing: nginx passes requests from
    // the internet on with the upstream's address as Host and, unless told
    // to, no forwarding header.
    CHECK(!fernsdr::admin_transport_allowed(plain("fernsdr", {{"x-forwarded-for", "198.51.100.7"}}), "192.168.1.5",
                                            true));
    CHECK(!fernsdr::admin_transport_allowed(plain("fernsdr", {{"forwarded", "for=198.51.100.7"}}), "192.168.1.5",
                                            true));
    fernsdr::HttpRequest proxied = plain("192.168.1.20:8073");
    proxied.via_trusted_proxy = true;
    CHECK(!fernsdr::admin_transport_allowed(proxied, "192.168.1.2", true));
    // This machine: localhost as before, and with the option its own name,
    // which Debian gives 127.0.1.1; not a public name, as through a raw tunnel.
    CHECK(allowed("localhost:8073", "127.0.0.1"));
    CHECK(allowed("raspberrypi:8073", "127.0.1.1"));
    CHECK(!fernsdr::admin_transport_allowed(plain("raspberrypi:8073"), "127.0.1.1", false));
    CHECK(!allowed("sdr.example.org", "127.0.0.1"));
}

// The 403 says what would work, and only what would.
TEST_CASE(admin_a_refused_transport_says_what_would_work) {
    const auto plain = [](const std::string& host, const std::vector<std::pair<std::string, std::string>>& extra = {}) {
        fernsdr::HttpRequest request;
        request.headers.push_back({"host", host});
        for (const auto& header : extra) request.headers.push_back(header);
        return request;
    };
    const std::string https_or_ssh =
        "Use HTTPS to administer this receiver, or plain HTTP from the machine itself, for example through ssh -L.";
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("fernsdr"), "192.168.1.5", true), "");
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("fernsdr"), "192.168.1.5", false),
                 https_or_ssh + " For plain HTTP from the home network, set home_network = yes under [admin] and "
                                "restart the receiver.");
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("sdr.example.org"), "192.168.1.5", false),
                 https_or_ssh + " For plain HTTP from the home network, set home_network = yes under [admin] and "
                                "restart the receiver, then open it by its address, or by a name of one word or "
                                "ending in .local.");
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("sdr.fritz.box"), "192.168.1.5", true),
                 https_or_ssh + " From the home network, open it by its address, or by a name of one word or ending "
                                "in .local.");
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("raspberrypi:8073"), "127.0.1.1", false),
                 https_or_ssh + " Open it as localhost on this machine or through ssh -L.");
    // A proxy in the home network, or a stranger, gets no hint about names.
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("sdr.lan", {{"x-forwarded-for", "192.168.1.9"}}),
                                                  "192.168.1.5", true),
                 https_or_ssh);
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("fernsdr"), "198.51.100.7", false), https_or_ssh);
    CHECK_EQ_STR(fernsdr::admin_transport_refusal(plain("fernsdr"), "100.64.0.1", true), https_or_ssh);
}

TEST_CASE(admin_wrong_address_does_not_spend_the_owners_challenge) {
    AdminAuth auth = make_auth();
    AdminChallenge challenge;
    int retry = 0;
    CHECK(auth.issue_challenge("10.0.0.1", challenge, retry));
    const auto proof = proof_for(challenge, kPassword);
    CHECK(auth.login_with_proof(challenge.nonce, proof, "10.0.0.2", retry).empty());
    CHECK(!auth.login_with_proof(challenge.nonce, proof, "10.0.0.1", retry).empty());
}

TEST_CASE(admin_the_config_editor_never_sees_the_credentials) {
    const std::string hash = fernsdr::hash_password(kPassword, 1000);
    const std::string text =
        "[site]\nname = Test ; password_hash = not this one\n"
        "[band:demo]\npassword_hash = belongs to nobody\n"
        "[admin]\r\npassword_hash = \"" + hash + "\"   ; set 2026-09-25\r\n"
        "password=plain text\nsession_hours = 8\n"
        "[admin]\npassword_hash =\n"
        "[server]\nport = 8073";
    const std::string hidden = fernsdr::hide_admin_credentials(text);
    CHECK(hidden.find(hash) == std::string::npos);
    CHECK(hidden.find("plain text") == std::string::npos);
    // Only the credentials change, and only in [admin]: comments, quotes around
    // nothing else, CRLF line ends, an empty value and a missing final newline
    // come back as they were.
    CHECK(hidden ==
          "[site]\nname = Test ; password_hash = not this one\n"
          "[band:demo]\npassword_hash = belongs to nobody\n"
          "[admin]\r\npassword_hash = (kept on the machine)   ; set 2026-09-25\r\n"
          "password=(kept on the machine)\nsession_hours = 8\n"
          "[admin]\npassword_hash =\n"
          "[server]\nport = 8073");

    fernsdr::Config current;
    std::string error;
    CHECK(current.parse(text, error));
    const std::string restored = fernsdr::restore_admin_credentials(hidden, current.section("admin"));
    fernsdr::Config back;
    CHECK(back.parse(restored, error));
    CHECK(back.section("admin").get("password_hash", "") == hash);
    CHECK(back.section("admin").get("password", "") == "plain text");
    CHECK(back.section("admin").get("session_hours", "") == "8");
    CHECK(back.section("band:demo").get("password_hash", "") == "belongs to nobody");

    // A value that is not the placeholder is left for the caller to judge.
    const std::string replaced = fernsdr::restore_admin_credentials(
        "[admin]\npassword_hash = pbkdf2$1$aa$bb\n", current.section("admin"));
    CHECK(replaced == "[admin]\npassword_hash = pbkdf2$1$aa$bb\n");
}

TEST_CASE(admin_a_dead_challenge_is_not_a_failed_guess) {
    // No password is tested by a nonce that is unknown or belongs to someone
    // else, so it must not count towards a lockout: a flood that pushed out an
    // administrator's challenge would otherwise lock them out as they retried.
    AdminAuth auth = make_auth();
    int retry = 0;
    for (int i = 0; i < 10; i++) {
        CHECK(auth.login_with_proof(std::string(32, 'a'), std::string(64, '0'), "10.0.0.1", retry).empty());
        CHECK_EQ(retry, 0);
    }
    AdminChallenge challenge;
    CHECK(auth.issue_challenge("10.0.0.1", challenge, retry));
    CHECK(!auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword), "10.0.0.1", retry).empty());
}

TEST_CASE(admin_addresses_in_one_ipv6_network_share_a_lockout) {
    AdminAuth auth = make_auth();
    int retry = 0;
    for (int i = 1; i <= 5; i++) {
        const std::string address = "2001:db8:1:2::" + std::to_string(i);
        AdminChallenge challenge;
        CHECK(auth.issue_challenge(address, challenge, retry));
        auth.login_with_proof(challenge.nonce, std::string(64, '0'), address, retry);
    }
    AdminChallenge challenge;
    CHECK(!auth.issue_challenge("2001:db8:1:2:ffff::99", challenge, retry));
    CHECK(retry > 0);
    // The next /64 is somebody else.
    CHECK(auth.issue_challenge("2001:db8:1:3::1", challenge, retry));
}

TEST_CASE(admin_failures_from_everywhere_share_a_budget) {
    AdminAuth auth = make_auth();
    int retry = 0;
    AdminChallenge before;
    CHECK(auth.issue_challenge("192.0.2.1", before, retry));
    CHECK(!auth.login_with_proof(before.nonce, proof_for(before, kPassword), "192.0.2.1", retry).empty());

    // One wrong guess from each of a hundred networks: no single one is
    // locked out, but together they spend the hour's budget.
    for (int i = 0; i < 100; i++) {
        const std::string address = "2001:db8:" + std::to_string(i) + "::1";
        AdminChallenge challenge;
        CHECK(auth.issue_challenge(address, challenge, retry));
        auth.login_with_proof(challenge.nonce, std::string(64, '0'), address, retry);
    }
    AdminChallenge stranger;
    CHECK(!auth.issue_challenge("198.51.100.1", stranger, retry));
    CHECK(retry > 0);
    CHECK(retry <= 3600);
    // A network an administrator signed in from carries on.
    AdminChallenge known;
    CHECK(auth.issue_challenge("192.0.2.1", known, retry));
    CHECK(!auth.login_with_proof(known.nonce, proof_for(known, kPassword), "192.0.2.1", retry).empty());
}

TEST_CASE(admin_a_lockout_outlives_its_window) {
    int64_t now = 1000000;
    AdminAuth auth = make_auth([&now] { return now; });
    const std::string address = "203.0.113.5";
    for (int i = 0; i < 4; i++) CHECK_EQ(fail_once(auth, address), 0);
    CHECK_EQ(fail_once(auth, address), 60);
    CHECK_EQ(fail_once(auth, address), -1);
    // Every guess after that locks again for twice as long, up to 64
    // minutes, well past the five-minute window the first five fell in.
    int lockout = 60;
    for (int expected : {120, 240, 480, 960, 1920, 3840, 3840}) {
        now += lockout * 1000LL;
        CHECK_EQ(fail_once(auth, address), expected);
        lockout = expected;
    }

    // Signing in from there clears it.
    now += 3840 * 1000LL;
    AdminChallenge challenge;
    int retry = 0;
    CHECK(auth.issue_challenge(address, challenge, retry));
    CHECK(!auth.login_with_proof(challenge.nonce, proof_for(challenge, kPassword), address, retry).empty());
    for (int i = 0; i < 4; i++) CHECK_EQ(fail_once(auth, address), 0);
    CHECK_EQ(fail_once(auth, address), 60);

    // So does a day without a wrong guess.
    now += 60 * 1000LL;
    CHECK_EQ(fail_once(auth, address), 120);
    now += 120 * 1000LL + 24 * 3600 * 1000LL + 1;
    auth.expire(now);
    for (int i = 0; i < 4; i++) CHECK_EQ(fail_once(auth, address), 0);
    CHECK_EQ(fail_once(auth, address), 60);
}

TEST_CASE(admin_one_address_cannot_spend_the_shared_budget_alone) {
    int64_t now = 1000000;
    AdminAuth auth = make_auth([&now] { return now; });
    // Guessing as fast as the lockouts allow, for an hour.
    const int64_t end = now + 3600 * 1000LL;
    int guesses = 0;
    while (now < end) {
        const int lockout = fail_once(auth, "203.0.113.5");
        CHECK(lockout >= 0);
        if (lockout < 0) break;
        guesses++;
        now += lockout > 0 ? lockout * 1000LL : 1;
    }
    CHECK(guesses <= 10);
    AdminChallenge stranger;
    int retry = 0;
    CHECK(auth.issue_challenge("198.51.100.7", stranger, retry));
}

TEST_CASE(admin_this_machine_can_sign_in_when_the_shared_budget_is_spent) {
    AdminAuth auth = make_auth();
    for (int i = 0; i < 100; i++) fail_once(auth, "2001:db8:" + std::to_string(i) + "::1");
    int retry = 0;
    AdminChallenge stranger;
    CHECK(!auth.issue_challenge("198.51.100.1", stranger, retry));
    // The address is not what counts: a proxy on this machine connects from
    // loopback too, on behalf of anyone.
    AdminChallenge proxied;
    CHECK(!auth.issue_challenge("127.0.0.1", proxied, retry, AdminCaller::Remote));
    AdminChallenge local;
    CHECK(auth.issue_challenge("127.0.0.1", local, retry, AdminCaller::ThisMachine));
    CHECK(!auth.login_with_proof(local.nonce, proof_for(local, kPassword), "127.0.0.1", retry,
                                 AdminCaller::ThisMachine).empty());
}

TEST_CASE(admin_the_panel_cannot_change_what_the_machine_decides) {
    const auto parsed = [](const std::string& text) {
        fernsdr::Config config;
        std::string error;
        CHECK(config.parse(text, error));
        return config;
    };
    const std::string base =
        "[site]\nname = A\ntheme_file = look.json\n"
        "[server]\nport = 8073\n"
        "[admin]\npassword_hash = pbkdf2$1$aa$bb\n"
        "[modules]\ncatalog = a/b\n"
        "[band:a]\nsource = file\npath = /srv/a.cu8\nsample_rate = 2M\n";
    const fernsdr::Config current = parsed(base);
    const auto change = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        text.replace(text.find(from), from.size(), to);
        return fernsdr::machine_only_change(current, parsed(text));
    };
    const auto added = [&](const std::string& text) {
        return fernsdr::machine_only_change(current, parsed(base + text));
    };
    const auto names = [](const std::string& refusal, const std::string& what) {
        return refusal.find(what) != std::string::npos;
    };

    // What the panel is for goes through, and so does dropping a file
    // setting, which brings back the default the program chose.
    CHECK(change("name = A", "name = B").empty());
    CHECK(change("sample_rate = 2M", "sample_rate = 2M\nnoise_blanker = 0.5").empty());
    CHECK(change("theme_file = look.json\n", "").empty());

    CHECK(names(change("port = 8073", "port = 8073\nuploads = ."), "[server] section"));
    CHECK(names(change("port = 8073", "port = 8074"), "[server] section"));
    CHECK(names(added("[server]\nroot = /\n"), "[server] section"));
    CHECK(names(change("catalog = a/b", "catalog = a/b c/d"), "[modules] section"));
    CHECK(names(change("pbkdf2$1$aa$bb", "pbkdf2$1$aa$cc"), "fernsdr --hash-password"));
    CHECK(names(change("[admin]\n", "[admin]\nsession_hours = 168\n"), "[admin] section"));
    CHECK(names(added("[admin]\n"), "[admin] section"));

    CHECK(names(change("look.json", "/etc/x"), "[site] theme_file"));
    CHECK(names(change("/srv/a.cu8", "/srv/b.cu8"), "[band:a] path"));
    CHECK(names(change("sample_rate = 2M", "sample_rate = 2M\nhistory_path = a.wfa"), "[band:a] history_path"));
    CHECK(names(added("[band:b]\nsource = file\npath = /srv/a.cu8\n"), "[band:b] path"));
    CHECK(names(added("[site]\ntheme_file = look.json\n"), "[site] theme_file"));
}

TEST_CASE(admin_every_loopback_address_is_one_network) {
    AdminAuth auth = make_auth();
    for (int i = 0; i < 100; i++) fail_once(auth, "2001:db8:" + std::to_string(i) + "::1");
    // Any process on the machine can pick its source address anywhere in
    // 127/8. Five fresh tries per address would be no limit at all, and the
    // machine is exempt from the shared budget.
    int retry = 0;
    for (int i = 1; i <= 5; i++) {
        const std::string address = "127.0." + std::to_string(i) + ".1";
        AdminChallenge challenge;
        CHECK(auth.issue_challenge(address, challenge, retry, AdminCaller::ThisMachine));
        auth.login_with_proof(challenge.nonce, std::string(64, '0'), address, retry, AdminCaller::ThisMachine);
    }
    CHECK(retry > 0);
    for (const char* address : {"127.9.9.9", "::1", "::ffff:127.0.0.2"}) {
        AdminChallenge challenge;
        CHECK(!auth.issue_challenge(address, challenge, retry, AdminCaller::ThisMachine));
    }
}

TEST_CASE(admin_the_panel_cannot_open_or_move_a_network_input) {
    const auto parsed = [](const std::string& text) {
        fernsdr::Config config;
        std::string error;
        CHECK(config.parse(text, error));
        return config;
    };
    const std::string base =
        "[band:u]\nsource = udp\nbind = 127.0.0.1\nport = 5004\nsample_rate = 48k\n"
        "[band:t]\nsource = test\nsample_rate = 192k\n";
    const fernsdr::Config current = parsed(base);
    const auto change = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        text.replace(text.find(from), from.size(), to);
        return fernsdr::machine_only_change(current, parsed(text));
    };
    CHECK(change("sample_rate = 48k", "sample_rate = 96k").empty());
    // Closing one is fine: another source, or the band gone.
    CHECK(change("source = udp", "source = test").empty());
    CHECK(change("[band:u]\nsource = udp\nbind = 127.0.0.1\nport = 5004\nsample_rate = 48k\n", "").empty());

    CHECK(change("port = 5004", "port = 5005").find("[band:u]") != std::string::npos);
    CHECK(change("bind = 127.0.0.1\n", "").find("[band:u]") != std::string::npos);
    CHECK(change("port = 5004", "port = 5004\nmulticast = 239.1.2.3").find("[band:u]") != std::string::npos);
    CHECK(change("source = test", "source = udp\nport = 5004").find("[band:t]") != std::string::npos);
    // Nor let more senders in, or any at all where only some were allowed.
    CHECK(change("port = 5004", "port = 5004\nsenders = 0.0.0.0/0").find("[band:u]") != std::string::npos);
}

TEST_CASE(admin_the_panel_cannot_widen_a_network_inputs_senders) {
    fernsdr::Config current, candidate;
    std::string error;
    CHECK(current.parse("[band:u]\nsource = udp\nbind = 127.0.0.1\nport = 5004\nsample_rate = 48k\n"
                        "senders = 10.0.0.5\n", error));
    CHECK(candidate.parse("[band:u]\nsource = udp\nbind = 127.0.0.1\nport = 5004\nsample_rate = 48k\n", error));
    CHECK(fernsdr::machine_only_change(current, candidate).find("[band:u]") != std::string::npos);
    CHECK(candidate.parse("[band:u]\nsource = udp\nbind = 127.0.0.1\nport = 5004\nsample_rate = 48k\n"
                          "senders = 10.0.0.5, 10.0.0.6\n", error));
    CHECK(fernsdr::machine_only_change(current, candidate).find("[band:u]") != std::string::npos);
}

