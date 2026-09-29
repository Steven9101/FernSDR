#include "password.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace fernsdr {

std::string random_hex(size_t bytes) {
    std::vector<uint8_t> buffer(bytes);
    // getrandom(2) would be one call, but it is Linux-only and this is the one
    // place a portable fallback is worth the four lines.
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t filled = 0;
        while (filled < buffer.size()) {
            const ssize_t got = ::read(fd, buffer.data() + filled, buffer.size() - filled);
            if (got <= 0) break;
            filled += static_cast<size_t>(got);
        }
        ::close(fd);
        if (filled == buffer.size()) return to_hex(buffer.data(), buffer.size());
    }
    // No /dev/urandom means no way to generate a token an attacker cannot
    // guess. Returning a weak one would be worse than failing.
    return "";
}

std::string hash_password(const std::string& password, int iterations) {
    const std::string salt = random_hex(16);
    if (salt.empty()) return "";
    uint8_t derived[32];
    pbkdf2_sha256(password, salt, iterations, derived, sizeof(derived));
    return "pbkdf2$" + std::to_string(iterations) + "$" + salt + "$" + to_hex(derived, sizeof(derived));
}

bool parse_password_hash(const std::string& stored, int& iterations, std::string& salt,
                         std::string& derived_hex) {
    // "pbkdf2$<iterations>$<salt>$<hash>"
    if (stored.rfind("pbkdf2$", 0) != 0) return false;
    const size_t first = stored.find('$', 7);
    if (first == std::string::npos) return false;
    const size_t second = stored.find('$', first + 1);
    if (second == std::string::npos) return false;

    iterations = std::atoi(stored.substr(7, first - 7).c_str());
    // A hash claiming a tiny iteration count is either corrupt or an attempt to
    // make verification cheap enough to brute force; either way, refuse it.
    if (iterations < 1000 || iterations > 10000000) return false;

    salt = stored.substr(first + 1, second - first - 1);
    derived_hex = stored.substr(second + 1);
    return !salt.empty() && derived_hex.size() == 64;
}

bool verify_password(const std::string& password, const std::string& stored) {
    int iterations = 0;
    std::string salt, expected;
    if (!parse_password_hash(stored, iterations, salt, expected)) return false;

    uint8_t derived[32];
    pbkdf2_sha256(password, salt, iterations, derived, sizeof(derived));
    return constant_time_equal(to_hex(derived, sizeof(derived)), expected);
}

}  // namespace fernsdr
