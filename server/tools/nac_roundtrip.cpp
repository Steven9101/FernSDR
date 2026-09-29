// Puts a WAV file through the NAC codec and writes what comes out.
//
// The point is to make the project's central claim falsifiable by somebody
// else. FernSDR exists on the premise that a listener can decode digital modes
// out of the audio stream, which means the codec must not distort time: no
// stretching to conceal loss, no resampling, a lost frame occupying exactly one
// frame. That is easy to assert and easy to get wrong.
//
// With this, anyone can take a real off-air recording, run it through, and hand
// both files to a decoder that knows nothing about FernSDR:
//
//     nac-roundtrip in.wav out.wav --bitrate 48000
//     decode_ft8 in.wav ; decode_ft8 out.wav
//
// If the decoder finds the same messages in both, the claim holds for that
// recording. If it does not, it does not.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/codec/nac.h"

namespace {

struct Wav {
    int rate = 0;
    int channels = 1;
    std::vector<float> samples;
};

uint32_t read_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool load_wav(const char* path, Wav& out, std::string& error) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + got);
    fclose(file);

    if (bytes.size() < 44 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "not a WAV file";
        return false;
    }

    size_t position = 12;
    int bits = 0;
    bool have_format = false;
    while (position + 8 <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + position);
        const uint32_t size = read_u32(bytes.data() + position + 4);
        const size_t body = position + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= bytes.size()) {
            out.channels = read_u16(bytes.data() + body + 2);
            out.rate = static_cast<int>(read_u32(bytes.data() + body + 4));
            bits = read_u16(bytes.data() + body + 14);
            have_format = true;
        } else if (memcmp(id, "data", 4) == 0 && have_format) {
            if (bits != 16) {
                error = "only 16-bit PCM is supported";
                return false;
            }
            const size_t count = std::min<size_t>(size, bytes.size() - body) / 2;
            out.samples.reserve(count / out.channels);
            for (size_t i = 0; i < count; i += out.channels) {
                const int16_t value = static_cast<int16_t>(read_u16(bytes.data() + body + i * 2));
                out.samples.push_back(value / 32768.0f);
            }
            return true;
        }
        position = body + size + (size & 1);
    }
    error = "no data chunk";
    return false;
}

bool save_wav(const char* path, const Wav& wav, std::string& error) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        error = std::string("cannot write ") + path;
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(wav.samples.size() * 2);
    const uint32_t rate = static_cast<uint32_t>(wav.rate);
    auto put_u32 = [&](uint32_t v) { fputc(v & 0xFF, file); fputc((v >> 8) & 0xFF, file);
                                     fputc((v >> 16) & 0xFF, file); fputc((v >> 24) & 0xFF, file); };
    auto put_u16 = [&](uint16_t v) { fputc(v & 0xFF, file); fputc((v >> 8) & 0xFF, file); };

    fwrite("RIFF", 1, 4, file);
    put_u32(36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, file);
    put_u32(16);
    put_u16(1);
    put_u16(1);
    put_u32(rate);
    put_u32(rate * 2);
    put_u16(2);
    put_u16(16);
    fwrite("data", 1, 4, file);
    put_u32(data_bytes);
    for (float sample : wav.samples) {
        int value = static_cast<int>(sample * 32767.0f);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        put_u16(static_cast<uint16_t>(static_cast<int16_t>(value)));
    }
    fclose(file);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: nac-roundtrip <in.wav> <out.wav> [--bitrate BPS] [--drop N]\n"
                "\n"
                "  --bitrate  target bitrate, default 48000\n"
                "  --drop     discard every Nth frame, to model packet loss\n");
        return 2;
    }

    int bitrate = 48000;
    int drop = 0;
    for (int i = 3; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "--bitrate" && i + 1 < argc) bitrate = atoi(argv[++i]);
        else if (argument == "--drop" && i + 1 < argc) drop = atoi(argv[++i]);
    }

    Wav input;
    std::string error;
    if (!load_wav(argv[1], input, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    fernsdr::nac::Encoder encoder(input.rate);
    encoder.set_bitrate(bitrate);
    fernsdr::nac::Decoder decoder(input.rate);

    Wav output;
    output.rate = input.rate;
    output.samples.reserve(input.samples.size());
    std::vector<float> frame(fernsdr::nac::kFrameHop);

    size_t coded_bytes = 0;
    size_t frames = 0, dropped = 0;
    for (size_t offset = 0; offset + fernsdr::nac::kFrameHop <= input.samples.size();
         offset += fernsdr::nac::kFrameHop) {
        const auto& payload = encoder.encode(input.samples.data() + offset);
        coded_bytes += payload.size();
        frames++;
        if (drop > 0 && frames % static_cast<size_t>(drop) == 0) {
            // Concealment has to occupy exactly one frame, or every symbol
            // after it is late and the decoder loses the timeline.
            decoder.conceal(frame.data());
            dropped++;
        } else {
            decoder.decode(payload.data(), payload.size(), frame.data());
        }
        output.samples.insert(output.samples.end(), frame.begin(), frame.end());
    }

    if (!save_wav(argv[2], output, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    printf("in       %s: %zu samples at %d Hz (%.2f s)\n", argv[1], input.samples.size(), input.rate,
           static_cast<double>(input.samples.size()) / input.rate);
    printf("out      %s: %zu samples (%.2f s)\n", argv[2], output.samples.size(),
           static_cast<double>(output.samples.size()) / output.rate);
    if (frames) {
        printf("codec    %.1f kbit/s over %zu frames", coded_bytes * 8.0 * input.rate /
               (frames * fernsdr::nac::kFrameHop) / 1000.0, frames);
        if (dropped) printf(", %zu concealed (1 in %d)", dropped, drop);
        printf("\n");
    }
    // The timeline is the whole claim: the same number of samples must come
    // out, whatever happened in between.
    const size_t expected = frames * fernsdr::nac::kFrameHop;
    printf("timeline %s\n", output.samples.size() == expected
           ? "intact: output length is exactly frames x hop"
           : "BROKEN: length does not match the frame count");
    return output.samples.size() == expected ? 0 : 1;
}
