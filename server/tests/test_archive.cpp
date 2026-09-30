#include "../src/core/archive.h"
#include "test_util.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using fernsdr::WaterfallArchive;

namespace {

std::string temp_path(const char* name) { return std::string("/tmp/fernsdr-test-") + name + ".wfa"; }

/** A line with a carrier at `bin`, so it can be recognised on the way out. */
std::vector<float> line_with_carrier(size_t count, size_t bin, float level = -30.0f) {
    std::vector<float> line(count, -100.0f);
    if (bin < count) line[bin] = level;
    return line;
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
            CHECK(std::memcmp(magic, "FRNWFA1", 8) == 0);
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
