// Release signing: the keys, the manifest and its signature. A separate
// program, never part of the receiver, which only ever checks.
//
//   fernsdr-release keygen SECRET-FILE
//       Makes a signing key, writes its secret to SECRET-FILE (a new file,
//       readable by its owner only) and prints the public half for
//       src/update/release_keys.h.
//   fernsdr-release public-key SECRET
//       Prints the public half of a secret, so CI can check that its secret
//       belongs to a key the receivers carry.
//   fernsdr-release public-keys
//       Prints the keys this build carries, one a line, as OpenSSL reads a
//       public key: the base64 of its SubjectPublicKeyInfo. This is how the
//       published install.sh gets them, so that it checks releases with the
//       keys the receiver it installs checks them with.
//   fernsdr-release manifest OUT VERSION DATE CHANNEL NOTES-FILE ARCHIVE...
//       Writes the manifest for the archives, fernsdr-VERSION-PLATFORM.tar,
//       each of which has to read as a release archive, with NOTES-FILE as
//       the release notes. CHANNEL is stable or testing.
//   fernsdr-release sign [--test-key] SECRET MANIFEST
//       Signs MANIFEST as a release and writes MANIFEST.sig. Only with a key
//       this build carries, unless --test-key says the signature is for a
//       rehearsal with a key made for it.
//   fernsdr-release verify MANIFEST [PUBLIC-KEY...]
//       Checks MANIFEST.sig with the given keys, or with the ones this build
//       carries, then each archive, which has to be beside MANIFEST.
//   fernsdr-release pack OUT DIRECTORY
//       Writes DIRECTORY's contents as a release archive: files and
//       directories in name order, owned by root, modes 0755 and 0644, all
//       with the time SOURCE_DATE_EPOCH gives (0 without it), so that the
//       same tree always gives the same archive. A link or any other special
//       file is refused.
//
// SECRET is a file holding the secret in hex, or env:NAME for an environment
// variable holding it, which is how CI passes it without writing it down.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/update/release.h"
#include "../src/update/release_keys.h"
#include "../src/update/ustar.h"
#include "../src/util/ed25519.h"
#include "../src/util/password.h"
#include "../src/util/sha1.h"

using namespace fernsdr;

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return static_cast<bool>(in) || in.eof();
}

bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return static_cast<bool>(out);
}

bool hex_key(std::string text, uint8_t out[32]) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    std::vector<uint8_t> bytes;
    if (text.size() != 64 || !from_hex(text, bytes)) return false;
    std::memcpy(out, bytes.data(), 32);
    return true;
}

bool load_secret(const std::string& source, uint8_t seed[32]) {
    std::string text;
    if (source.rfind("env:", 0) == 0) {
        const char* value = std::getenv(source.c_str() + 4);
        if (!value) {
            fprintf(stderr, "fernsdr-release: %s is not set\n", source.c_str() + 4);
            return false;
        }
        text = value;
    } else if (!read_file(source, text)) {
        fprintf(stderr, "fernsdr-release: cannot read %s\n", source.c_str());
        return false;
    }
    if (!hex_key(text, seed)) {
        fprintf(stderr, "fernsdr-release: %s does not hold a secret key (64 hex digits)\n", source.c_str());
        return false;
    }
    return true;
}

std::string sha256_hex(const std::string& data) {
    Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, sizeof(digest));
}

int keygen(const std::string& path) {
    const std::string secret = random_hex(32);
    uint8_t seed[32];
    if (secret.empty() || !hex_key(secret, seed)) {
        fprintf(stderr, "fernsdr-release: no randomness to make a key from\n");
        return 1;
    }
    // O_EXCL: a key is made once, and a second run must not replace the
    // secret of a key receivers already trust.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "fernsdr-release: cannot create %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    const std::string line = secret + "\n";
    const bool written = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    if (::fsync(fd) != 0 || ::close(fd) != 0 || !written) {
        fprintf(stderr, "fernsdr-release: could not write %s\n", path.c_str());
        return 1;
    }
    uint8_t key[32];
    ed25519_public_key(seed, key);
    printf("%s\n", to_hex(key, 32).c_str());
    return 0;
}

int public_key(const std::string& source) {
    uint8_t seed[32], key[32];
    if (!load_secret(source, seed)) return 1;
    ed25519_public_key(seed, key);
    printf("%s\n", to_hex(key, 32).c_str());
    return 0;
}

int public_keys() {
    // RFC 8410's SubjectPublicKeyInfo for Ed25519 is this fixed prefix, then
    // the 32 bytes of the key.
    static const uint8_t prefix[12] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
    for (const ReleaseKey& key : release_keys()) {
        uint8_t info[44];
        std::memcpy(info, prefix, sizeof(prefix));
        std::memcpy(info + sizeof(prefix), key.data(), key.size());
        printf("%s\n", base64_encode(info, sizeof(info)).c_str());
    }
    return 0;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

int manifest(const std::vector<std::string>& args) {
    if (args.size() < 6) {
        fprintf(stderr, "usage: fernsdr-release manifest OUT VERSION DATE CHANNEL NOTES-FILE ARCHIVE...\n");
        return 2;
    }
    ReleaseManifest release;
    release.version = args[1];
    release.date = args[2];
    release.channel = args[3];
    if (!read_file(args[4], release.notes)) {
        fprintf(stderr, "fernsdr-release: cannot read %s\n", args[4].c_str());
        return 1;
    }
    while (!release.notes.empty() && release.notes.back() == '\n') release.notes.pop_back();
    for (size_t i = 5; i < args.size(); i++) {
        const std::string name = base_name(args[i]), prefix = "fernsdr-" + release.version + "-";
        if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size() + 4 ||
            name.compare(name.size() - 4, 4, ".tar") != 0) {
            fprintf(stderr, "fernsdr-release: %s is not named fernsdr-%s-PLATFORM.tar\n", args[i].c_str(),
                    release.version.c_str());
            return 1;
        }
        std::string data, error;
        if (!read_file(args[i], data)) {
            fprintf(stderr, "fernsdr-release: cannot read %s\n", args[i].c_str());
            return 1;
        }
        // Receivers unpack it with this reader, so it has to read here.
        std::vector<UstarEntry> entries;
        if (!read_ustar(data, entries, error)) {
            fprintf(stderr, "fernsdr-release: %s: %s\n", args[i].c_str(), error.c_str());
            return 1;
        }
        release.assets.push_back({name.substr(prefix.size(), name.size() - prefix.size() - 4), name, data.size(),
                                  sha256_hex(data)});
    }
    std::string text, error;
    if (!format_release_manifest(release, text, error)) {
        fprintf(stderr, "fernsdr-release: %s\n", error.c_str());
        return 1;
    }
    if (!write_file(args[0], text)) {
        fprintf(stderr, "fernsdr-release: cannot write %s\n", args[0].c_str());
        return 1;
    }
    return 0;
}

int sign(const std::string& source, const std::string& path, bool test_key) {
    uint8_t seed[32];
    if (!load_secret(source, seed)) return 1;
    ReleaseKey key;
    ed25519_public_key(seed, key.data());
    const std::vector<ReleaseKey>& carried = release_keys();
    const bool known = std::find(carried.begin(), carried.end(), key) != carried.end();
    if (!known && !test_key) {
        // A signature by a key no receiver carries is refused by all of them.
        fprintf(stderr, "fernsdr-release: this secret's key, %s, is not in src/update/release_keys.h%s\n",
                to_hex(key.data(), 32).c_str(), carried.empty() ? ", which is empty" : "");
        return 1;
    }
    std::string text, error;
    ReleaseManifest release;
    if (!read_file(path, text)) {
        fprintf(stderr, "fernsdr-release: cannot read %s\n", path.c_str());
        return 1;
    }
    // Only a manifest that reads is signed: the key never signs anything
    // a receiver would refuse anyway.
    if (!parse_release_manifest(text, release, error)) {
        fprintf(stderr, "fernsdr-release: %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    const std::string message = release_signed_message(text);
    uint8_t signature[64];
    ed25519_sign(seed, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature);
    if (!write_file(path + ".sig", std::string(reinterpret_cast<const char*>(signature), 64))) {
        fprintf(stderr, "fernsdr-release: cannot write %s.sig\n", path.c_str());
        return 1;
    }
    return 0;
}

int verify(const std::vector<std::string>& args) {
    if (args.empty()) {
        fprintf(stderr, "usage: fernsdr-release verify MANIFEST [PUBLIC-KEY...]\n");
        return 2;
    }
    std::vector<ReleaseKey> keys;
    for (size_t i = 1; i < args.size(); i++) {
        ReleaseKey key;
        if (!hex_key(args[i], key.data()) || !ed25519_key_is_valid(key.data())) {
            fprintf(stderr, "fernsdr-release: %s is not a public key\n", args[i].c_str());
            return 2;
        }
        keys.push_back(key);
    }
    if (keys.empty()) keys = release_keys();
    std::string text, signature, error;
    if (!read_file(args[0], text) || !read_file(args[0] + ".sig", signature)) {
        fprintf(stderr, "fernsdr-release: cannot read %s and %s.sig\n", args[0].c_str(), args[0].c_str());
        return 1;
    }
    ReleaseManifest release;
    if (!verify_release_manifest(text, signature, keys, release, error)) {
        fprintf(stderr, "fernsdr-release: %s\n", error.c_str());
        return 1;
    }
    printf("FernSDR %s of %s, signed\n", release.version.c_str(), release.date.c_str());
    int failures = 0;
    for (const ReleaseAsset& asset : release.assets) {
        std::string data;
        const std::string path = directory_of(args[0]) + "/" + asset.file;
        if (!read_file(path, data)) {
            printf("  %s: %s MISSING\n", asset.platform.c_str(), asset.file.c_str());
            failures++;
            continue;
        }
        const bool good = data.size() == asset.size && sha256_hex(data) == asset.sha256;
        printf("  %s: %s %s\n", asset.platform.c_str(), asset.file.c_str(), good ? "matches" : "DOES NOT MATCH");
        if (!good) failures++;
    }
    return failures == 0 ? 0 : 1;
}

// Every file and directory below `root`, as paths relative to it.
bool walk(const std::string& root, const std::string& relative, std::vector<UstarInput>& out) {
    const std::string path = relative.empty() ? root : root + "/" + relative;
    DIR* directory = ::opendir(path.c_str());
    if (!directory) {
        fprintf(stderr, "fernsdr-release: cannot read %s\n", path.c_str());
        return false;
    }
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(directory);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string child = relative.empty() ? name : relative + "/" + name;
        struct stat info {};
        if (::lstat((root + "/" + child).c_str(), &info) != 0) {
            fprintf(stderr, "fernsdr-release: cannot stat %s/%s\n", root.c_str(), child.c_str());
            return false;
        }
        if (S_ISDIR(info.st_mode)) {
            out.push_back({child, true, false, ""});
            if (!walk(root, child, out)) return false;
        } else if (S_ISREG(info.st_mode)) {
            UstarInput file{child, false, (info.st_mode & S_IXUSR) != 0, ""};
            if (!read_file(root + "/" + child, file.contents)) {
                fprintf(stderr, "fernsdr-release: cannot read %s/%s\n", root.c_str(), child.c_str());
                return false;
            }
            out.push_back(std::move(file));
        } else {
            fprintf(stderr, "fernsdr-release: %s/%s is neither a file nor a directory\n", root.c_str(),
                    child.c_str());
            return false;
        }
    }
    return true;
}

int pack(const std::string& out, const std::string& root) {
    int64_t mtime = 0;
    if (const char* epoch = std::getenv("SOURCE_DATE_EPOCH")) {
        char* end = nullptr;
        mtime = std::strtoll(epoch, &end, 10);
        if (!*epoch || *end || mtime < 0) {
            fprintf(stderr, "fernsdr-release: SOURCE_DATE_EPOCH is not a time: %s\n", epoch);
            return 1;
        }
    }
    std::vector<UstarInput> inputs;
    if (!walk(root, "", inputs)) return 1;
    std::string archive, error;
    if (!write_ustar(inputs, mtime, archive, error)) {
        fprintf(stderr, "fernsdr-release: %s\n", error.c_str());
        return 1;
    }
    if (!write_file(out, archive)) {
        fprintf(stderr, "fernsdr-release: cannot write %s\n", out.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string command = args.empty() ? "" : args[0];
    const std::vector<std::string> rest(args.empty() ? args.begin() : args.begin() + 1, args.end());
    if (command == "keygen" && rest.size() == 1) return keygen(rest[0]);
    if (command == "public-key" && rest.size() == 1) return public_key(rest[0]);
    if (command == "public-keys" && rest.empty()) return public_keys();
    if (command == "manifest") return manifest(rest);
    if (command == "sign" && rest.size() == 2) return sign(rest[0], rest[1], false);
    if (command == "sign" && rest.size() == 3 && rest[0] == "--test-key") return sign(rest[1], rest[2], true);
    if (command == "verify") return verify(rest);
    if (command == "pack" && rest.size() == 2) return pack(rest[0], rest[1]);
    fprintf(stderr,
            "usage: fernsdr-release keygen SECRET-FILE\n"
            "       fernsdr-release public-key SECRET\n"
            "       fernsdr-release public-keys\n"
            "       fernsdr-release manifest OUT VERSION DATE CHANNEL NOTES-FILE ARCHIVE...\n"
            "       fernsdr-release sign [--test-key] SECRET MANIFEST\n"
            "       fernsdr-release verify MANIFEST [PUBLIC-KEY...]\n"
            "       fernsdr-release pack OUT DIRECTORY\n"
            "SECRET is a file holding the secret in hex, or env:NAME.\n");
    return 2;
}
