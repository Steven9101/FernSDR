// Test vectors, not self-consistency checks.
//
// A hash function that is wrong but consistent with itself passes any test
// that only compares it to itself, and the failure surfaces years later when
// something else has to interoperate. These are the published vectors:
// SHA-256 from FIPS 180-4, HMAC-SHA256 from RFC 4231, PBKDF2-HMAC-SHA256 from
// RFC 7914's worked examples.
#include "../src/util/password.h"
#include "test_util.h"

#include <string>

using fernsdr::Sha256;

namespace {

std::string sha256_hex(const std::string& input) {
    uint8_t digest[32];
    Sha256 hash;
    hash.update(input);
    hash.finish(digest);
    return fernsdr::to_hex(digest, sizeof(digest));
}

std::string hmac_hex(const std::string& key, const std::string& message) {
    uint8_t out[32];
    fernsdr::hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                         reinterpret_cast<const uint8_t*>(message.data()), message.size(), out);
    return fernsdr::to_hex(out, sizeof(out));
}

}  // namespace

TEST_CASE(sha256_matches_the_published_vectors) {
    CHECK_EQ_STR(sha256_hex(""),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ_STR(sha256_hex("abc"),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ_STR(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million 'a's: exercises the multi-block path and the length field.
    CHECK_EQ_STR(sha256_hex(std::string(1000000, 'a')),
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // The padding boundaries, cross-checked against Python's hashlib: 55 is
    // the last length whose padding fits in the same block, 56 is the first
    // that needs another, and 64 is a whole block with nothing left over.
    CHECK_EQ_STR(sha256_hex(std::string(55, 'x')),
                 "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072");
    CHECK_EQ_STR(sha256_hex(std::string(56, 'x')),
                 "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e");
    CHECK_EQ_STR(sha256_hex(std::string(64, 'x')),
                 "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c");
}

TEST_CASE(hmac_sha256_matches_rfc_4231) {
    CHECK_EQ_STR(hmac_hex(std::string(20, '\x0b'), "Hi There"),
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK_EQ_STR(hmac_hex("Jefe", "what do ya want for nothing?"),
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // A key longer than the block size, which has to be hashed first.
    CHECK_EQ_STR(hmac_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
                 "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE(pbkdf2_matches_rfc_7914) {
    uint8_t out[64];
    fernsdr::pbkdf2_sha256("passwd", "salt", 1, out, 64);
    CHECK_EQ_STR(fernsdr::to_hex(out, 16), "55ac046e56e3089fec1691c22544b605");

    fernsdr::pbkdf2_sha256("Password", "NaCl", 80000, out, 64);
    CHECK_EQ_STR(fernsdr::to_hex(out, 16), "4ddcd8f60b98be21830cee5ef22701f9");
}

TEST_CASE(password_round_trip) {
    // A low count here only because the test should not take a second; the
    // default is 600000.
    const std::string stored = fernsdr::hash_password("correct horse battery staple", 5000);
    CHECK(stored.rfind("pbkdf2$5000$", 0) == 0);
    CHECK(fernsdr::verify_password("correct horse battery staple", stored));
    CHECK(!fernsdr::verify_password("Correct horse battery staple", stored));
    CHECK(!fernsdr::verify_password("", stored));
    CHECK(!fernsdr::verify_password("correct horse battery stapl", stored));

    // Two hashes of the same password differ: the salt is doing its job, so a
    // stolen config cannot be attacked with a rainbow table.
    CHECK(fernsdr::hash_password("same", 5000) != fernsdr::hash_password("same", 5000));
}

TEST_CASE(a_malformed_stored_hash_is_refused) {
    for (const char* stored : {
             "", "pbkdf2", "pbkdf2$", "pbkdf2$5000", "pbkdf2$5000$salt",
             "pbkdf2$5000$salt$short",
             // An iteration count low enough to brute force, which is what an
             // attacker who could edit the config would set it to.
             "pbkdf2$1$abcd$0000000000000000000000000000000000000000000000000000000000000000",
             "plain$hunter2",
         }) {
        CHECK(!fernsdr::verify_password("anything", stored));
    }
}

TEST_CASE(constant_time_equal_still_compares_correctly) {
    CHECK(fernsdr::constant_time_equal("abc", "abc"));
    CHECK(!fernsdr::constant_time_equal("abc", "abd"));
    CHECK(!fernsdr::constant_time_equal("abc", "ab"));
    CHECK(fernsdr::constant_time_equal("", ""));
}

TEST_CASE(random_hex_is_the_right_shape_and_not_repeated) {
    const std::string a = fernsdr::random_hex(32);
    const std::string b = fernsdr::random_hex(32);
    CHECK_EQ(static_cast<long long>(a.size()), 64);
    CHECK(a != b);
    for (char c : a) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}
