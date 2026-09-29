// RFC 6455 framing.  Only what a server needs: the handshake response, frame
// encoding for outbound data, and a parser that tolerates arbitrary TCP
// segmentation.
//
// Written by hand rather than pulled in, for the same reason as everything
// else here: this must build on a machine with a compiler and nothing else.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fernsdr {
namespace ws {

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

// Sec-WebSocket-Accept for a given Sec-WebSocket-Key.
std::string accept_key(const std::string& client_key);
bool valid_client_key(const std::string& client_key);

// Appends a complete unmasked frame to `out`.  Server-to-client frames are
// never masked.
void encode_frame(Opcode opcode, const uint8_t* payload, size_t length, std::vector<uint8_t>& out);
void encode_text(const std::string& text, std::vector<uint8_t>& out);
void encode_binary(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);
void encode_close(uint16_t code, const std::string& reason, std::vector<uint8_t>& out);

struct Frame {
    Opcode opcode = Opcode::Binary;
    bool fin = true;
    std::vector<uint8_t> payload;
};

enum class ParseResult {
    Ok,        // a frame was produced and `consumed` bytes were used
    NeedMore,  // incomplete; keep buffering
    Error,     // protocol violation; the connection must be closed
};

// Parses one frame from the front of a buffer.
ParseResult parse_frame(const uint8_t* data, size_t size, Frame& out, size_t& consumed,
                        size_t max_payload_bytes = 1 << 20);

// Reassembles fragmented messages and enforces a size limit, so a client
// cannot exhaust server memory by opening a fragmented message and never
// finishing it.
class MessageAssembler {
public:
    explicit MessageAssembler(size_t max_message_bytes = 1 << 20) : limit_(max_message_bytes) {}

    enum class Status { Incomplete, Complete, Error };

    // Feeds one parsed frame.  On Complete, `message` holds the whole message
    // and `opcode` its type.  Control frames are returned immediately and
    // never interrupt a fragmented data message.
    Status feed(const Frame& frame, std::vector<uint8_t>& message, Opcode& opcode);

    void reset();

private:
    size_t limit_;
    std::vector<uint8_t> buffer_;
    Opcode fragment_opcode_ = Opcode::Binary;
    bool fragmented_ = false;
};

}  // namespace ws
}  // namespace fernsdr
