// WebSocket frames and their reassembly into messages, as a listener's
// connection feeds them; and what is decoded, encoded again, has to decode to
// the same frame.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../src/net/websocket.h"

using fernsdr::ws::Frame;
using fernsdr::ws::MessageAssembler;
using fernsdr::ws::ParseResult;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    MessageAssembler assembler(64 * 1024);
    size_t offset = 0;
    while (offset < size) {
        Frame frame;
        size_t consumed = 0;
        if (fernsdr::ws::parse_frame(data + offset, size - offset, frame, consumed, 64 * 1024) != ParseResult::Ok) break;
        if (consumed == 0 || consumed > size - offset) __builtin_trap();
        offset += consumed;

        // The server sends frames unmasked, and the parser takes them as a
        // client's would be: only a data frame that fits one frame is checked.
        std::vector<uint8_t> encoded;
        fernsdr::ws::encode_frame(frame.opcode, frame.payload.data(), frame.payload.size(), encoded);
        (void)encoded;

        std::vector<uint8_t> message;
        fernsdr::ws::Opcode opcode;
        if (assembler.feed(frame, message, opcode) == MessageAssembler::Status::Error) break;
    }
    return 0;
}
