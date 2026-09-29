// A release as the updater and the installer see it: a manifest of plain
// lines naming each platform's archive by size and SHA-256, signed with
// Ed25519 by a key the receiver carries.
//
//   fernsdr-release 1
//   version 0.1.1
//   date 2026-10-01
//   channel stable
//   asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar 5242880 <sha256 in hex>
//   note The release notes, one line each; "note" alone is an empty line.
//
// Lines, not JSON, so that install.sh can read it with awk. The lines come in
// that order, each key once but asset and note, and nothing else may appear:
// what a manifest says is exactly what it holds.
//
// A receiver reads only this format and refuses every other, so the format's
// number is in the file's name too. One that changes it is published under a
// new name beside this one, which releases keep publishing for as long as
// receivers that read only this one are around. The channel is in the signed
// text because a testing release is signed with the same key: where it was
// fetched from says nothing that an attacker in the way could not change.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fernsdr {

// The manifest and its signature, as release assets.
constexpr const char* kReleaseManifestName = "fernsdr-release-v1.txt";
constexpr const char* kReleaseSignatureName = "fernsdr-release-v1.txt.sig";

struct ReleaseAsset {
    std::string platform;  // linux-x86_64, linux-aarch64 or linux-armhf
    std::string file;      // fernsdr-<version>-<platform>.tar
    uint64_t size = 0;
    std::string sha256;  // lower-case hex
};

struct ReleaseManifest {
    std::string version;
    std::string date;
    std::string channel;  // stable or testing
    std::string notes;
    std::vector<ReleaseAsset> assets;

    // The asset for `platform`, or nullptr.
    const ReleaseAsset* asset_for(const std::string& platform) const;
};

using ReleaseKey = std::array<uint8_t, 32>;

// Reads a manifest strictly; `error` says what is wrong.
bool parse_release_manifest(const std::string& text, ReleaseManifest& out, std::string& error);

// Writes the manifest `parse_release_manifest` reads back as `manifest`.
// False, with the reason, when a field would not read back.
bool format_release_manifest(const ReleaseManifest& manifest, std::string& text, std::string& error);

// What a release signature covers: a line naming what is signed, then the
// manifest. A key that ever signs anything else cannot have that signature
// taken for a release.
std::string release_signed_message(const std::string& manifest_text);

// True when `signature` (64 bytes) is by one of `keys` over `manifest_text`,
// and the manifest reads. Nothing in the manifest is looked at before its
// signature holds.
bool verify_release_manifest(const std::string& manifest_text, const std::string& signature,
                             const std::vector<ReleaseKey>& keys, ReleaseManifest& out, std::string& error);

// Orders two versions of the form MAJOR.MINOR.PATCH: -1, 0 or 1 in `order`.
// False when either is not one.
bool compare_versions(const std::string& a, const std::string& b, int& order);

// The platform this program was built for, as release assets name it, or ""
// for one releases are not built for.
const char* release_platform();

}  // namespace fernsdr
