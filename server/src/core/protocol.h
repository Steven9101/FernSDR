// Wire format for the binary streams.  The JSON control channel is described
// in docs/PROTOCOL.md; this header is the authority for the byte layouts,
// mirrored by web/src/net/protocol.ts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace fernsdr {
namespace proto {

// First byte of every binary message.
enum : uint8_t {
    kStreamAudio = 0x01,
    kStreamWaterfall = 0x02,
    kStreamMeter = 0x03,
};

// Audio: [type u8][flags u8][sequence u16 LE][NAC payload]
//
// flags: bit 0   - muted (squelch closed); payload is still a valid frame
//        bit 1   - compact NAC side information, negotiated as nac2
//        bit 2   - resume after discarded audio, negotiated as audio-discontinuity
//        bit 3   - NAC3 packet of one to four frames, negotiated as nac3
//        bits 4-7 - configuration generation
//
// The sequence counts frames. A NAC3 packet carries the sequence of its first
// frame, and the next packet's is that plus its frame count.
//
// The generation is what makes the stream self-describing without spending
// four bytes per frame on a sample rate: the server announces
// {generation -> sample rate, bitrate} on the control channel, and the client
// only has to notice when the nibble changes.  At ~94 frames/second a
// per-frame rate field would cost 3 kbit/s for information that changes once
// an hour.
constexpr size_t kAudioHeaderBytes = 4;
constexpr uint8_t kAudioFlagMuted = 0x01;
constexpr uint8_t kAudioFlagCompact = 0x02;
constexpr uint8_t kAudioFlagDiscontinuity = 0x04;
constexpr uint8_t kAudioFlagPacket = 0x08;

inline uint8_t audio_flags(bool muted, uint8_t generation, bool compact = false, bool packet = false) {
    return static_cast<uint8_t>((muted ? kAudioFlagMuted : 0) | (compact ? kAudioFlagCompact : 0) |
                                (packet ? kAudioFlagPacket : 0) | ((generation & 0x0F) << 4));
}

// Frames in an audio message: a NAC3 packet names its count in the top two
// bits of its first payload byte; every other layout carries one frame.
inline int audio_frames(const uint8_t* message, size_t size) {
    if (size <= kAudioHeaderBytes || !(message[1] & kAudioFlagPacket)) return 1;
    return (message[kAudioHeaderBytes] >> 6) + 1;
}

// Waterfall: [type u8][flags u8][sequence u16 LE][low Hz f64 LE][high Hz f64 LE]
//            [width u16 LE][compressed line]
// flags: bit 0 zero runs, bit 1 extended predictor, bit 2 native grid,
//        bit 3 uses 2 dB quantizer units (otherwise 1 dB).
//
// The frequency span travels with every line rather than being implied by the
// last viewport command.  A line in flight when the user pans would otherwise
// be drawn at the wrong place, which shows up as the waterfall tearing
// sideways during a drag.
constexpr size_t kWaterfallHeaderBytes = 22;

// Little-endian writers; the wire format is fixed-endian so that a
// big-endian server would still interoperate.
inline void write_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

inline void write_u32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; i++) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

// Negotiated as meter-v1. Levels retain the JSON meter's 0.1 dB resolution;
// the flags distinguish an absent SAM/auto-squelch reading from a zero one.
constexpr size_t kMeterBytes = 26;
struct Meter {
    float dbfs = 0;
    float gain_db = 0;
    float squelch_statistic = 0;
    float pll_offset = 0;
    float waterfall_fps = 0;
    uint32_t audio_bps = 0;
    uint32_t waterfall_bps = 0;
    uint32_t listeners = 0;
    bool squelch_open = false;
    bool pll_locked = false;
    bool auto_squelch = false;
    bool sam = false;
    bool nfm = false;
    // Hz, in NFM only; 0 for no tone.
    float ctcss_hz = 0;
};

inline void write_meter(uint8_t* out, const Meter& meter) {
    auto tenths = [](double value, double low, double high) -> int64_t {
        return std::isfinite(value) ? static_cast<int64_t>(std::round(std::clamp(value * 10, low, high))) : 0;
    };
    out[0] = kStreamMeter;
    out[1] = (meter.squelch_open ? 1 : 0) | (meter.pll_locked ? 2 : 0) |
             (meter.auto_squelch ? 4 : 0) | (meter.sam ? 8 : 0) | (meter.nfm ? 16 : 0);
    write_u16(out + 2, static_cast<uint16_t>(tenths(meter.dbfs, -32768, 32767)));
    write_u16(out + 4, static_cast<uint16_t>(tenths(meter.gain_db, -32768, 32767)));
    write_u16(out + 6, static_cast<uint16_t>(tenths(meter.squelch_statistic, 0, 65535)));
    // The carrier's offset in SAM; in NFM the same four bytes carry the
    // CTCSS tone (0 for none), since the two modes never share a reading.
    // Only for a page that said "meter-ctcss": an older one refuses a reading
    // with an unknown flag.
    const double shared_field = meter.nfm ? meter.ctcss_hz : meter.pll_offset;
    write_u32(out + 8, static_cast<uint32_t>(tenths(shared_field, -2147483648.0, 2147483647.0)));
    write_u32(out + 12, meter.audio_bps);
    write_u32(out + 16, meter.waterfall_bps);
    write_u16(out + 20, static_cast<uint16_t>(tenths(meter.waterfall_fps, 0, 65535)));
    write_u32(out + 22, meter.listeners);
}

inline void write_f64(uint8_t* out, double value) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
    __builtin_memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(bits >> (8 * i));
}

}  // namespace proto
}  // namespace fernsdr
