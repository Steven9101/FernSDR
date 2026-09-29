// The ustar reader refuses everything an archive could use to write outside
// the directory it is unpacked into, or to be read two ways: each case below
// is a valid archive from write_ustar with one thing changed.
#include "../src/update/ustar.h"
#include "test_util.h"

#include <cstring>
#include <string>
#include <vector>

using fernsdr::read_ustar;
using fernsdr::UstarEntry;
using fernsdr::UstarInput;
using fernsdr::write_ustar;

namespace {

std::string archive_of(const std::vector<UstarInput>& inputs, int64_t mtime = 1700000000) {
    std::string archive, error;
    CHECK(write_ustar(inputs, mtime, archive, error));
    if (!error.empty()) fprintf(stderr, "    write_ustar: %s\n", error.c_str());
    return archive;
}

// Writes `value` into the header at `header` and sets its checksum right, so
// that only the change itself is on trial.
void patch(std::string& archive, size_t header, size_t offset, const std::string& value) {
    std::memcpy(&archive[header + offset], value.data(), value.size());
    std::memset(&archive[header + 148], ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < 512; i++) sum += static_cast<unsigned char>(archive[header + i]);
    char digits[8];
    snprintf(digits, sizeof(digits), "%06o", sum);
    std::memcpy(&archive[header + 148], digits, 7);
}

bool refused(const std::string& archive, const std::string& expected_reason) {
    std::vector<UstarEntry> entries;
    std::string error;
    const bool read = read_ustar(archive, entries, error);
    if (read || error.find(expected_reason) == std::string::npos) {
        fprintf(stderr, "    expected a refusal mentioning \"%s\", got %s\"%s\"\n", expected_reason.c_str(),
                read ? "success " : "", error.c_str());
        return false;
    }
    return true;
}

std::string name_field(const std::string& name) {
    std::string field(100, '\0');
    std::memcpy(&field[0], name.data(), name.size());
    return field;
}

}  // namespace

TEST_CASE(ustar_reads_back_what_it_writes) {
    const std::string long_path =
        "web/assets/" + std::string(90, 'a') + "/" + std::string(40, 'b') + "/index-4f1c2e.js";
    const std::vector<UstarInput> inputs = {
        {"fernsdr", false, true, "\x7f" "ELF binary"},
        {"web", true, false, ""},
        {"web/index.html", false, false, "<!doctype html>"},
        {"web/empty.txt", false, false, ""},
        {"web/block.bin", false, false, std::string(512, 'x')},
        {"web/block-and-one.bin", false, false, std::string(513, 'y')},
        {long_path, false, false, "long"},
    };
    const std::string archive = archive_of(inputs);
    CHECK_EQ(archive.size() % 512, 0u);
    std::vector<UstarEntry> entries;
    std::string error;
    CHECK(read_ustar(archive, entries, error));
    CHECK_EQ(entries.size(), inputs.size());
    for (size_t i = 0; i < entries.size() && i < inputs.size(); i++) {
        CHECK_EQ_STR(entries[i].path, inputs[i].path);
        CHECK_EQ(entries[i].directory, inputs[i].directory);
        CHECK_EQ(entries[i].executable, inputs[i].executable);
        CHECK_EQ_STR(archive.substr(entries[i].offset, entries[i].size), inputs[i].contents);
    }
    // The same tree gives the same bytes; the time is the only thing it records.
    CHECK(archive == archive_of(inputs));
    CHECK(archive != archive_of(inputs, 1700000001));
}

TEST_CASE(ustar_refuses_anything_but_files_and_directories) {
    // Hard and symbolic links, devices, a FIFO, pax extended headers and
    // GNU long names: each could point extraction elsewhere or rename what
    // follows it.
    for (const char type : {'1', '2', '3', '4', '6', '7', 'x', 'g', 'L', 'K'}) {
        std::string archive = archive_of({{"link", false, false, ""}});
        patch(archive, 0, 156, std::string(1, type));
        CHECK(refused(archive, "neither a file nor a directory"));
    }
}

TEST_CASE(ustar_refuses_paths_that_leave_the_directory) {
    const std::string base = archive_of({{"placeholder-file", false, false, "x"}});
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"/etc/passwd", "absolute"},
        {"../etc/passwd", "component"},
        {"web/../../etc/passwd", "component"},
        {"web/./index.html", "component"},
        {"web//index.html", "component"},
        {"web\\index.html", "character"},
        {"web/index.html ", "character"},
        {"web/C:evil", "character"},
    };
    for (const auto& [path, reason] : cases) {
        std::string archive = base;
        patch(archive, 0, 0, name_field(path));
        CHECK(refused(archive, reason));
    }
    // The prefix field is part of the path too.
    std::string archive = base;
    patch(archive, 0, 345, std::string("..") + std::string(153, '\0'));
    CHECK(refused(archive, "component"));
}

TEST_CASE(ustar_refuses_one_path_two_ways) {
    // Twice the same file: which one is meant?
    std::string twice = archive_of({{"a", false, false, "1"}, {"b", false, false, "2"}});
    patch(twice, 1024, 0, name_field("a"));
    CHECK(refused(twice, "twice"));
    // A file, then something below it; something below a name, then a file
    // of that name; a directory and a file of one name.
    CHECK(refused(archive_of({{"a", false, false, "1"}}).substr(0, 1024) +
                      archive_of({{"a/b", false, false, "2"}}),
                  "through a file"));
    CHECK(refused(archive_of({{"a/b", false, false, "2"}}).substr(0, 1024) + archive_of({{"a", false, false, "1"}}),
                  "name of a directory"));
    CHECK(refused(archive_of({{"a", true, false, ""}}).substr(0, 512) + archive_of({{"a", false, false, "1"}}),
                  "twice"));
}

TEST_CASE(ustar_refuses_damaged_or_extended_archives) {
    const std::string base = archive_of({{"file", false, false, std::string(700, 'z')}});
    // A header changed without its checksum.
    std::string damaged = base;
    damaged[10] = 'Q';
    CHECK(refused(damaged, "checksum"));
    // GNU's own format, and a version other than 00.
    std::string gnu = base;
    patch(gnu, 0, 257, std::string("ustar  \0", 8));
    CHECK(refused(gnu, "not POSIX ustar"));
    // Cut off inside the file, before the end blocks, after one of them.
    CHECK(refused(base.substr(0, 1024), "ends inside"));
    CHECK(refused(base.substr(0, 1536), "before its end"));
    CHECK(refused(base.substr(0, 2048), "one zero block"));
    // Something after the end: a second archive a lax reader would never see.
    CHECK(refused(base + archive_of({{"hidden", false, false, "x"}}), "after its end"));
    // Zeros after the end are padding, as tar writes to fill a record.
    std::vector<UstarEntry> entries;
    std::string error;
    CHECK(read_ustar(base + std::string(8192, '\0'), entries, error));
    // Numbers that are not octal, or not ended.
    std::string size = base;
    patch(size, 0, 124, "0000000128x\0");
    CHECK(refused(size, "malformed number"));
    std::string mode = base;
    patch(mode, 0, 100, "9999999\0");
    CHECK(refused(mode, "malformed number"));
    // A directory with contents.
    std::string directory = archive_of({{"dir", true, false, ""}});
    patch(directory, 0, 124, "00000000001\0");
    CHECK(refused(directory + std::string(512, '\0'), "directory has contents"));
}

TEST_CASE(ustar_holds_to_its_limits) {
    fernsdr::UstarLimits limits;
    limits.max_entries = 2;
    limits.max_file_size = 1000;
    limits.max_total_size = 1500;
    limits.max_depth = 3;
    std::vector<UstarEntry> entries;
    std::string error;
    CHECK(read_ustar(archive_of({{"a", false, false, "1"}, {"b", false, false, "2"}}), entries, error, limits));
    CHECK(!read_ustar(archive_of({{"a", false, false, "1"}, {"b", false, false, "2"}, {"c", false, false, "3"}}),
                      entries, error, limits));
    CHECK(error.find("more entries") != std::string::npos);
    CHECK(!read_ustar(archive_of({{"a", false, false, std::string(1001, 'x')}}), entries, error, limits));
    const std::string two_of_800 =
        archive_of({{"a", false, false, std::string(800, 'x')}, {"b", false, false, std::string(800, 'x')}});
    CHECK(!read_ustar(two_of_800, entries, error, limits));
    CHECK(error.find("larger than a release") != std::string::npos);
    CHECK(read_ustar(archive_of({{"a/b/c", false, false, "x"}}), entries, error, limits));
    CHECK(!read_ustar(archive_of({{"a/b/c/d", false, false, "x"}}), entries, error, limits));
    CHECK(error.find("nested too deeply") != std::string::npos);
}

// A directory whose name fills the 100-byte field, which tar writes without
// the trailing slash it has no room for: read, then written back.
TEST_CASE(ustar_writes_back_a_directory_whose_name_fills_the_field) {
    for (const std::string& prefix : {std::string(), std::string("x")}) {
        std::string archive = archive_of({{"d", true, false, ""}});
        patch(archive, 0, 0, std::string(100, 'a'));
        if (!prefix.empty()) patch(archive, 0, 345, prefix);
        std::vector<UstarEntry> entries;
        std::string error;
        CHECK(read_ustar(archive, entries, error));
        CHECK_EQ(entries.size(), 1u);
        if (entries.size() != 1) continue;
        CHECK(entries[0].directory);
        std::string again;
        CHECK(write_ustar({{entries[0].path, true, false, ""}}, 0, again, error));
        std::vector<UstarEntry> back;
        CHECK(read_ustar(again, back, error));
        CHECK(back.size() == 1 && back[0].path == entries[0].path && back[0].directory);
    }
}

TEST_CASE(ustar_writes_only_what_it_can_read_back) {
    std::string archive, error;
    CHECK(!write_ustar({{"../escape", false, false, "x"}}, 0, archive, error));
    CHECK(!write_ustar({{"dir", true, false, "contents"}}, 0, archive, error));
    CHECK(!write_ustar({{std::string(101, 'a'), false, false, "no slash to split at"}}, 0, archive, error));
    CHECK(!write_ustar({{"a", false, false, "1"}, {"a", false, false, "2"}}, 0, archive, error));
    CHECK(!write_ustar({{"a", false, false, "x"}}, -1, archive, error));
    CHECK(archive.empty());
}
