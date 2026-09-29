// A release manifest and its signature, as an update check fetches them.
// Verification must refuse whatever a fixed key did not sign; parsing, which
// only ever runs on signed text, must not crash on any text, and what it
// accepts has to be written back as the same manifest.
#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/update/release.h"
#include "../src/util/ed25519.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);
    fernsdr::ReleaseManifest manifest;
    std::string error;

    if (size >= 64) {
        static const fernsdr::ReleaseKey key = [] {
            fernsdr::ReleaseKey k;
            const uint8_t seed[32] = {7};
            fernsdr::ed25519_public_key(seed, k.data());
            return k;
        }();
        if (fernsdr::verify_release_manifest(input.substr(64), input.substr(0, 64), {key}, manifest, error)) {
            __builtin_trap();
        }
    }

    if (!fernsdr::parse_release_manifest(input, manifest, error)) return 0;
    std::string text;
    fernsdr::ReleaseManifest back;
    if (!fernsdr::format_release_manifest(manifest, text, error) ||
        !fernsdr::parse_release_manifest(text, back, error) || back.version != manifest.version ||
        back.date != manifest.date || back.notes != manifest.notes || back.assets.size() != manifest.assets.size()) {
        __builtin_trap();
    }
    return 0;
}
