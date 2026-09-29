#include "websocket.h"

#include <cstring>

#include "../util/sha1.h"

namespace fernsdr {
namespace ws {

namespace {
// The fixed GUID from RFC 6455 section 1.3.
constexpr const char* kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}  // namespace

std::string accept_key(const std::string& client_key) {
    const std::string combined = client_key + kMagic;
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(), digest);
    return base64_encode(digest, sizeof(digest));
}

bool valid_client_key(const std::string& key) {
    // The handshake nonce is exactly 16 bytes, represented by 22 base64
    // digits and two padding bytes. The unused low four bits must be zero.
    if (key.size() != 24 || key[22] != '=' || key[23] != '=') return false;
    for (size_t i = 0; i < 22; i++) {
        const char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
    }
    return key[21] == 'A' || key[21] == 'Q' || key[21] == 'g' || key[21] == 'w';
}

void encode_frame(Opcode opcode, const uint8_t* payload, size_t length, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(opcode)));  // FIN set

    if (length < 126) {
        out.push_back(static_cast<uint8_t>(length));
    } else if (length <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(length));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; i--) out.push_back(static_cast<uint8_t>(length >> (8 * i)));
    }

    if (length) out.insert(out.end(), payload, payload + length);
}

void encode_text(const std::string& text, std::vector<uint8_t>& out) {
    encode_frame(Opcode::Text, reinterpret_cast<const uint8_t*>(text.data()), text.size(), out);
}

void encode_binary(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    encode_frame(Opcode::Binary, payload.data(), payload.size(), out);
}

void encode_close(uint16_t code, const std::string& reason, std::vector<uint8_t>& out) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(code >> 8));
    payload.push_back(static_cast<uint8_t>(code));
    payload.insert(payload.end(), reason.begin(), reason.end());
    encode_frame(Opcode::Close, payload.data(), payload.size(), out);
}

ParseResult parse_frame(const uint8_t* data, size_t size, Frame& out, size_t& consumed,
                        size_t max_payload_bytes) {
    if (size < 2) return ParseResult::NeedMore;

    const uint8_t byte0 = data[0];
    const uint8_t byte1 = data[1];

    // Reserved bits must be clear: we negotiate no extensions.
    if (byte0 & 0x70) return ParseResult::Error;

    out.fin = (byte0 & 0x80) != 0;
    const uint8_t raw_opcode = byte0 & 0x0F;
    switch (raw_opcode) {
        case 0x0: case 0x1: case 0x2: case 0x8: case 0x9: case 0xA:
            out.opcode = static_cast<Opcode>(raw_opcode);
            break;
        default:
            return ParseResult::Error;
    }

    const bool masked = (byte1 & 0x80) != 0;
    // Every client-to-server frame must be masked (RFC 6455 section 5.1).
    if (!masked) return ParseResult::Error;

    uint64_t length = byte1 & 0x7F;
    size_t offset = 2;

    if (length == 126) {
        if (size < offset + 2) return ParseResult::NeedMore;
        length = (static_cast<uint64_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (length < 126) return ParseResult::Error;  // must use the shortest form
    } else if (length == 127) {
        if (size < offset + 8) return ParseResult::NeedMore;
        length = 0;
        for (int i = 0; i < 8; i++) length = (length << 8) | data[offset + i];
        offset += 8;
        if (length <= 0xFFFF) return ParseResult::Error;
        if (length >> 63) return ParseResult::Error;  // high bit must be clear
    }

    // Control frames carry at most 125 bytes and are never fragmented.
    const bool is_control = raw_opcode >= 0x8;
    if (is_control && (length > 125 || !out.fin)) return ParseResult::Error;
    // Reject from the header. Waiting for the body would let an unauthenticated
    // peer grow the receive buffer up to the length it chose.
    if (length > max_payload_bytes) return ParseResult::Error;

    if (size < offset + 4) return ParseResult::NeedMore;
    uint8_t mask[4];
    std::memcpy(mask, data + offset, 4);
    offset += 4;

    if (size < offset + length) return ParseResult::NeedMore;

    out.payload.resize(static_cast<size_t>(length));
    for (uint64_t i = 0; i < length; i++) {
        out.payload[static_cast<size_t>(i)] = data[offset + i] ^ mask[i & 3];
    }

    consumed = offset + static_cast<size_t>(length);
    return ParseResult::Ok;
}

MessageAssembler::Status MessageAssembler::feed(const Frame& frame, std::vector<uint8_t>& message,
                                                Opcode& opcode) {
    const bool is_control = static_cast<uint8_t>(frame.opcode) >= 0x8;
    if (is_control) {
        // Control frames may be interleaved into a fragmented message; they
        // are delivered straight through without disturbing the buffer.
        message = frame.payload;
        opcode = frame.opcode;
        return Status::Complete;
    }

    if (frame.opcode == Opcode::Continuation) {
        if (!fragmented_) return Status::Error;  // continuation with nothing to continue
    } else {
        if (fragmented_) return Status::Error;  // new message before the last one finished
        fragment_opcode_ = frame.opcode;
        buffer_.clear();
        fragmented_ = true;
    }

    if (buffer_.size() + frame.payload.size() > limit_) {
        reset();
        return Status::Error;
    }
    buffer_.insert(buffer_.end(), frame.payload.begin(), frame.payload.end());

    if (!frame.fin) return Status::Incomplete;

    message.swap(buffer_);
    opcode = fragment_opcode_;
    reset();
    return Status::Complete;
}

void MessageAssembler::reset() {
    buffer_.clear();
    fragmented_ = false;
}

}  // namespace ws
}  // namespace fernsdr
