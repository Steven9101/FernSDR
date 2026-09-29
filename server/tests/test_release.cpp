// Release manifests: what reads, what does not, and that a signature counts
// only when it is by a trusted key over the manifest as a release.
#include "../src/update/release.h"
#include "../src/update/release_keys.h"
#include "../src/util/ed25519.h"
#include "test_util.h"

#include <set>
#include <string>
#include <vector>

using fernsdr::ReleaseAsset;
using fernsdr::ReleaseKey;
using fernsdr::ReleaseManifest;

namespace {

const std::string kHash = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

ReleaseManifest sample() {
    ReleaseManifest manifest;
    manifest.version = "0.1.1";
    manifest.date = "2026-10-01";
    manifest.channel = "stable";
    manifest.notes = "Fixes a stall on slow links.\n\n- Sch\xc3\xa4rfer: 50 \xc2\xb0 notes keep UTF-8\n  indented";
    for (const char* platform : {"linux-x86_64", "linux-aarch64", "linux-armhf"}) {
        manifest.assets.push_back({platform, std::string("fernsdr-0.1.1-") + platform + ".tar", 5242880, kHash});
    }
    return manifest;
}

std::string text_of(const ReleaseManifest& manifest) {
    std::string text, error;
    CHECK(fernsdr::format_release_manifest(manifest, text, error));
    return text;
}

// A key from a fixed seed, and its signature over `text` as a release.
struct TestKey {
    uint8_t seed[32];
    ReleaseKey key;
    explicit TestKey(uint8_t fill) {
        for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(fill + i);
        fernsdr::ed25519_public_key(seed, key.data());
    }
    std::string sign_bytes(const std::string& bytes) const {
        uint8_t signature[64];
        fernsdr::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), signature);
        return std::string(reinterpret_cast<const char*>(signature), 64);
    }
    std::string sign(const std::string& text) const { return sign_bytes(fernsdr::release_signed_message(text)); }
};

bool parses(const std::string& text, const std::string& expected_reason = "") {
    ReleaseManifest manifest;
    std::string error;
    const bool ok = fernsdr::parse_release_manifest(text, manifest, error);
    if (!ok && !expected_reason.empty() && error.find(expected_reason) == std::string::npos) {
        fprintf(stderr, "    refused for \"%s\", expected \"%s\"\n", error.c_str(), expected_reason.c_str());
        return true;  // the wrong refusal counts as a failure of the !parses() check
    }
    return ok;
}

// The sample's text with one line replaced, inserted or removed.
std::string with_line(size_t index, const std::string& line, bool insert = false, bool remove = false) {
    std::vector<std::string> lines;
    const std::string text = text_of(sample());
    for (size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    if (remove) {
        lines.erase(lines.begin() + static_cast<long>(index));
    } else if (insert) {
        lines.insert(lines.begin() + static_cast<long>(index), line);
    } else {
        lines[index] = line;
    }
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    return out;
}

}  // namespace

TEST_CASE(release_manifest_reads_back_what_is_written) {
    const std::string text = text_of(sample());
    CHECK_EQ_STR(text.substr(0, 63), "fernsdr-release 1\nversion 0.1.1\ndate 2026-10-01\nchannel stable\n");
    CHECK(text.find("asset linux-armhf fernsdr-0.1.1-linux-armhf.tar 5242880 " + kHash + "\n") != std::string::npos);
    CHECK(text.find("\nnote\nnote - Sch") != std::string::npos);
    ReleaseManifest back;
    std::string error;
    CHECK(fernsdr::parse_release_manifest(text, back, error));
    CHECK_EQ_STR(back.version, "0.1.1");
    CHECK_EQ_STR(back.date, "2026-10-01");
    CHECK_EQ_STR(back.channel, "stable");
    CHECK_EQ_STR(back.notes, sample().notes);
    CHECK_EQ(back.assets.size(), 3u);
    CHECK(back.asset_for("linux-aarch64") != nullptr);
    CHECK(back.asset_for("linux-mips") == nullptr);
    // Without notes, and with one asset.
    ReleaseManifest small = sample();
    small.notes.clear();
    small.assets.resize(1);
    CHECK(fernsdr::parse_release_manifest(text_of(small), back, error));
    CHECK(back.notes.empty());
}

TEST_CASE(release_manifest_refuses_what_it_does_not_expect) {
    CHECK(parses(text_of(sample())));
    // The header, version and date lines, each in its place and form.
    CHECK(!parses(with_line(0, "fernsdr-release 2"), "start with"));
    for (const char* version : {"version 0.1", "version 0.1.1.1", "version 01.1.1", "version 0.1.x", "version 0.1.1 ",
                                "version  0.1.1", "version", "release 0.1.1"}) {
        CHECK(!parses(with_line(1, version), "version"));
    }
    for (const char* date :
         {"date 2026-13-01", "date 2026-10-32", "date 26-10-01", "date 2026/10/01", "day 2026-10-01"}) {
        CHECK(!parses(with_line(2, date), "date"));
    }
    // The channel: one of two, spelled exactly, and not left out.
    for (const char* channel : {"channel beta", "channel Stable", "channel", "channel stable ", "chanel stable"}) {
        CHECK(!parses(with_line(3, channel), "channel"));
    }
    CHECK(!parses(with_line(3, "", false, true), "channel"));
    CHECK(parses(with_line(3, "channel testing")));
    // Assets: a platform nobody builds, the wrong file, sizes and hashes that
    // are not what they claim, one platform twice.
    const std::string a = "asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar 5242880 ";
    for (const std::string& asset :
         {std::string("asset linux-mips fernsdr-0.1.1-linux-mips.tar 1 ") + kHash,
          "asset linux-x86_64 fernsdr-0.1.0-linux-x86_64.tar 5242880 " + kHash,
          "asset linux-x86_64 ../fernsdr-0.1.1-linux-x86_64.tar 5242880 " + kHash,
          "asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar 0 " + kHash,
          "asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar 05242880 " + kHash,
          "asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar 2000000000 " + kHash,
          "asset linux-x86_64 fernsdr-0.1.1-linux-x86_64.tar -1 " + kHash,
          a + kHash.substr(1), a + "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08",
          a + kHash + " extra", "asset linux-aarch64 fernsdr-0.1.1-linux-aarch64.tar 5242880 " + kHash}) {
        CHECK(!parses(with_line(4, asset)));
    }
    // Order: an asset after the notes, a note before any asset, an unknown key.
    CHECK(!parses(text_of(sample()) + "asset linux-x86_64 x 1 " + kHash + "\n", "out of place"));
    CHECK(!parses(with_line(4, "note too early", true), "out of place"));
    CHECK(!parses(with_line(5, "signature abc", true), "unknown"));
    CHECK(!parses(with_line(4, "", false, false), "out of place"));
    CHECK(!parses("fernsdr-release 1\nversion 0.1.1\ndate 2026-10-01\nchannel stable\n", "no archive"));
    // Bytes: a carriage return or tab, no final line break, bad UTF-8, size.
    CHECK(!parses(with_line(1, "version 0.1.1\r"), "control character"));
    CHECK(!parses(with_line(8, "note a\ttab"), "control character"));
    const std::string text = text_of(sample());
    CHECK(!parses(text.substr(0, text.size() - 1), "line break"));
    CHECK(!parses(text + "note \xc3\x28\n", "UTF-8"));
    CHECK(!parses(text + std::string(64 * 1024, 'n') + "\n", "larger"));
    std::string notes;
    for (int i = 0; i < 400; i++) notes += "note " + std::string(90, 'n') + "\n";
    CHECK(!parses(text + notes, "notes are longer"));
}

TEST_CASE(release_signature_needs_a_trusted_key_over_the_release) {
    const TestKey ours(1), other(101);
    const std::string text = text_of(sample());
    const std::string signature = ours.sign(text);
    ReleaseManifest manifest;
    std::string error;
    CHECK(fernsdr::verify_release_manifest(text, signature, {ours.key}, manifest, error));
    CHECK_EQ_STR(manifest.version, "0.1.1");
    // Either of two keys will do, as the offline key does when CI's is lost.
    CHECK(fernsdr::verify_release_manifest(text, signature, {other.key, ours.key}, manifest, error));
    // Nobody else's key, a changed manifest, another signature's bytes.
    CHECK(!fernsdr::verify_release_manifest(text, signature, {other.key}, manifest, error));
    CHECK(error.find("not signed by a key") != std::string::npos);
    CHECK(manifest.version.empty());
    std::string changed = text;
    changed[changed.find("5242880")] = '6';
    CHECK(!fernsdr::verify_release_manifest(changed, signature, {ours.key}, manifest, error));
    CHECK(!fernsdr::verify_release_manifest(text, other.sign(text), {ours.key}, manifest, error));
    // The same key over the bare manifest, as if it had signed the text for
    // some other purpose: not a release signature.
    CHECK(!fernsdr::verify_release_manifest(text, ours.sign_bytes(text), {ours.key}, manifest, error));
    // Signatures of the wrong size, and no keys at all.
    CHECK(!fernsdr::verify_release_manifest(text, signature.substr(0, 63), {ours.key}, manifest, error));
    CHECK(!fernsdr::verify_release_manifest(text, signature + "x", {ours.key}, manifest, error));
    CHECK(!fernsdr::verify_release_manifest(text, signature, {}, manifest, error));
    CHECK(error.find("no release key") != std::string::npos);
    // A signed manifest that does not read is still refused, after the check.
    const std::string garbage = "fernsdr-release 1\nversion one\n";
    CHECK(!fernsdr::verify_release_manifest(garbage, ours.sign(garbage), {ours.key}, manifest, error));
    CHECK(error.find("version") != std::string::npos);
}

TEST_CASE(release_versions_order_by_number) {
    const auto order = [](const std::string& a, const std::string& b) {
        int result = 99;
        CHECK(fernsdr::compare_versions(a, b, result));
        return result;
    };
    CHECK_EQ(order("0.1.0", "0.1.1"), -1);
    CHECK_EQ(order("0.1.1", "0.2.0"), -1);
    CHECK_EQ(order("0.9.0", "0.10.0"), -1);
    CHECK_EQ(order("1.0.0", "0.99.99"), 1);
    CHECK_EQ(order("2.3.4", "2.3.4"), 0);
    int unused = 0;
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "1.2.03", "a.b.c", "1.2.3-rc1", " 1.2.3", "1234567890.0.0"}) {
        CHECK(!fernsdr::compare_versions(bad, "1.2.3", unused));
    }
}

TEST_CASE(release_platform_and_keys_are_usable) {
#if defined(__linux__) && defined(__x86_64__)
    CHECK_EQ_STR(fernsdr::release_platform(), "linux-x86_64");
#endif
    // Every embedded key is a point that can verify, and none twice.
    std::set<ReleaseKey> seen;
    for (const ReleaseKey& key : fernsdr::release_keys()) {
        CHECK(fernsdr::ed25519_key_is_valid(key.data()));
        CHECK(seen.insert(key).second);
    }
}
