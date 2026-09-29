#include "ustar.h"

#include <algorithm>
#include <cstring>
#include <set>

// The header is POSIX.1-1988 ustar: 512 bytes, text fields NUL-terminated
// unless full, numbers in octal ASCII. What this reads is what release
// tooling writes, and what `tar --format=ustar` writes: the GNU and pax
// extensions (long names, extended headers, base-256 numbers) are refused
// rather than half understood.
namespace fernsdr {

namespace {

constexpr size_t kBlock = 512;

struct Field {
    size_t offset, length;
};
constexpr Field kName{0, 100}, kMode{100, 8}, kUid{108, 8}, kGid{116, 8}, kSize{124, 12}, kMtime{136, 12},
    kChecksum{148, 8}, kType{156, 1}, kMagic{257, 6}, kVersion{263, 2}, kUname{265, 32}, kGname{297, 32},
    kPrefix{345, 155};

std::string text_field(const char* header, Field field) {
    const char* start = header + field.offset;
    return std::string(start, strnlen(start, field.length));
}

// Octal digits, optionally led by spaces and ended by NUL or spaces, as
// writers differ in both.
bool octal_field(const char* header, Field field, uint64_t& value) {
    size_t i = 0;
    const char* start = header + field.offset;
    while (i < field.length && start[i] == ' ') i++;
    const size_t first = i;
    value = 0;
    for (; i < field.length && start[i] >= '0' && start[i] <= '7'; i++) {
        if (value > (UINT64_MAX >> 3)) return false;
        value = value * 8 + static_cast<uint64_t>(start[i] - '0');
    }
    if (i == first) return false;
    for (; i < field.length; i++) {
        if (start[i] != ' ' && start[i] != '\0') return false;
    }
    return true;
}

bool all_zero(const char* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

uint64_t checksum_of(const char* header) {
    uint64_t sum = 0;
    for (size_t i = 0; i < kBlock; i++) {
        const bool in_field = i >= kChecksum.offset && i < kChecksum.offset + kChecksum.length;
        sum += in_field ? static_cast<uint64_t>(' ') : static_cast<unsigned char>(header[i]);
    }
    return sum;
}

// A path the extraction can put under its own directory and nowhere else:
// relative, no empty, "." or ".." component, and only characters a release's
// file names use, so nothing needs quoting or means something to a shell.
bool safe_path(const std::string& path, size_t max_depth, std::string& error) {
    if (path.empty() || path.size() > 255) {
        error = "an entry has an empty or overlong path";
        return false;
    }
    if (path.front() == '/') {
        error = "an entry has an absolute path: " + path;
        return false;
    }
    size_t depth = 0;
    for (size_t begin = 0; begin <= path.size(); depth++) {
        size_t end = path.find('/', begin);
        if (end == std::string::npos) end = path.size();
        const std::string component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") {
            error = "an entry's path has an empty, '.' or '..' component: " + path;
            return false;
        }
        for (const char c : component) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                 c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == '~';
            if (!allowed) {
                error = "an entry's path has a character releases do not use: " + path;
                return false;
            }
        }
        begin = end + 1;
    }
    if (depth > max_depth) {
        error = "an entry's path is nested too deeply: " + path;
        return false;
    }
    return true;
}

// Every ancestor is a directory, and a file has no descendants: an archive
// cannot put a file where it also puts a directory, whichever comes first.
bool fits_tree(const std::set<std::string>& files, const std::set<std::string>& all, const UstarEntry& entry,
               std::string& error) {
    if (all.count(entry.path)) {
        error = "an archive names a path twice: " + entry.path;
        return false;
    }
    for (size_t slash = entry.path.find('/'); slash != std::string::npos; slash = entry.path.find('/', slash + 1)) {
        if (files.count(entry.path.substr(0, slash))) {
            error = "an entry's path runs through a file: " + entry.path;
            return false;
        }
    }
    if (!entry.directory) {
        const std::string below = entry.path + "/";
        const auto next = all.lower_bound(below);
        if (next != all.end() && next->compare(0, below.size(), below) == 0) {
            error = "a file has the name of a directory: " + entry.path;
            return false;
        }
    }
    return true;
}

}  // namespace

bool read_ustar(const std::string& archive, std::vector<UstarEntry>& entries, std::string& error,
                const UstarLimits& limits) {
    entries.clear();
    std::set<std::string> files, all;
    size_t total = 0;
    size_t at = 0;
    while (true) {
        if (archive.size() - at < kBlock) {
            error = "the archive ends before its end-of-archive blocks";
            return false;
        }
        const char* header = archive.data() + at;
        if (all_zero(header, kBlock)) {
            // Two zero blocks end it, and nothing but zeros may follow: a
            // second archive appended behind the first would be a way to
            // smuggle past a reader that stops early.
            if (archive.size() - at < 2 * kBlock || !all_zero(header + kBlock, kBlock)) {
                error = "the archive ends with one zero block instead of two";
                return false;
            }
            if (!all_zero(archive.data() + at + 2 * kBlock, archive.size() - at - 2 * kBlock)) {
                error = "the archive has data after its end";
                return false;
            }
            return true;
        }

        uint64_t stored = 0;
        if (!octal_field(header, kChecksum, stored) || stored != checksum_of(header)) {
            error = "an entry's header checksum is wrong";
            return false;
        }
        if (std::memcmp(header + kMagic.offset, "ustar\0", 6) != 0 ||
            std::memcmp(header + kVersion.offset, "00", 2) != 0) {
            error = "the archive is not POSIX ustar";
            return false;
        }

        UstarEntry entry;
        const std::string name = text_field(header, kName), prefix = text_field(header, kPrefix);
        entry.path = prefix.empty() ? name : prefix + "/" + name;
        const char type = header[kType.offset];
        if (type == '5') {
            entry.directory = true;
            if (!entry.path.empty() && entry.path.back() == '/') entry.path.pop_back();
        } else if (type != '0' && type != '\0') {
            error = "an entry is neither a file nor a directory: " + entry.path;
            return false;
        }
        if (!safe_path(entry.path, limits.max_depth, error)) return false;

        uint64_t mode = 0, size = 0, unused = 0;
        if (!octal_field(header, kMode, mode) || !octal_field(header, kSize, size) ||
            !octal_field(header, kUid, unused) || !octal_field(header, kGid, unused) ||
            !octal_field(header, kMtime, unused)) {
            error = "an entry has a malformed number: " + entry.path;
            return false;
        }
        if (entry.directory && size != 0) {
            error = "a directory has contents: " + entry.path;
            return false;
        }
        if (size > limits.max_file_size || size > limits.max_total_size - total) {
            error = "the archive is larger than a release can be";
            return false;
        }
        const size_t padded = (static_cast<size_t>(size) + kBlock - 1) / kBlock * kBlock;
        if (archive.size() - at - kBlock < padded) {
            error = "the archive ends inside " + entry.path;
            return false;
        }
        entry.executable = !entry.directory && (mode & 0111) != 0;
        entry.offset = at + kBlock;
        entry.size = static_cast<size_t>(size);
        total += entry.size;

        if (!fits_tree(files, all, entry, error)) return false;
        if (entries.size() == limits.max_entries) {
            error = "the archive has more entries than a release can";
            return false;
        }
        all.insert(entry.path);
        if (!entry.directory) files.insert(entry.path);
        entries.push_back(entry);
        at += kBlock + padded;
    }
}

namespace {

void put_text(char* header, Field field, const std::string& text) {
    std::memcpy(header + field.offset, text.data(), std::min(text.size(), field.length));
}

void put_octal(char* header, Field field, uint64_t value) {
    // length - 1 digits and a NUL, the form every reader takes.
    const size_t digits = field.length - 1;
    for (size_t i = 0; i < digits; i++) {
        header[field.offset + digits - 1 - i] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    header[field.offset + digits] = '\0';
}

}  // namespace

bool write_ustar(const std::vector<UstarInput>& inputs, int64_t mtime, std::string& archive, std::string& error) {
    archive.clear();
    if (mtime < 0 || mtime >= (int64_t{1} << 33)) {
        error = "the modification time does not fit a header";
        return false;
    }
    for (const UstarInput& input : inputs) {
        if (!safe_path(input.path, UstarLimits{}.max_depth, error)) return false;
        if (input.directory && !input.contents.empty()) {
            error = "a directory cannot have contents: " + input.path;
            return false;
        }
        if (input.contents.size() >= (uint64_t{1} << 33)) {
            error = "a file is too large for a header: " + input.path;
            return false;
        }
        // Directories carry the trailing slash tar writes for them, unless
        // there is no room for it: the type says what they are either way. A
        // path too long for the name field is split at a slash into prefix
        // and name.
        std::string name, prefix;
        const auto fits = [&](const std::string& stored) {
            prefix.clear();
            name = stored;
            if (stored.size() <= kName.length) return true;
            for (size_t slash = stored.find('/'); slash != std::string::npos && slash < stored.size() - 1;
                 slash = stored.find('/', slash + 1)) {
                if (slash <= kPrefix.length && stored.size() - slash - 1 <= kName.length) {
                    prefix = stored.substr(0, slash);
                    name = stored.substr(slash + 1);
                    return true;
                }
            }
            return false;
        };
        if (!(input.directory && fits(input.path + "/")) && !fits(input.path)) {
            error = "a path does not fit a ustar header: " + input.path;
            return false;
        }

        char header[kBlock] = {};
        put_text(header, kName, name);
        put_octal(header, kMode, input.directory || input.executable ? 0755 : 0644);
        put_octal(header, kUid, 0);
        put_octal(header, kGid, 0);
        put_octal(header, kSize, input.contents.size());
        put_octal(header, kMtime, static_cast<uint64_t>(mtime));
        header[kType.offset] = input.directory ? '5' : '0';
        std::memcpy(header + kMagic.offset, "ustar\0", 6);
        std::memcpy(header + kVersion.offset, "00", 2);
        put_text(header, kUname, "root");
        put_text(header, kGname, "root");
        put_text(header, kPrefix, prefix);
        // Six digits, a NUL and a space, as tar writes it.
        std::memset(header + kChecksum.offset, ' ', kChecksum.length);
        uint64_t sum = checksum_of(header);
        for (int i = 5; i >= 0; i--) {
            header[kChecksum.offset + static_cast<size_t>(i)] = static_cast<char>('0' + (sum & 7));
            sum >>= 3;
        }
        header[kChecksum.offset + 6] = '\0';

        archive.append(header, kBlock);
        archive += input.contents;
        archive.append((kBlock - input.contents.size() % kBlock) % kBlock, '\0');
    }
    archive.append(2 * kBlock, '\0');

    std::vector<UstarEntry> check;
    std::string why;
    if (!read_ustar(archive, check, why)) {
        error = "the archive would not read back: " + why;
        archive.clear();
        return false;
    }
    return true;
}

}  // namespace fernsdr
