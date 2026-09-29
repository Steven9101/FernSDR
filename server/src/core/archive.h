// A rolling record of what a band has looked like.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fernsdr {

/**
 * The last N hours of one band's waterfall, on disk.
 *
 * A ring of fixed-size slots in a single file, sized when it is created and
 * never grown. That is the whole design, and it is chosen for one reason: an
 * archive that can grow is an archive that eventually fills the disk of a
 * machine nobody is watching, and takes the receiver down with it. Here the
 * cost is decided once, stated in bytes before it is switched on, and cannot
 * change afterwards without deleting the file.
 *
 * Lines are stored far coarser than they are drawn. A band produces 25 lines a
 * second across 65 536 bins; keeping that for a day would be 140 GB. What
 * anyone actually wants from an archive is "was there anything on this
 * frequency last night", which one line a second at a thousand bins answers
 * perfectly well, at about 90 MB a day.
 *
 * Each slot carries the absolute index of the line in it, so a ring left torn
 * by a power cut describes itself: a slot whose index does not match where it
 * sits is simply not there yet. There is no separate consistency to maintain
 * and nothing to repair on startup.
 */
class WaterfallArchive {
public:
    /** Levels are quantised across this range, which is the whole useful span. */
    static constexpr float kFloorDb = -140.0f;
    static constexpr float kCeilingDb = -20.0f;

    ~WaterfallArchive();

    /**
     * Opens or creates the archive.
     *
     * An existing file whose shape does not match what is asked for is
     * replaced rather than reinterpreted: the retention or the width has been
     * changed in the configuration, and silently reading old lines at the
     * wrong width would draw nonsense.
     */
    /**
     * `low_hz` and `high_hz` are the frequencies a row covers, when known.
     * They are kept beside the file, and an archive recorded for frequencies
     * more than a quarter of a row's cell away is started again rather than
     * drawn at the wrong place: after the band was re-tuned, or its axis
     * corrected by more than the archive can resolve.
     */
    bool open(const std::string& path, size_t bins, double seconds_per_line, int retention_hours,
              std::string& error, double low_hz = 0.0, double high_hz = 0.0);

    void close();
    bool is_open() const { return file_ != nullptr; }

    /**
     * Records one line, if enough time has passed since the last.
     *
     * `now_ms` is wall-clock, because an archive is read by time of day and
     * has to survive a restart; a monotonic clock cannot say when yesterday
     * evening was. `bins` are levels in dB at any width, resampled here.
     */
    void append(const float* bins, size_t count, int64_t now_ms);

    /**
     * Reads the lines covering a time range, oldest first.
     *
     * Returns one row of `bins()` bytes per line found, and writes the
     * timestamp of each into `times_ms`. Slots that were never written, or
     * that belong to an older lap of the ring, are skipped rather than
     * returned as zeroes, so a gap in the record reads as a gap.
     */
    //
    // `max_rows` asks for no more rows than a picture can show (0 for the
    // usual limit); `bin_group` merges that many neighbouring bins into one
    // by their maximum, for a picture narrower than the archive, so a
    // carrier one bin wide still shows. `out_bins` says how many came back.
    bool read(int64_t from_ms, int64_t to_ms, std::vector<uint8_t>& rows,
              std::vector<int64_t>& times_ms, double* row_ms = nullptr,
              size_t max_rows = 0, size_t bin_group = 1, size_t* out_bins = nullptr) const;

    /**
     * What a read needs, taken while the band's lock is held, so that the
     * reading itself, thousands of seeks on a slow card perhaps, holds nothing
     * the band's own thread waits for. It carries a descriptor of its own for
     * the file: the archive may be closed or reopened meanwhile, and a slot
     * written since reads as the gap it was, by its index.
     */
    struct Reading {
        int fd = -1;
        size_t bins = 0;
        double seconds_per_line = 1.0;
        uint64_t capacity = 0;
        uint64_t next_index = 0;
        int64_t epoch_ms = 0;
        Reading() = default;
        Reading(const Reading&) = delete;
        Reading& operator=(const Reading&) = delete;
        ~Reading();
    };
    bool begin_read(Reading& out) const;
    static bool read(const Reading& from, int64_t from_ms, int64_t to_ms, std::vector<uint8_t>& rows,
                     std::vector<int64_t>& times_ms, double* row_ms = nullptr, size_t max_rows = 0,
                     size_t bin_group = 1, size_t* out_bins = nullptr);

    size_t bins() const { return bins_; }
    double seconds_per_line() const { return seconds_per_line_; }
    size_t capacity() const { return capacity_; }
    /** Bytes the file occupies. Stated before the archive is switched on. */
    uint64_t size_bytes() const;
    /** Wall-clock time of the oldest and newest line held, or 0 when empty. */
    int64_t oldest_ms() const;
    int64_t newest_ms() const;

    /** Quantises a level to a byte. Exposed so the client can invert it. */
    static uint8_t quantise(float db);
    static float dequantise(uint8_t value);

private:
    struct Header {
        char magic[8];      // "FRNWFA1\0"
        uint32_t bins;
        uint32_t reserved;
        double seconds_per_line;
        uint64_t capacity;
        uint64_t next_index;  // absolute index of the next line to be written
        int64_t epoch_ms;     // wall-clock time of line 0
    };

    bool read_header(Header& header) const;
    bool write_header() const;
    size_t slot_bytes() const { return sizeof(uint64_t) + bins_; }
    int64_t time_of(uint64_t index) const;

    std::FILE* file_ = nullptr;
    std::string path_;
    size_t bins_ = 0;
    double seconds_per_line_ = 1.0;
    uint64_t capacity_ = 0;
    uint64_t next_index_ = 0;
    int64_t epoch_ms_ = 0;
    std::vector<uint8_t> scratch_;
};

}  // namespace fernsdr
