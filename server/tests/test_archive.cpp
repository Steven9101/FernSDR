#include "../src/core/archive.h"
#include "../src/util/log.h"
#include "test_util.h"

#include <algorithm>
#include <sys/resource.h>
#include <sys/stat.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

using fernsdr::WaterfallArchive;

namespace {

std::string temp_path(const char* name) { return std::string("/tmp/fernsdr-test-") + name + ".wfa"; }

/** A line with a carrier at `bin`, so it can be recognised on the way out. */
std::vector<float> line_with_carrier(size_t count, size_t bin, float level = -30.0f) {
    std::vector<float> line(count, -100.0f);
    if (bin < count) line[bin] = level;
    return line;
}

// The fault tests also run against version 1, to establish the regression.
struct DiskHeader {
    char magic[8];
    uint32_t bins;
    uint32_t checksum;
    double seconds_per_line;
    uint64_t capacity;
    uint64_t next_index;
    int64_t epoch_ms;
};
static_assert(sizeof(DiskHeader) == 48);

std::vector<uint8_t> file_bytes(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (!file) return {};
    CHECK_EQ(std::fseek(file, 0, SEEK_END), 0);
    const long size = std::ftell(file);
    CHECK(size > 0);
    std::rewind(file);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    CHECK_EQ(std::fread(bytes.data(), 1, bytes.size(), file), bytes.size());
    std::fclose(file);
    return bytes;
}

void replace_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    if (!file) return;
    CHECK_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    CHECK_EQ(std::fclose(file), 0);
}

size_t rows_offset(const std::vector<uint8_t>& bytes) {
    return std::memcmp(bytes.data(), "FRNWFA2", 8) == 0 ? 8192 : sizeof(DiskHeader);
}

}  // namespace

TEST_CASE(archive_states_its_cost_before_it_is_used) {
    const std::string path = temp_path("cost");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 1024, 1.0, 24, error));
    // 24 hours at one line a second: 86 400 slots of 1024 bytes plus an index.
    CHECK(archive.capacity() == 86400);
    CHECK(archive.size_bytes() > 86400ull * 1024);
    CHECK(archive.size_bytes() < 100ull * 1000 * 1000);

    // The file is that size immediately, not a day later. Being told what an
    // archive costs is worth nothing if the number only becomes true later.
    std::FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    CHECK(static_cast<uint64_t>(std::ftell(f)) == archive.size_bytes());
    std::fclose(f);
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_reads_back_what_it_recorded) {
    const std::string path = temp_path("roundtrip");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 64, 1.0, 1, error));

    const int64_t start = 1'700'000'000'000;
    for (int i = 0; i < 10; i++) {
        const auto line = line_with_carrier(256, static_cast<size_t>(i) * 4);
        archive.append(line.data(), line.size(), start + i * 1000);
    }

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 9000, rows, times));
    CHECK(times.size() == 10);
    CHECK(rows.size() == 10 * 64);
    CHECK(times[0] == start);
    CHECK(times[9] == start + 9000);

    // The carrier moved one archive bin per line: 256 source bins into 64.
    for (size_t i = 0; i < 10; i++) {
        const uint8_t* row = rows.data() + i * 64;
        CHECK(row[i] > row[(i + 5) % 64]);
    }
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_keeps_a_carrier_that_a_mean_would_lose) {
    const std::string path = temp_path("peak");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 16, 1.0, 1, error));

    // One strong bin in a thousand of noise. Averaged into sixteen cells it
    // would vanish; a waterfall is read for what was there.
    std::vector<float> line(1024, -110.0f);
    line[500] = -30.0f;
    archive.append(line.data(), line.size(), 1'700'000'000'000);

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(1'700'000'000'000, 1'700'000'001'000, rows, times));
    CHECK(rows.size() == 16);
    CHECK(rows[500 * 16 / 1024] > 200);
}

TEST_CASE(archive_does_not_grow_past_its_ring) {
    const std::string path = temp_path("ring");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    // Four slots, so it laps quickly.
    CHECK(archive.open(path, 8, 1.0, 1, error));
    const uint64_t size = archive.size_bytes();

    const int64_t start = 1'700'000'000'000;
    for (int i = 0; i < 10000; i++) {
        const auto line = line_with_carrier(8, static_cast<size_t>(i) % 8);
        archive.append(line.data(), line.size(), start + i * 1000);
    }
    archive.close();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::fseek(f, 0, SEEK_END);
    // The whole point: an archive that can grow eventually fills the disk of a
    // machine nobody is watching.
    CHECK(static_cast<uint64_t>(std::ftell(f)) == size);
    std::fclose(f);
    std::remove(path.c_str());
}

TEST_CASE(archive_forgets_the_oldest_first) {
    const std::string path = temp_path("forget");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 8, 1.0, 1, error));
    const size_t capacity = archive.capacity();

    const int64_t start = 1'700'000'000'000;
    for (size_t i = 0; i < capacity + 100; i++) {
        const auto line = line_with_carrier(8, i % 8);
        archive.append(line.data(), line.size(), start + static_cast<int64_t>(i) * 1000);
    }

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    // The very first lines are gone.
    CHECK(archive.read(start, start + 50000, rows, times));
    CHECK(times.empty());
    // The most recent are there.
    const int64_t recent = start + static_cast<int64_t>(capacity + 50) * 1000;
    CHECK(archive.read(recent, recent + 40000, rows, times));
    CHECK(!times.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_past_two_gigabytes_records_and_reads_at_its_end) {
    // Thirty days of a thousand bins a second is 2.7 GB: past what a 32-bit
    // long or off_t reaches, which armhf has unless large files are asked
    // for. The file is sparse, so it costs the test a few blocks.
    const std::string path = temp_path("large");
    std::remove(path.c_str());
    const int64_t start = 1'700'000'000'000;
    const int64_t late = start + 2'500'000LL * 1000;  // slot 2 500 000, 2.58 GB in
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 1024, 1.0, 24 * 30, error));
        CHECK(archive.size_bytes() > (1ull << 31));
        struct stat info {};
        CHECK(::stat(path.c_str(), &info) == 0);
        CHECK_EQ(static_cast<uint64_t>(info.st_size), archive.size_bytes());
        const auto first = line_with_carrier(1024, 100);
        archive.append(first.data(), first.size(), start);
        const auto line = line_with_carrier(1024, 700);
        archive.append(line.data(), line.size(), late);
        archive.close();
    }
    WaterfallArchive again;
    std::string error;
    CHECK(again.open(path, 1024, 1.0, 24 * 30, error));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(again.read(late, late, rows, times));
    CHECK_EQ(times.size(), 1u);
    if (times.size() == 1) {
        CHECK_EQ(times[0], late);
        CHECK(rows[700] > 200 && rows[100] < 100);
    }
    again.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_says_when_a_line_cannot_be_written) {
    // A file size limit fails writes past it as a full disk does, on the
    // archive's own descriptor: the second slot cannot be written, then can.
    const std::string path = temp_path("full");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 32, 1.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(32, 5);
    archive.append(line.data(), line.size(), start);
    CHECK(!archive.failing());

    rlimit previous {};
    CHECK(::getrlimit(RLIMIT_FSIZE, &previous) == 0);
    const auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = previous;
    limited.rlim_cur = 48 + 40;  // the header and the first slot of 8 + 32 bytes
    CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    archive.append(line.data(), line.size(), start + 1000);
    const bool failed = archive.failing();
    CHECK(::setrlimit(RLIMIT_FSIZE, &previous) == 0);
    ::signal(SIGXFSZ, old_handler);
    CHECK(failed);

    archive.append(line.data(), line.size(), start + 2000);
    CHECK(!archive.failing());
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 2000, rows, times));
    CHECK(!times.empty() && times.front() == start && times.back() == start + 2000);
    archive.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_survives_being_reopened) {
    const std::string path = temp_path("reopen");
    std::remove(path.c_str());
    const int64_t start = 1'700'000'000'000;
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error));
        for (int i = 0; i < 5; i++) {
            const auto line = line_with_carrier(32, 7);
            archive.append(line.data(), line.size(), start + i * 1000);
        }
        archive.close();
    }
    WaterfallArchive again;
    std::string error;
    CHECK(again.open(path, 32, 1.0, 1, error));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(again.read(start, start + 5000, rows, times));
    // An archive that cannot outlive a restart is not an archive.
    CHECK(times.size() == 5);
    CHECK(rows[7] > 200);
    again.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_starts_again_when_its_shape_changes) {
    const std::string path = temp_path("reshape");
    std::remove(path.c_str());
    const int64_t start = 1'700'000'000'000;
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error));
        const auto line = line_with_carrier(32, 7);
        archive.append(line.data(), line.size(), start);
        archive.close();
    }
    // The operator widened the archive. Reading the old lines at the new width
    // would draw nonsense, and quietly drawing nonsense is the worse outcome.
    WaterfallArchive wider;
    std::string error;
    CHECK(wider.open(path, 64, 1.0, 1, error));
    CHECK(wider.bins() == 64);
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(wider.read(start, start + 1000, rows, times));
    CHECK(times.empty());
    wider.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_only_takes_a_line_when_one_is_due) {
    const std::string path = temp_path("rate");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 16, 1.0, 1, error));
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        CHECK(file != nullptr);
        if (file) {
            char magic[8]{};
            CHECK_EQ(std::fread(magic, 1, sizeof(magic), file), sizeof(magic));
            CHECK(std::memcmp(magic, "FRNWFA2", 8) == 0);
            std::fclose(file);
        }
    }

    const int64_t start = 1'700'000'000'000;
    // A band offers 25 lines a second; the archive wants one.
    for (int i = 0; i < 250; i++) {
        const auto line = line_with_carrier(16, 3);
        archive.append(line.data(), line.size(), start + i * 40);
    }
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 10000, rows, times));
    CHECK(times.size() == 10);
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_leaves_a_gap_where_there_was_a_gap) {
    const std::string path = temp_path("gap");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 16, 1.0, 1, error));

    const int64_t start = 1'700'000'000'000;
    for (int i = 0; i < 3; i++) {
        const auto line = line_with_carrier(16, 3);
        archive.append(line.data(), line.size(), start + i * 1000);
    }
    // The band stopped for a minute. Time and position have to stay in step,
    // or everything after the gap is filed under the wrong time.
    for (int i = 0; i < 3; i++) {
        const auto line = line_with_carrier(16, 3);
        archive.append(line.data(), line.size(), start + 60000 + i * 1000);
    }

    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 70000, rows, times));
    CHECK(times.size() == 6);
    CHECK(times[2] == start + 2000);
    CHECK(times[3] >= start + 59000);
    // The minute in between returns nothing at all.
    std::vector<uint8_t> empty_rows;
    std::vector<int64_t> empty_times;
    CHECK(archive.read(start + 10000, start + 50000, empty_rows, empty_times));
    CHECK(empty_times.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_refuses_a_shape_that_makes_no_sense) {
    WaterfallArchive archive;
    std::string error;
    CHECK(!archive.open(temp_path("bad"), 0, 1.0, 24, error));
    CHECK(!error.empty());
    CHECK(!archive.open(temp_path("bad"), 1024, 0.0, 24, error));
    CHECK(!archive.open(temp_path("bad"), 1024, 1.0, 0, error));
    CHECK(!archive.open(temp_path("bad"), 1024, std::numeric_limits<double>::quiet_NaN(), 1, error));
    CHECK(!archive.is_open());
}

TEST_CASE(archive_bounds_work_and_memory_for_extreme_time_ranges) {
    const std::string path = temp_path("bounded");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 32, 1.0, 3, error));
    const auto line = line_with_carrier(32, 8);
    const int64_t start = 1'700'000'000'000;
    for (int i = 0; i < 9000; ++i) archive.append(line.data(), line.size(), start + i * 1000);
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(INT64_MIN, INT64_MAX, rows, times));
    CHECK(!times.empty());
    CHECK(times.size() <= 4096);
    CHECK(rows.size() == times.size() * 32);
    CHECK(times.front() == start);
    CHECK(times.back() > start + 8900 * 1000);
    CHECK(archive.read(INT64_MAX - 100, INT64_MAX, rows, times));
    CHECK(rows.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_quantisation_round_trips_within_half_a_step) {
    // Half a dB either way, which is finer than a waterfall is read to.
    for (float db = -140.0f; db <= -20.0f; db += 0.7f) {
        const float back = WaterfallArchive::dequantise(WaterfallArchive::quantise(db));
        CHECK(std::fabs(back - db) < 0.5f);
    }
    CHECK(WaterfallArchive::quantise(-200.0f) == 0);
    CHECK(WaterfallArchive::quantise(0.0f) == 255);
}

TEST_CASE(archive_keeps_its_rows_through_a_correction_it_cannot_resolve) {
    const std::string path = temp_path("span-small");
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
    const int64_t start = 1'700'000'000'000;
    // 32 cells over 200 kHz: 6.25 kHz each. Half a kilohertz, what a crystal
    // correction moves an HF band by, is far inside one.
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error, 7.0e6, 7.2e6));
        const auto line = line_with_carrier(32, 7);
        archive.append(line.data(), line.size(), start);
        archive.close();
    }
    WaterfallArchive again;
    std::string error;
    CHECK(again.open(path, 32, 1.0, 1, error, 7.0e6 + 500.0, 7.2e6 + 510.0));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(again.read(start, start + 1000, rows, times));
    CHECK_EQ(times.size(), 1);
    again.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_starts_again_when_its_frequencies_move) {
    const std::string path = temp_path("span-moved");
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
    const int64_t start = 1'700'000'000'000;
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error, 7.0e6, 7.2e6));
        const auto line = line_with_carrier(32, 7);
        archive.append(line.data(), line.size(), start);
        archive.close();
    }
    // Moved by more than a quarter of a cell: yesterday's carriers would be
    // drawn beside where they were.
    WaterfallArchive moved;
    std::string error;
    CHECK(moved.open(path, 32, 1.0, 1, error, 7.0e6 + 5000.0, 7.2e6 + 5000.0));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(moved.read(start, start + 1000, rows, times));
    CHECK(times.empty());
    moved.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_from_before_frequencies_were_kept_is_continued) {
    const std::string path = temp_path("span-legacy");
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
    const int64_t start = 1'700'000'000'000;
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error));  // no frequencies, as before
        const auto line = line_with_carrier(32, 7);
        archive.append(line.data(), line.size(), start);
        archive.close();
    }
    WaterfallArchive again;
    std::string error;
    CHECK(again.open(path, 32, 1.0, 1, error, 7.0e6, 7.2e6));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(again.read(start, start + 1000, rows, times));
    CHECK_EQ(times.size(), 1);
    again.close();
    // And from now on it knows where it is.
    std::FILE* span = std::fopen((path + ".span").c_str(), "rb");
    CHECK(span != nullptr);
    if (span) std::fclose(span);
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_notices_small_corrections_that_add_up) {
    const std::string path = temp_path("span-drift");
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
    const int64_t start = 1'700'000'000'000;
    // Cells of 6.25 kHz: a quarter of one is 1562.5 Hz.
    {
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, 32, 1.0, 1, error, 7.0e6, 7.2e6));
        const auto line = line_with_carrier(32, 7);
        archive.append(line.data(), line.size(), start);
        archive.close();
    }
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    std::string error;
    {
        WaterfallArchive once;
        CHECK(once.open(path, 32, 1.0, 1, error, 7.0e6 + 1000.0, 7.2e6 + 1000.0));
        CHECK(once.read(start, start + 1000, rows, times));
        CHECK_EQ(times.size(), 1);
        once.close();
    }
    // Another 1000 Hz: each step alone is too small to matter, the two together are not.
    WaterfallArchive twice;
    CHECK(twice.open(path, 32, 1.0, 1, error, 7.0e6 + 2000.0, 7.2e6 + 2000.0));
    CHECK(twice.read(start, start + 1000, rows, times));
    CHECK(times.empty());
    twice.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}

TEST_CASE(archive_reads_only_what_a_picture_can_show) {
    const std::string path = temp_path("picture");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 64, 1.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    for (int i = 0; i < 100; i++) {
        // One carrier, always in archive bin 5.
        const auto line = line_with_carrier(256, 20);
        archive.append(line.data(), line.size(), start + i * 1000);
    }
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    double row_ms = 0;
    size_t bins = 0;
    // Ten rows for a picture ten pixels tall, sixteen bins for one sixteen
    // wide: groups of four, each the loudest of its four.
    CHECK(archive.read(start, start + 99000, rows, times, &row_ms, 10, 4, &bins));
    CHECK_EQ(bins, 16u);
    CHECK(times.size() <= 10u);
    CHECK(times.size() >= 9u);
    CHECK_EQ(rows.size(), times.size() * 16);
    CHECK_EQ(row_ms, 10000.0);
    for (size_t i = 0; i < times.size(); i++) {
        const uint8_t* row = rows.data() + i * 16;
        // Bin 5 lies in group 1, and a narrower picture keeps it.
        CHECK(row[1] > row[0]);
        CHECK(row[1] > row[3]);
    }
    // Without the asks, the archive's own shape.
    CHECK(archive.read(start, start + 99000, rows, times, &row_ms, 0, 1, &bins));
    CHECK_EQ(bins, 64u);
    CHECK_EQ(times.size(), 100u);
    archive.close();
    std::remove(path.c_str());
}

// A shape refused by open() leaves the archive in use open and recording.
TEST_CASE(archive_refused_a_new_shape_keeps_recording) {
    const std::string path = temp_path("refused");
    std::remove(path.c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 64, 1.0, 1, error));
    CHECK(!archive.open(path, 0, 1.0, 1, error));
    CHECK(!archive.open(path, 64, 0.0, 1, error));
    CHECK(!archive.open(path, 64, 1.0, 0, error));
    CHECK(archive.is_open());
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(256, 8);
    archive.append(line.data(), line.size(), start);
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start, rows, times));
    CHECK_EQ(times.size(), 1);
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_torn_payload_reads_as_a_gap) {
    const std::string path = temp_path("torn-payload");
    std::remove(path.c_str());
    std::string error;
    WaterfallArchive archive;
    CHECK(archive.open(path, 16, 600.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(16, 3);
    for (int i = 0; i < 3; ++i) archive.append(line.data(), line.size(), start + i * 600000);
    archive.close();
    auto bytes = file_bytes(path);
    bytes[rows_offset(bytes) + 24 + 8 + 3] ^= 0x80;
    replace_bytes(path, bytes);
    CHECK(archive.open(path, 16, 600.0, 1, error));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 1200000, rows, times));
    CHECK(times == std::vector<int64_t>({start, start + 1200000}));
    CHECK_EQ(rows.size(), 32);
    if (rows.size() == 32) CHECK_EQ(rows[3], rows[19]);
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_torn_ring_overwrite_reads_as_a_gap) {
    const std::string path = temp_path("torn-overwrite");
    std::remove(path.c_str());
    std::string error;
    WaterfallArchive archive;
    CHECK(archive.open(path, 16, 600.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    const auto old_line = line_with_carrier(16, 12);
    for (int i = 0; i < 6; ++i) archive.append(old_line.data(), old_line.size(), start + i * 600000);
    const auto before = file_bytes(path);
    const auto new_line = line_with_carrier(16, 2, -20.0f);
    archive.append(new_line.data(), new_line.size(), start + 3600000);
    auto torn = file_bytes(path);
    archive.close();
    const size_t offset = rows_offset(torn);
    // The new slot prefix persisted, but the last half of its payload did not.
    std::copy(before.begin() + offset + 16, before.begin() + offset + 24, torn.begin() + offset + 16);
    replace_bytes(path, torn);
    CHECK(archive.open(path, 16, 600.0, 1, error));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start + 3600000, rows, times));
    CHECK_EQ(times.size(), 5);
    CHECK(std::find(times.begin(), times.end(), start + 3600000) == times.end());
    CHECK_EQ(rows.size(), 80);
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_torn_headers_keep_the_usable_record) {
    const std::string path = temp_path("torn-header");
    const int64_t start = 1'700'000'000'000;
    // Magic, shape, cursor, epoch and checksum can all tear independently.
    for (const size_t field : {size_t{0}, size_t{8}, size_t{16}, size_t{24}, size_t{32}, size_t{40}, size_t{12}}) {
        std::remove(path.c_str());
        std::string error;
        WaterfallArchive archive;
        CHECK(archive.open(path, 16, 600.0, 1, error));
        const auto line = line_with_carrier(16, 3);
        for (int i = 0; i < 3; ++i) archive.append(line.data(), line.size(), start + i * 600000);
        const auto before = file_bytes(path);
        archive.close();
        const bool version2 = rows_offset(before) == 8192;
        for (const size_t copy : {size_t{0}, size_t{4096}}) {
            if (!version2 && copy != 0) continue;
            auto torn = before;
            torn[copy + field] ^= 0x40;
            replace_bytes(path, torn);
            CHECK(archive.open(path, 16, 600.0, 1, error));
            std::vector<uint8_t> rows;
            std::vector<int64_t> times;
            CHECK(archive.read(start, start + 1200000, rows, times));
            CHECK(times.size() >= 2 && times.size() <= 3);
            if (times.size() >= 2) {
                CHECK_EQ(times[0], start);
                CHECK_EQ(times[1], start + 600000);
                CHECK_EQ(rows[3], WaterfallArchive::quantise(-30.0f));
            }
            archive.append(line.data(), line.size(), start + 3000000);
            archive.close();
            CHECK(archive.open(path, 16, 600.0, 1, error));
            CHECK(archive.read(start, start + 3000000, rows, times));
            CHECK(!times.empty() && times.back() == start + 3000000);
            archive.close();
        }
    }
    std::remove(path.c_str());
}

TEST_CASE(archive_unwritten_first_slot_reads_as_a_gap) {
    const std::string path = temp_path("unwritten-zero");
    std::remove(path.c_str());
    std::string error;
    WaterfallArchive archive;
    CHECK(archive.open(path, 16, 600.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(16, 3);
    archive.append(line.data(), line.size(), start);
    archive.close();
    auto bytes = file_bytes(path);
    const size_t offset = rows_offset(bytes);
    // A header reached storage before the first row, which is still a hole.
    std::fill(bytes.begin() + offset, bytes.begin() + offset + 24, 0);
    replace_bytes(path, bytes);
    CHECK(archive.open(path, 16, 600.0, 1, error));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start, rows, times));
    CHECK(times.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_truncated_file_is_recreated_cleanly) {
    const std::string path = temp_path("truncated");
    std::remove(path.c_str());
    std::string error;
    WaterfallArchive archive;
    CHECK(archive.open(path, 16, 600.0, 1, error));
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(16, 3);
    archive.append(line.data(), line.size(), start);
    archive.close();
    auto bytes = file_bytes(path);
    bytes.resize(bytes.size() - 1);
    replace_bytes(path, bytes);
    CHECK(archive.open(path, 16, 600.0, 1, error));
    CHECK_EQ(file_bytes(path).size(), archive.size_bytes());
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start, rows, times));
    CHECK(times.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_version1_is_recreated_with_a_log) {
    const std::string path = temp_path("version1");
    const int64_t start = 1'700'000'000'000;
    DiskHeader header{};
    std::memcpy(header.magic, "FRNWFA1", 8);
    header.bins = 16;
    header.seconds_per_line = 600.0;
    header.capacity = 6;
    header.next_index = 1;
    header.epoch_ms = start;
    std::vector<uint8_t> bytes(sizeof(header) + 6 * 24, 0);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::fill(bytes.begin() + sizeof(header) + 8, bytes.begin() + sizeof(header) + 24, 200);
    replace_bytes(path, bytes);
    const int previous_level = fernsdr::log_level().load();
    fernsdr::set_log_level(fernsdr::LogLevel::Info);
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 16, 600.0, 1, error));
    fernsdr::log_level().store(previous_level);
    const auto log = fernsdr::LogRing::instance().snapshot();
    CHECK(std::any_of(log.begin(), log.end(), [&](const std::string& entry) {
        return entry.find(path) != std::string::npos && entry.find("version 1") != std::string::npos;
    }));
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(archive.read(start, start, rows, times));
    CHECK(times.empty());
    archive.close();
    std::remove(path.c_str());
}

TEST_CASE(archive_checksum_matches_short_and_long_known_rows) {
    const std::string path = temp_path("checksum-vector");
    const int64_t start = 1'700'000'000'000;
    for (const size_t width : {size_t{8}, size_t{64}}) {
        std::remove(path.c_str());
        WaterfallArchive archive;
        std::string error;
        CHECK(archive.open(path, width, 600.0, 1, error));
        std::vector<float> line(width);
        for (size_t i = 0; i < width; ++i) {
            const uint8_t value = width == 8 ? static_cast<uint8_t>('1' + i) : static_cast<uint8_t>(i);
            line[i] = WaterfallArchive::dequantise(value);
        }
        archive.append(line.data(), line.size(), start);
        archive.close();
        const auto bytes = file_bytes(path);
        uint64_t stored = 0;
        std::memcpy(&stored, bytes.data() + rows_offset(bytes), sizeof(stored));
        // XXH64(index zero followed by payload, seeded with the row's epoch).
        CHECK(stored == (width == 8 ? 0xfda11a560c86e5b9ull : 0x6e514d1837588d53ull));
    }
    std::remove(path.c_str());
}

TEST_CASE(archive_snapshot_rejects_rows_from_a_recreated_epoch) {
    const std::string path = temp_path("snapshot-epoch");
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
    WaterfallArchive archive;
    std::string error;
    CHECK(archive.open(path, 16, 600.0, 1, error, 100.0, 200.0));
    const int64_t start = 1'700'000'000'000;
    const auto line = line_with_carrier(16, 3);
    archive.append(line.data(), line.size(), start);
    WaterfallArchive::Reading snapshot;
    CHECK(archive.begin_read(snapshot));
    CHECK(archive.open(path, 16, 600.0, 1, error, 500.0, 600.0));
    archive.append(line.data(), line.size(), start + 600000);
    std::vector<uint8_t> rows;
    std::vector<int64_t> times;
    CHECK(WaterfallArchive::read(snapshot, start, start, rows, times));
    CHECK(times.empty());
    archive.close();
    std::remove(path.c_str());
    std::remove((path + ".span").c_str());
}
