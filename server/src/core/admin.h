// The admin panel's server side: authentication and the endpoints behind it.
//
// The threat model is deliberately explicit, because "admin panel" usually
// means "the part that gets owned":
//
//   - The panel is on the same port as the public receiver, so it is reachable
//     by anyone who can reach the receiver. Every route therefore authenticates
//     on every request; nothing is protected by being hard to guess.
//   - The password is stored as PBKDF2, never in the clear. Its derived key
//     also signs requests, so a leaked configuration permits authentication
//     without recovering the original password. Treat it as a secret.
//   - Guessing is rate-limited per network (an IPv4 address, an IPv6 /64, or
//     this machine as a whole) and across all of them, because a rate limiter
//     is what actually stops a weak password being found, not the hash.
//   - A session is a 256-bit random token in an HttpOnly, SameSite=Strict
//     cookie. JavaScript cannot read it, but an injected script can still
//     act through the logged-in page. Remote access requires TLS.
//   - The panel is disabled unless a password hash is configured. A default
//     password would be found by a scanner within hours of the receiver going
//     online, and every receiver running this software would share it.
#pragma once
#include <cstdint>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../net/http.h"
#include "../util/config.h"

namespace fernsdr {

class Radio;
class Application;

/** What the browser needs to derive the same key the server holds. */
struct AdminChallenge {
    std::string salt;
    int iterations = 0;
    std::string nonce;
};

struct AdminConfig {
    bool enabled = false;
    std::string password_hash;
    // How long a login lasts. Short enough that a forgotten open tab in a club
    // shack is not a standing invitation.
    int session_hours = 8;
    std::string config_path;
    // home_network = yes under [admin]: plain HTTP from the home network as
    // well as from this machine; see admin_transport_refusal.
    bool home_network = false;
    // plain_http_anywhere = yes under [admin]: plain HTTP from the internet
    // too, at the operator's own risk. The password still never crosses the
    // network, but someone on the path can read the panel, change the page
    // on its way and so catch the password as it is typed. Only in the file
    // on the machine, like the rest of [admin].
    bool plain_http_anywhere = false;
};

/**
 * Where a sign-in comes from, as far as the limits are concerned. A direct
 * connection from this machine (see direct_local_connection) stays open when
 * failures from everywhere have closed sign-in to strangers, so an operator
 * with a shell can always get in, through ssh -L if need be.
 */
enum class AdminCaller { Remote, ThisMachine };

class AdminAuth {
public:
    /** `clock` gives steady milliseconds; tests pass their own to move time on. */
    explicit AdminAuth(AdminConfig config, std::function<int64_t()> clock = {})
        : config_(std::move(config)), clock_(std::move(clock)) {
        config_.session_hours = std::clamp(config_.session_hours, 1, 168);
    }

    bool enabled() const { return config_.enabled && !config_.password_hash.empty(); }

    /** Public context for request signatures, distinct from the bearer cookie. */
    static std::string signing_context(const std::string& token);
    /**
     * The key a session signs its changes with: HMAC of the password's
     * derived key over the session's signing context. The browser keeps this
     * and not the derived key, which signs in by itself and never expires; a
     * session key is worth nothing once its session is over, or anywhere else.
     */
    static void session_key(const uint8_t derived[32], const std::string& context, uint8_t out[32]);

    /**
     * Issues a challenge so the browser can prove it knows the password
     * without sending it.
     *
     * Returns false when the caller is locked out. The salt and iteration
     * count are the ones the stored hash was made with, because the browser
     * has to derive the very same key; there is a single admin, so there is
     * no account to enumerate by asking.
     *
     * Nothing is stored. The nonce carries when it expires, a random part,
     * and a tag over both and the caller's address under a key drawn when
     * the receiver started, so asking for challenges cannot fill a table and
     * crowd out an administrator's. It is 32 hex digits, as it always was.
     */
    bool issue_challenge(const std::string& address, AdminChallenge& out, int& retry_after,
                         AdminCaller caller = AdminCaller::Remote);

    /**
     * Checks a proof against a challenge and issues a session.
     *
     * `proof` is HMAC-SHA256 of the nonce under the derived key. The nonce is
     * spent whether or not the proof is right, so a captured exchange cannot
     * be replayed and a wrong guess cannot be retried against the same
     * challenge.
     */
    std::string login_with_proof(const std::string& nonce, const std::string& proof,
                                 const std::string& address, int& retry_after,
                                 AdminCaller caller = AdminCaller::Remote);

    /**
     * Checks a proof of the current password against a challenge, as
     * login_with_proof does, without issuing a session: for a change that
     * asks for the password again. A wrong proof counts as a failed sign-in.
     */
    bool proof_valid(const std::string& nonce, const std::string& proof, const std::string& address,
                     int& retry_after, AdminCaller caller = AdminCaller::Remote);

    /**
     * Takes a new stored hash, made in the browser from the new password,
     * and ends every session: they were signed in with the old one. False,
     * changing nothing, when `hash` is not a hash of at least 100,000
     * rounds with a salt of 16 bytes.
     */
    bool replace_password_hash(const std::string& hash);
    static bool acceptable_password_hash(const std::string& hash);

    /** True when the request carries a live session cookie. */
    bool authorised(const HttpRequest& request) const;

    /**
     * True when a request that changes something is properly signed.
     *
     * A signature covers the session context, counter, method, full target
     * and body. A request from an old session cannot be replayed in a new
     * one, even though the operator still uses the same password. Remote
     * administration requires HTTPS; signatures do not replace TLS.
     */
    bool signature_valid(const HttpRequest& request, const std::string& body);

    void logout(const HttpRequest& request);

    /** Drops expired sessions and stale rate-limit records. */
    void expire(int64_t now_ms);

    const AdminConfig& config() const { return config_; }

private:
    struct Attempts {
        int count = 0;  // failures in the current window
        int64_t window_started_ms = 0;
        int64_t locked_until_ms = 0;
        int lockouts = 0;  // served so far; sets the length of the next one
        int64_t last_failure_ms = 0;
    };

    std::string token_from(const HttpRequest& request) const;
    /** The tag that makes a challenge ours: over its text and the address it was given to. */
    std::string challenge_tag(const std::string& body, const std::string& address) const;
    /** True for a challenge this receiver gave `address`; fills in when it expires. */
    bool challenge_genuine(const std::string& nonce, const std::string& address, int64_t& expires_ms) const;
    /** Lockout checks, failures and sessions, all by network rather than address. */
    bool locked_out(const std::string& network, AdminCaller caller, int64_t now, int& retry_after);
    void note_failure(const std::string& network, int64_t now, int& retry_after);
    std::string issue_session(const std::string& network, const std::string& address, int64_t now);

    int64_t now() const;

    AdminConfig config_;
    std::function<int64_t()> clock_;
    std::map<std::string, int64_t> sessions_;   // token -> expiry
    std::map<std::string, Attempts> attempts_;  // network -> recent failures
    std::string challenge_key_;                 // hex, drawn at the first challenge
    int64_t key_drawn_ms_ = 0;                  // when, on the clock above
    std::map<std::string, int64_t> spent_;      // answered challenge -> when it would have expired
    std::deque<int64_t> recent_failures_;       // when proofs failed, anywhere, oldest first
    std::map<std::string, int64_t> known_networks_;  // network -> last sign-in
    std::map<std::string, uint64_t> counters_;  // token -> highest counter seen
    int64_t last_expiry_ms_ = 0;
};

/**
 * Whether the admin API may be used over this request's transport: "" when it
 * may, otherwise a sentence for the person trying that says what would work.
 * HTTPS (directly or from a trusted proxy) may, plain HTTP on a direct local
 * connection may, and with `home_network` so may plain HTTP from a browser in
 * the home network. With `anywhere` (plain_http_anywhere), plain HTTP from
 * anywhere may. `peer` is the socket's own peer, not the address a proxy
 * reported.
 */
std::string admin_transport_refusal(const HttpRequest& request, const std::string& peer, bool home_network,
                                    bool anywhere = false);

inline bool admin_transport_allowed(const HttpRequest& request, const std::string& peer, bool home_network,
                                    bool anywhere = false) {
    return admin_transport_refusal(request, peer, home_network, anywhere).empty();
}

/**
 * True when the socket's peer is loopback and nothing says the request was
 * forwarded: a browser on this machine, or one at the end of ssh -L. A reverse
 * proxy on this machine is not, because it names the client it forwards for.
 */
bool direct_local_connection(const HttpRequest& request, const std::string& peer);

/** Reads the cookie header and returns one cookie's value, or "". */
std::string cookie_value(const std::string& cookie_header, const std::string& name);

// What the configuration editor shows in place of the admin credentials. The
// hash signs an administrator in by itself, so it never goes to a browser:
// a stolen session cookie would otherwise read it once and keep admin rights
// after the session ends.
extern const char* const kHiddenCredential;

/** The configuration text with the values of [admin] password_hash and password hidden. */
std::string hide_admin_credentials(const std::string& text);

/**
 * Text back from the editor, with every value still hidden replaced by the
 * value `current` (the [admin] section of the file on disk) holds for that key.
 * Everything else is returned byte for byte.
 */
std::string restore_admin_credentials(const std::string& text, const ConfigSection& current);

/**
 * `text` with the [admin] password_hash line holding `hash` instead: a new
 * password from the panel, written where the old one was, the rest of the
 * file as it was.
 */
std::string with_admin_password_hash(const std::string& text, const std::string& hash);

/**
 * The same for a file that may have no password yet: the line replaced where
 * there is one, put first in [admin] where the section has none, and an
 * [admin] section added at the end where there is none.
 */
std::string with_admin_password(const std::string& text, const std::string& hash);

/**
 * Why the configuration editor may not replace `current` (the file on disk)
 * with `candidate`, or "" when it may.
 *
 * A session is the most a stolen cookie or an injected script gets, and the
 * panel must not be able to turn it into more. So what decides who may
 * administer ([admin]), which programs may run ([modules]), what is served
 * and whom to believe ([server]), every setting that names a file, and where
 * a band takes samples from the network, are changed only in the file on the
 * machine. [server] takes effect at a restart anyway, which the panel cannot
 * do. A file setting may be dropped, which brings back a default the program
 * chose, and a network input may be closed, but neither added or changed.
 */
std::string machine_only_change(const Config& current, const Config& candidate);

}  // namespace fernsdr
