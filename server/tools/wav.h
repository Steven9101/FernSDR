// Minimal 16-bit PCM WAV reading and writing for the measurement tools.
//
// Deliberately narrow: 16-bit integer PCM, any channel count, chunk walking
// that tolerates LIST and other chunks before the data. Some recorders write
// a RIFF length larger than the file; the data chunk is clamped to what is
// actually there rather than rejected.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wav {

struct Audio {
    int rate = 0;
    int channels = 1;
    // Interleaved, normalised to [-1, 1).
    std::vector<float> samples;
    size_t frames() const { return channels > 0 ? samples.size() / channels : 0; }
};

inline uint32_t read_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t read_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline bool load(const std::string& path, Audio& out, std::string& error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + got);
    std::fclose(file);

    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = path + " is not a WAV file";
        return false;
    }
    size_t position = 12;
    int bits = 0;
    bool have_format = false;
    while (position + 8 <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + position);
        const uint32_t size = read_u32(bytes.data() + position + 4);
        const size_t body = position + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= bytes.size()) {
            out.channels = read_u16(bytes.data() + body + 2);
            out.rate = static_cast<int>(read_u32(bytes.data() + body + 4));
            bits = read_u16(bytes.data() + body + 14);
            have_format = true;
        } else if (std::memcmp(id, "data", 4) == 0 && have_format) {
            if (bits != 16 || out.channels < 1) {
                error = path + ": only 16-bit PCM is supported";
                return false;
            }
            // IQ recordings run to tens of megahertz; below a kilohertz the
            // header is broken, and zero would divide the tools by zero.
            if (out.rate < 1000 || out.rate > 200000000) {
                error = path + ": a sample rate of " + std::to_string(out.rate) + " Hz is not usable";
                return false;
            }
            const size_t available = std::min<size_t>(size, bytes.size() - body) / 2;
            const size_t count = available - available % static_cast<size_t>(out.channels);
            out.samples.resize(count);
            for (size_t i = 0; i < count; i++) {
                out.samples[i] = static_cast<int16_t>(read_u16(bytes.data() + body + i * 2)) / 32768.0f;
            }
            return true;
        }
        position = body + size + (size & 1);
    }
    error = path + " has no data chunk";
    return false;
}

inline bool save_mono(const std::string& path, int rate, const std::vector<float>& samples, std::string& error) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = "cannot write " + path;
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    auto put_u32 = [&](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                              static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        std::fwrite(b, 1, 4, file);
    };
    auto put_u16 = [&](uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        std::fwrite(b, 1, 2, file);
    };
    std::fwrite("RIFF", 1, 4, file);
    put_u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    put_u32(16);
    put_u16(1);
    put_u16(1);
    put_u32(static_cast<uint32_t>(rate));
    put_u32(static_cast<uint32_t>(rate) * 2);
    put_u16(2);
    put_u16(16);
    std::fwrite("data", 1, 4, file);
    put_u32(data_bytes);
    for (float sample : samples) {
        long value = std::lround(sample * 32767.0f);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        put_u16(static_cast<uint16_t>(static_cast<int16_t>(value)));
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) error = "short write to " + path;
    return ok;
}

}  // namespace wav
