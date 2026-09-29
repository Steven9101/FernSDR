// POSIX ustar archives, the container a release ships in: uncompressed, so the
// receiver needs no inflate, and written by release tooling that puts in
// nothing but files and directories.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fernsdr {

struct UstarEntry {
    std::string path;  // relative, '/'-separated, without a trailing slash
    bool directory = false;
    bool executable = false;  // any execute bit in the archived mode
    size_t offset = 0;        // where a file's bytes start in the archive
    size_t size = 0;
};

// Limits a release is nowhere near: its archive is a few megabytes of a few
// hundred files.
struct UstarLimits {
    size_t max_entries = 4096;
    size_t max_file_size = 64u << 20;
    size_t max_total_size = 256u << 20;
    size_t max_depth = 16;
};

// Lists an archive's entries, or says why it will not: anything but regular
// files and directories (links, devices, extended headers), a path that is
// absolute, climbs out with "..", repeats or runs through a file, a header
// whose checksum is wrong, or bytes after the end that are not zero. `archive`
// has to stay alive while the offsets are used.
bool read_ustar(const std::string& archive, std::vector<UstarEntry>& entries, std::string& error,
                const UstarLimits& limits = {});

// One file or directory to write. Directories are written as given; nothing
// makes parents that are missing.
struct UstarInput {
    std::string path;
    bool directory = false;
    bool executable = false;
    std::string contents;
};

// Writes entries as ustar with owner 0:0, modes 0755 and 0644 and the one
// modification time given, so the same tree always gives the same archive.
// False when a path does not fit a header or would not read back.
bool write_ustar(const std::vector<UstarInput>& inputs, int64_t mtime, std::string& archive, std::string& error);

}  // namespace fernsdr
