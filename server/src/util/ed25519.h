// Ed25519 signatures (RFC 8032), for release manifests: every receiver checks
// them, and only the release tools make them.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fernsdr {

// The public key belonging to a 32-byte secret seed.
void ed25519_public_key(const uint8_t seed[32], uint8_t public_key[32]);

// Signs `message` with the key made from `seed`.
void ed25519_sign(const uint8_t seed[32], const uint8_t* message, size_t length, uint8_t signature[64]);

// True when `signature` is `public_key`'s over `message`. Strict about the
// forms a signature can take without changing what it proves: an S not under
// the group order, or a key or R not in canonical form, is refused, so a
// valid signature cannot be turned into another without the key. So is a key
// of small order, under which anyone can make signatures.
bool ed25519_verify(const uint8_t public_key[32], const uint8_t* message, size_t length,
                    const uint8_t signature[64]);

// True when ed25519_verify can accept anything under `public_key`: it is a
// point on the curve, written in canonical form, and not of small order.
bool ed25519_key_is_valid(const uint8_t public_key[32]);

}  // namespace fernsdr
