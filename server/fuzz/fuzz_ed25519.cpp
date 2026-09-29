// Ed25519 verification: what an update check runs on the signature of a
// manifest fetched from the network, before anything in it is believed.
//
// The input is read twice. As key, signature and message, verification must
// not crash or trip a sanitizer, whatever it answers. As a seed and a message,
// the signature made from them has to verify, and has to stop verifying when
// one bit of it, of the key or of the message changes; which bit the input
// picks with its first two bytes.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../src/util/ed25519.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 96) return 0;
    (void)fernsdr::ed25519_verify(data, data + 96, size - 96, data + 32);

    uint8_t key[32], signature[64];
    std::vector<uint8_t> message(data + 32, data + size);
    fernsdr::ed25519_public_key(data, key);
    fernsdr::ed25519_sign(data, message.data(), message.size(), signature);
    if (!fernsdr::ed25519_verify(key, message.data(), message.size(), signature)) __builtin_trap();

    const size_t bits = (sizeof(key) + sizeof(signature) + message.size()) * 8;
    const size_t bit = (static_cast<size_t>(data[0]) | static_cast<size_t>(data[1]) << 8) % bits;
    uint8_t* target = bit < 256 ? key + bit / 8
                      : bit < 768 ? signature + (bit - 256) / 8
                                  : message.data() + (bit - 768) / 8;
    *target ^= static_cast<uint8_t>(1u << (bit % 8));
    if (fernsdr::ed25519_verify(key, message.data(), message.size(), signature)) __builtin_trap();
    return 0;
}
