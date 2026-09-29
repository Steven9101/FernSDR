#include "release.h"

#include "../util/ed25519.h"
#include "../util/utf8.h"

namespace fernsdr {

namespace {

constexpr size_t kMaxManifest = 64 * 1024;
constexpr size_t kMaxNotes = 32 * 1024;
// Release archives are megabytes; a gigabyte is a mistake or a lie.
constexpr uint64_t kMaxAssetSize = uint64_t{1} << 30;
const char* const kPlatforms[] = {"linux-x86_64", "linux-aarch64", "linux-armhf"};

bool known_platform(const std::string& name) {
    for (const char* platform : kPlatforms) {
        if (name == platform) return true;
    }
    return false;
}

bool digits(const std::string& text, size_t from, size_t count) {
    if (text.size() < from + count) return false;
    for (size_t i = from; i < from + count; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

bool valid_date(const std::string& date) {
    if (date.size() != 10 || !digits(date, 0, 4) || date[4] != '-' || !digits(date, 5, 2) || date[7] != '-' ||
        !digits(date, 8, 2)) {
        return false;
    }
    const int month = std::stoi(date.substr(5, 2)), day = std::stoi(date.substr(8, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool parse_size(const std::string& text, uint64_t& value) {
    if (text.empty() || text.size() > 12 || (text[0] == '0') || !digits(text, 0, text.size())) return false;
    value = std::stoull(text);
    return value <= kMaxAssetSize;
}

bool lower_hex(const std::string& text, size_t length) {
    if (text.size() != length) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool version_parts(const std::string& version, uint64_t parts[3]) {
    size_t at = 0;
    for (int i = 0; i < 3; i++) {
        size_t end = at;
        while (end < version.size() && version[end] >= '0' && version[end] <= '9') end++;
        const size_t length = end - at;
        if (length == 0 || length > 9 || (length > 1 && version[at] == '0')) return false;
        parts[i] = std::stoull(version.substr(at, length));
        if (i < 2) {
            if (end >= version.size() || version[end] != '.') return false;
            at = end + 1;
        } else if (end != version.size()) {
            return false;
        }
    }
    return true;
}

// Splits "key rest" at the first space; a line with no space is all key.
void key_and_rest(const std::string& line, std::string& key, std::string& rest) {
    const size_t space = line.find(' ');
    key = line.substr(0, space);
    rest = space == std::string::npos ? "" : line.substr(space + 1);
}

std::vector<std::string> fields(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        const size_t space = text.find(' ', at);
        out.push_back(text.substr(at, space - at));
        if (space == std::string::npos) return out;
        at = space + 1;
    }
}

}  // namespace

const ReleaseAsset* ReleaseManifest::asset_for(const std::string& platform) const {
    for (const ReleaseAsset& asset : assets) {
        if (asset.platform == platform) return &asset;
    }
    return nullptr;
}

bool parse_release_manifest(const std::string& text, ReleaseManifest& out, std::string& error) {
    out = ReleaseManifest{};
    if (text.size() > kMaxManifest) {
        error = "the manifest is larger than one can be";
        return false;
    }
    if (text.empty() || text.back() != '\n') {
        error = "the manifest does not end with a line break";
        return false;
    }
    if (!valid_utf8(text)) {
        error = "the manifest is not UTF-8";
        return false;
    }

    // Where each line may go: header, version, date, channel, then assets,
    // then notes.
    enum Stage { Header, Version, Date, Channel, Assets, Notes } stage = Header;
    size_t number = 0;
    for (size_t at = 0; at < text.size(); number++) {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const std::string where = "line " + std::to_string(number + 1) + " of the manifest";
        for (const char c : line) {
            if ((static_cast<unsigned char>(c) < 0x20) || c == 0x7f) {
                error = where + " has a control character";
                return false;
            }
        }
        std::string key, rest;
        key_and_rest(line, key, rest);

        if (stage == Header) {
            if (line != "fernsdr-release 1") {
                error = "the manifest does not start with fernsdr-release 1";
                return false;
            }
            stage = Version;
        } else if (stage == Version) {
            uint64_t parts[3];
            if (key != "version" || !version_parts(rest, parts)) {
                error = where + " should be the version, as MAJOR.MINOR.PATCH";
                return false;
            }
            out.version = rest;
            stage = Date;
        } else if (stage == Date) {
            if (key != "date" || !valid_date(rest)) {
                error = where + " should be the date, as YYYY-MM-DD";
                return false;
            }
            out.date = rest;
            stage = Channel;
        } else if (stage == Channel) {
            if (key != "channel" || (rest != "stable" && rest != "testing")) {
                error = where + " should be the channel, stable or testing";
                return false;
            }
            out.channel = rest;
            stage = Assets;
        } else if (key == "asset" && stage == Assets) {
            const std::vector<std::string> parts = fields(rest);
            ReleaseAsset asset;
            if (parts.size() != 4 || !known_platform(parts[0]) || !parse_size(parts[2], asset.size) ||
                !lower_hex(parts[3], 64)) {
                error = where + " is not an asset: platform, file, size and SHA-256";
                return false;
            }
            asset.platform = parts[0];
            asset.file = parts[1];
            asset.sha256 = parts[3];
            if (asset.file != "fernsdr-" + out.version + "-" + asset.platform + ".tar") {
                error = where + " names a file other than this version's archive for " + asset.platform;
                return false;
            }
            if (out.asset_for(asset.platform)) {
                error = where + " repeats the asset for " + asset.platform;
                return false;
            }
            out.assets.push_back(asset);
        } else if (key == "note" && (stage == Assets || stage == Notes) && !out.assets.empty()) {
            if (stage == Notes) out.notes += '\n';
            out.notes += rest;
            if (out.notes.size() > kMaxNotes) {
                error = "the release notes are longer than they can be";
                return false;
            }
            stage = Notes;
        } else {
            error = where + " is out of place or unknown: " + printable(key, 40);
            return false;
        }
    }
    if (out.assets.empty()) {
        error = "the manifest names no archive";
        return false;
    }
    return true;
}

bool format_release_manifest(const ReleaseManifest& manifest, std::string& text, std::string& error) {
    text = "fernsdr-release 1\nversion " + manifest.version + "\ndate " + manifest.date + "\nchannel " +
           manifest.channel + "\n";
    for (const ReleaseAsset& asset : manifest.assets) {
        text += "asset " + asset.platform + " " + asset.file + " " + std::to_string(asset.size) + " " + asset.sha256 +
                "\n";
    }
    if (!manifest.notes.empty()) {
        for (size_t at = 0; at <= manifest.notes.size();) {
            size_t end = manifest.notes.find('\n', at);
            if (end == std::string::npos) end = manifest.notes.size();
            const std::string line = manifest.notes.substr(at, end - at);
            text += line.empty() ? "note\n" : "note " + line + "\n";
            at = end + 1;
        }
    }
    ReleaseManifest check;
    std::string why;
    bool same = parse_release_manifest(text, check, why) && check.version == manifest.version &&
                check.date == manifest.date && check.channel == manifest.channel && check.notes == manifest.notes &&
                check.assets.size() == manifest.assets.size();
    for (size_t i = 0; same && i < check.assets.size(); i++) {
        const ReleaseAsset &a = check.assets[i], &b = manifest.assets[i];
        same = a.platform == b.platform && a.file == b.file && a.size == b.size && a.sha256 == b.sha256;
    }
    if (!same) {
        error = why.empty() ? "the manifest would not read back as given" : why;
        text.clear();
        return false;
    }
    return true;
}

std::string release_signed_message(const std::string& manifest_text) {
    return "fernsdr-release-v1\n" + manifest_text;
}

bool verify_release_manifest(const std::string& manifest_text, const std::string& signature,
                             const std::vector<ReleaseKey>& keys, ReleaseManifest& out, std::string& error) {
    out = ReleaseManifest{};
    if (keys.empty()) {
        error = "this build carries no release key, so it cannot check a release";
        return false;
    }
    if (signature.size() != 64 || manifest_text.size() > kMaxManifest) {
        error = "the release signature is not one";
        return false;
    }
    const std::string message = release_signed_message(manifest_text);
    bool signed_by_one = false;
    for (const ReleaseKey& key : keys) {
        if (ed25519_verify(key.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                           reinterpret_cast<const uint8_t*>(signature.data()))) {
            signed_by_one = true;
            break;
        }
    }
    if (!signed_by_one) {
        error = "the release is not signed by a key this receiver trusts";
        return false;
    }
    return parse_release_manifest(manifest_text, out, error);
}

bool compare_versions(const std::string& a, const std::string& b, int& order) {
    uint64_t x[3], y[3];
    if (!version_parts(a, x) || !version_parts(b, y)) return false;
    order = 0;
    for (int i = 0; i < 3 && order == 0; i++) order = x[i] < y[i] ? -1 : (x[i] > y[i] ? 1 : 0);
    return true;
}

const char* release_platform() {
#if defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-aarch64";
#elif defined(__linux__) && defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7 && defined(__ARM_PCS_VFP)
    return "linux-armhf";
#else
    return "";
#endif
}

}  // namespace fernsdr
