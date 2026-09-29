// Ed25519 against OpenSSL's libcrypto: random seeds and messages signed by
// both, each signature checked by both, and each changed in one bit for both
// to refuse. Ed25519 signing is deterministic, so keys and signatures have to
// agree byte for byte. The first round is RFC 8032's TEST 1, whose empty
// message the openssl command line cannot sign.
//
//   make -C server ed25519-compare               2000 rounds
//   server/build/ed25519-compare 100000 7        100000 rounds, random seed 7
//
// libcrypto is linked into this program only, never into the receiver.
#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "../src/util/ed25519.h"

namespace {

bool openssl_sign(const uint8_t seed[32], const std::vector<uint8_t>& message, uint8_t key[32],
                  uint8_t signature[64]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t key_length = 32, signature_length = 64;
    const bool ok = pkey && ctx && EVP_PKEY_get_raw_public_key(pkey, key, &key_length) == 1 && key_length == 32 &&
                    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
                    EVP_DigestSign(ctx, signature, &signature_length, message.data(), message.size()) == 1 &&
                    signature_length == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool openssl_verifies(const uint8_t key[32], const std::vector<uint8_t>& message, const uint8_t signature[64]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key, 32);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = pkey && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
                    EVP_DigestVerify(ctx, signature, 64, message.data(), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ERR_clear_error();  // a refusal leaves its reason queued
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const long rounds = argc > 1 ? std::atol(argv[1]) : 2000;
    const unsigned long run = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1;
    std::mt19937_64 random(run);
    long failures = 0;
    auto fail = [&](long round, const char* what) {
        if (failures++ < 20) std::printf("round %ld: %s\n", round, what);
    };
    for (long round = 0; round < rounds; round++) {
        uint8_t seed[32];
        std::vector<uint8_t> message;
        if (round == 0) {
            static const uint8_t test1[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                              0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                              0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
            std::memcpy(seed, test1, 32);
        } else {
            for (uint8_t& b : seed) b = static_cast<uint8_t>(random());
            // Mostly a few hash blocks; now and then a few dozen.
            message.resize(round % 16 == 0 ? random() % 4096 : random() % 300);
            for (uint8_t& b : message) b = static_cast<uint8_t>(random());
        }

        uint8_t key[32], signature[64], their_key[32], their_signature[64];
        fernsdr::ed25519_public_key(seed, key);
        fernsdr::ed25519_sign(seed, message.data(), message.size(), signature);
        if (!openssl_sign(seed, message, their_key, their_signature)) {
            fail(round, "OpenSSL could not sign");
            continue;
        }
        if (std::memcmp(key, their_key, 32) != 0) fail(round, "the keys differ");
        if (std::memcmp(signature, their_signature, 64) != 0) fail(round, "the signatures differ");
        if (!fernsdr::ed25519_verify(their_key, message.data(), message.size(), their_signature))
            fail(round, "OpenSSL's signature is refused here");
        if (!openssl_verifies(key, message, signature)) fail(round, "this signature is refused by OpenSSL");

        const size_t bits = (32 + 64 + message.size()) * 8;
        const size_t bit = static_cast<size_t>(random() % bits);
        uint8_t* target = bit < 256 ? key + bit / 8
                          : bit < 768 ? signature + (bit - 256) / 8
                                      : message.data() + (bit - 768) / 8;
        *target ^= static_cast<uint8_t>(1u << (bit % 8));
        if (fernsdr::ed25519_verify(key, message.data(), message.size(), signature))
            fail(round, "a changed bit verifies here");
        if (openssl_verifies(key, message, signature)) fail(round, "a changed bit verifies in OpenSSL");
    }
    std::printf("%ld rounds from random seed %lu against %s: %ld failures\n", rounds, run,
                OpenSSL_version(OPENSSL_VERSION), failures);
    return failures == 0 ? 0 : 1;
}
