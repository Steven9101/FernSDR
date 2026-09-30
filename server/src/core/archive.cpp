#include "archive.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include "../util/log.h"

namespace fernsdr {

namespace {

constexpr char kMagic[8] = {'F', 'R', 'N', 'W', 'F', 'A', '1', '\0'};

// A month of a thousand bins a second is past 2 GB, beyond a 32-bit offset;
// the Makefile asks for 64-bit ones on every system.
static_assert(sizeof(off_t) >= 8, "the archive needs 64-bit file offsets: build with -D_FILE_OFFSET_BITS=64");

}  // namespace

WaterfallArchive::~WaterfallArchive() { close(); }

uint8_t WaterfallArchive::quantise(float db) {
    if (!(db > kFloorDb)) return 0;
    if (db >= kCeilingDb) return 255;
    const float t = (db - kFloorDb) / (kCeilingDb - kFloorDb);
    return static_cast<uint8_t>(t * 255.0f + 0.5f);
}

float WaterfallArchive::dequantise(uint8_t value) {
    return kFloorDb + (kCeilingDb - kFloorDb) * (static_cast<float>(value) / 255.0f);
}

bool WaterfallArchive::read_header(Header& header) const {
    if (!file_) return false;
    if (std::fseek(file_, 0, SEEK_SET) != 0) return false;
    return std::fread(&header, sizeof(header), 1, file_) == 1;
}

bool WaterfallArchive::write_header() const {
    if (!file_) return false;
    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.bins = static_cast<uint32_t>(bins_);
    header.seconds_per_line = seconds_per_line_;
    header.capacity = capacity_;
    header.next_index = next_index_;
    header.epoch_ms = epoch_ms_;
    if (std::fseek(file_, 0, SEEK_SET) != 0) return false;
    if (std::fwrite(&header, sizeof(header), 1, file_) != 1) return false;
    return std::fflush(file_) == 0;
}

namespace {

// The frequencies live in a file of their own because the header has no room
// for them, and changing the header would have thrown away every archive
// recorded before.
std::string span_path(const std::string& path) { return path + ".span"; }

// Whether the rows already in the archive were recorded for these frequencies,
// and in `known` whether the archive says what they were recorded for at all.
// An archive from before spans were kept, or a span nobody stated, is taken as
// matching: there is nothing to compare, and losing a record on an upgrade
// would be worse.
bool span_matches(const std::string& path, double low_hz, double high_hz, size_t bins, bool& known) {
    known = false;
    if (!(high_hz > low_hz)) return true;
    std::FILE* file = std::fopen(span_path(path).c_str(), "rbe");
    if (!file) return true;
    double low = 0.0, high = 0.0;
    const bool read = std::fscanf(file, "%lf %lf", &low, &high) == 2;
    std::fclose(file);
    if (!read) return true;
    known = true;
    const double tolerance = (high_hz - low_hz) / static_cast<double>(bins) / 4.0;
    return std::fabs(low - low_hz) <= tolerance && std::fabs(high - high_hz) <= tolerance;
}

void write_span(const std::string& path, double low_hz, double high_hz) {
    if (!(high_hz > low_hz)) return;
    const std::string target = span_path(path);
    const std::string temporary = target + ".part";
    std::FILE* file = std::fopen(temporary.c_str(), "wbe");
    if (!file) return;
    const bool written = std::fprintf(file, "%.3f %.3f\n", low_hz, high_hz) > 0;
    if (std::fclose(file) != 0 || !written || std::rename(temporary.c_str(), target.c_str()) != 0) {
        std::remove(temporary.c_str());
        LOG_WARN("archive", "cannot record the frequencies of %s", path.c_str());
    }
}

}  // namespace

bool WaterfallArchive::open(const std::string& path, size_t bins, double seconds_per_line,
                            int retention_hours, std::string& error, double low_hz, double high_hz) {
    // Checked before the archive in use is closed: a request refused for its
    // shape leaves recording as it was.
    if (bins == 0 || bins > 8192) {
        error = "the archive width must be between 1 and 8192 bins";
        return false;
    }
    if (!std::isfinite(seconds_per_line) || seconds_per_line < 0.1 || seconds_per_line > 3600.0) {
        error = "the archive interval must be between 0.1 and 3600 seconds";
        return false;
    }
    if (retention_hours <= 0 || retention_hours > 24 * 365) {
        error = "the archive must keep between 1 hour and a year";
        return false;
    }
    close();

    failing_ = false;
    bins_ = bins;
    seconds_per_line_ = seconds_per_line;
    capacity_ = static_cast<uint64_t>(retention_hours * 3600.0 / seconds_per_line);
    if (capacity_ == 0) capacity_ = 1;
    path_ = path;

    // An existing file of the right shape is continued; one of the wrong shape
    // is replaced. Reading old lines at a new width would draw nonsense, and
    // quietly drawing nonsense is worse than losing a record nobody has looked
    // at yet.
    // "e" is close-on-exec, so a module started later does not inherit it.
    file_ = std::fopen(path.c_str(), "r+be");
    if (file_) {
        Header header{};
        const bool shaped = read_header(header) &&
                            std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 &&
                            header.bins == bins_ && header.capacity == capacity_ &&
                            std::fabs(header.seconds_per_line - seconds_per_line_) < 1e-9;
        bool known = false;
        const bool placed = span_matches(path, low_hz, high_hz, bins_, known);
        if (shaped && placed) {
            // The frequencies the rows were first recorded for stay, rather
            // than following each correction: several, each too small to
            // matter, could otherwise carry the axis a whole cell away without
            // the archive ever starting again.
            if (!known) write_span(path, low_hz, high_hz);
            next_index_ = header.next_index;
            epoch_ms_ = header.epoch_ms;
            scratch_.assign(slot_bytes(), 0);
            LOG_INFO("archive", "%s continued: %llu lines held of %llu", path.c_str(),
                     static_cast<unsigned long long>(std::min(next_index_, capacity_)),
                     static_cast<unsigned long long>(capacity_));
            return true;
        }
        std::fclose(file_);
        file_ = nullptr;
        if (!shaped) LOG_INFO("archive", "%s was written with a different shape; starting again", path.c_str());
        else LOG_INFO("archive", "%s was recorded for other frequencies; starting again", path.c_str());
    }

    file_ = std::fopen(path.c_str(), "w+be");
    if (!file_) {
        error = "cannot write " + path;
        return false;
    }
    next_index_ = 0;
    epoch_ms_ = 0;
    if (!write_header()) {
        error = "cannot write the archive header in " + path;
        close();
        return false;
    }
    // The file is created at full size rather than grown: the operator is told
    // what it costs before it is switched on, and being told is worth nothing
    // if the number only becomes true a day later.
    if (::fseeko(file_, static_cast<off_t>(size_bytes() - 1), SEEK_SET) != 0 ||
        std::fputc(0, file_) == EOF || std::fflush(file_) != 0) {
        error = "cannot reserve " + std::to_string(size_bytes()) + " bytes for " + path;
        close();
        return false;
    }
    scratch_.assign(slot_bytes(), 0);
    write_span(path, low_hz, high_hz);
    LOG_INFO("archive", "%s created: %llu lines, %.1f MB", path.c_str(),
             static_cast<unsigned long long>(capacity_),
             static_cast<double>(size_bytes()) / 1e6);
    return true;
}

void WaterfallArchive::close() {
    if (file_) {
        write_header();
        std::fclose(file_);
        file_ = nullptr;
    }
}

uint64_t WaterfallArchive::size_bytes() const {
    return sizeof(Header) + capacity_ * static_cast<uint64_t>(slot_bytes());
}

int64_t WaterfallArchive::time_of(uint64_t index) const {
    return epoch_ms_ + static_cast<int64_t>(static_cast<double>(index) * seconds_per_line_ * 1000.0);
}

int64_t WaterfallArchive::newest_ms() const {
    return next_index_ == 0 ? 0 : time_of(next_index_ - 1);
}

int64_t WaterfallArchive::oldest_ms() const {
    if (next_index_ == 0) return 0;
    const uint64_t first = next_index_ > capacity_ ? next_index_ - capacity_ : 0;
    return time_of(first);
}

void WaterfallArchive::append(const float* bins, size_t count, int64_t now_ms) {
    if (!file_ || count == 0) return;

    if (next_index_ == 0) {
        epoch_ms_ = now_ms;
    } else {
        // The archive is indexed by position, so a line is only taken when the
        // clock says one is due. A band producing 25 lines a second would
        // otherwise fill a day's ring in an hour.
        const int64_t due = time_of(next_index_);
        if (now_ms < due) return;
        // After a suspend or a long stall, jumping the index forward keeps
        // position and time in step: the gap stays a gap rather than being
        // papered over with whatever arrives next.
        const double behind = static_cast<double>(now_ms - due) / (seconds_per_line_ * 1000.0);
        if (behind > 1.0) next_index_ += static_cast<uint64_t>(behind);
    }

    // Resample to the archive's width by taking the strongest bin in each
    // output cell. A waterfall is read for what was there, and a peak is what
    // was there; an average makes a narrow carrier disappear into the noise.
    uint8_t* row = scratch_.data() + sizeof(uint64_t);
    for (size_t i = 0; i < bins_; i++) {
        const size_t start = count * i / bins_;
        const size_t end = std::max(start + 1, count * (i + 1) / bins_);
        float peak = bins[start];
        for (size_t j = start + 1; j < end && j < count; j++) peak = std::max(peak, bins[j]);
        row[i] = quantise(peak);
    }
    std::memcpy(scratch_.data(), &next_index_, sizeof(uint64_t));

    const uint64_t slot = next_index_ % capacity_;
    const off_t offset = static_cast<off_t>(sizeof(Header) + slot * slot_bytes());
    if (::fseeko(file_, offset, SEEK_SET) != 0 || std::fwrite(scratch_.data(), scratch_.size(), 1, file_) != 1) {
        note_written(false);
        return;
    }

    next_index_++;
    // The header is rewritten every line. It is 48 bytes against a kilobyte of
    // payload, and it is what makes the file describe itself after a crash.
    // Its flush is also where a buffered line meets the disk and fails.
    note_written(write_header());
}

void WaterfallArchive::note_written(bool written) {
    if (written != failing_) return;
    failing_ = !written;
    // Once each way: a full disk would otherwise fill the log a line a second.
    if (failing_) {
        LOG_WARN("archive", "%s: a line cannot be written (%s); the record has a gap until one can", path_.c_str(),
                 std::strerror(errno));
    } else {
        LOG_INFO("archive", "%s: lines are written again", path_.c_str());
    }
}

bool WaterfallArchive::read(int64_t from_ms, int64_t to_ms, std::vector<uint8_t>& rows,
                            std::vector<int64_t>& times_ms, double* row_ms, size_t max_rows,
                            size_t bin_group, size_t* out_bins) const {
    Reading reading;
    if (!begin_read(reading)) {
        rows.clear();
        times_ms.clear();
        return false;
    }
    return read(reading, from_ms, to_ms, rows, times_ms, row_ms, max_rows, bin_group, out_bins);
}

WaterfallArchive::Reading::~Reading() {
    if (fd >= 0) ::close(fd);
}

bool WaterfallArchive::begin_read(Reading& out) const {
    if (!file_) return false;
    // Lines still in the stream's buffer are not in the file yet; the next
    // line's seek writes them. Read now, they are the gap they would be.
    out.fd = ::fcntl(::fileno(file_), F_DUPFD_CLOEXEC, 0);
    if (out.fd < 0) return false;
    out.bins = bins_;
    out.seconds_per_line = seconds_per_line_;
    out.capacity = capacity_;
    out.next_index = next_index_;
    out.epoch_ms = epoch_ms_;
    return true;
}

bool WaterfallArchive::read(const Reading& from, int64_t from_ms, int64_t to_ms, std::vector<uint8_t>& rows,
                            std::vector<int64_t>& times_ms, double* row_ms, size_t max_rows,
                            size_t bin_group, size_t* out_bins, uint64_t* read_bytes) {
    const size_t bins = from.bins;
    if (read_bytes) *read_bytes = 0;
    bin_group = std::max<size_t>(1, std::min(bin_group, bins));
    const size_t grouped = (bins + bin_group - 1) / bin_group;
    if (out_bins) *out_bins = grouped;
    rows.clear();
    times_ms.clear();
    if (row_ms) *row_ms = from.seconds_per_line * 1000;
    if (from.fd < 0 || to_ms < from_ms || from.capacity == 0) return false;
    // An archive with nothing in it yet is not a failure, it is an empty
    // answer. Only being unable to read the file at all is a failure.
    const uint64_t next_index = from.next_index;
    if (next_index == 0) return true;
    const auto time_of = [&](uint64_t index) {
        return from.epoch_ms + static_cast<int64_t>(static_cast<double>(index) * from.seconds_per_line * 1000.0);
    };

    const uint64_t first_held = next_index > from.capacity ? next_index - from.capacity : 0;
    // Turn the requested times into indices, then clamp to what is held.
    const double from_index =
        ((static_cast<double>(from_ms) - static_cast<double>(from.epoch_ms)) / 1000.0) / from.seconds_per_line;
    const double to_index =
        ((static_cast<double>(to_ms) - static_cast<double>(from.epoch_ms)) / 1000.0) / from.seconds_per_line;
    if (to_index < 0.0) return true;
    if (from_index >= static_cast<double>(next_index)) return true;

    uint64_t begin = from_index <= 0.0 ? 0 : static_cast<uint64_t>(from_index);
    begin = std::max(begin, first_held);
    const uint64_t end = to_index >= static_cast<double>(next_index - 1)
                             ? next_index
                             : static_cast<uint64_t>(std::max(0.0, std::ceil(to_index))) + 1;
    if (begin >= end) return true;

    // A public request can span the whole archive. Sample that range at a
    // bounded number of original rows so it cannot monopolise the network
    // thread or allocate a year's history. Timestamps still identify the
    // actual rows, and zooming into a shorter range retrieves finer detail.
    uint64_t limit = std::min<uint64_t>(4096, 1024 * 1024 / grouped);
    if (max_rows > 0) limit = std::max<uint64_t>(1, std::min<uint64_t>(limit, max_rows));
    const uint64_t stride = (end - begin - 1) / limit + 1;
    if (row_ms) *row_ms = stride * from.seconds_per_line * 1000;
    const size_t slot_bytes = sizeof(uint64_t) + bins;
    std::vector<uint8_t> slot(slot_bytes);
    for (uint64_t index = begin; index < end; index += stride) {
        const off_t offset = static_cast<off_t>(sizeof(Header) + (index % from.capacity) * slot_bytes);
        if (::pread(from.fd, slot.data(), slot.size(), offset) != static_cast<ssize_t>(slot.size())) return false;
        if (read_bytes) *read_bytes += slot.size();

        uint64_t stored = 0;
        std::memcpy(&stored, slot.data(), sizeof(uint64_t));
        // A slot holding a different index was never written, or belongs to an
        // older lap of the ring. Either way it is not this line, and a gap in
        // the record should read as a gap rather than as a row of zeroes.
        if (stored != index) continue;

        const uint8_t* level = slot.data() + sizeof(uint64_t);
        if (bin_group == 1) {
            rows.insert(rows.end(), level, level + bins);
        } else {
            for (size_t group = 0; group < grouped; group++) {
                const size_t first = group * bin_group;
                rows.push_back(*std::max_element(level + first, level + std::min(bins, first + bin_group)));
            }
        }
        times_ms.push_back(time_of(index));
    }
    return true;
}

}  // namespace fernsdr
