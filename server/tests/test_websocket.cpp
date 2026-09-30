#include "../src/net/websocket.h"
#include "../src/util/sha1.h"
#include "test_util.h"

TEST_CASE(websocket_handshake_nonce_is_a_canonical_16_byte_base64_value) {
    CHECK(fernsdr::ws::valid_client_key("dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK(fernsdr::ws::valid_client_key("AAAAAAAAAAAAAAAAAAAAAA=="));
    for (const char* key : {"", "abc", "dGhlIHNhbXBsZSBub25jZR==", "dGhlIHNhbXBsZSBub25jZQ=", "dGhlIHNhbXBsZSBub25jZ===", "dGhlIHNhbXBsZSBub25j Q=="}) {
        CHECK(!fernsdr::ws::valid_client_key(key));
    }
}

#include <cstring>
#include <random>
#include <string>

using namespace fernsdr::ws;

TEST_CASE(websocket_rejects_oversized_payloads_from_the_header_alone) {
    const uint8_t header[] = {0x81, 0xff, 0, 0, 0, 0, 1, 0, 0, 0};
    Frame frame;
    size_t consumed = 0;
    CHECK(parse_frame(header, sizeof(header), frame, consumed, 64 * 1024) == ParseResult::Error);
    CHECK(frame.payload.empty());
}

namespace {

// Builds a client-style masked frame, which is what the server must accept.
std::vector<uint8_t> masked_frame(Opcode opcode, const std::string& payload, bool fin = true,
                                  uint32_t mask_key = 0x37FA213D) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));

    const size_t length = payload.size();
    if (length < 126) {
        out.push_back(static_cast<uint8_t>(0x80 | length));
    } else if (length <= 0xFFFF) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(length));
    } else {
        out.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) out.push_back(static_cast<uint8_t>(length >> (8 * i)));
    }

    uint8_t mask[4] = {static_cast<uint8_t>(mask_key >> 24), static_cast<uint8_t>(mask_key >> 16),
                       static_cast<uint8_t>(mask_key >> 8), static_cast<uint8_t>(mask_key)};
    for (uint8_t m : mask) out.push_back(m);
    for (size_t i = 0; i < length; i++) out.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
    return out;
}

}  // namespace

TEST_CASE(websocket_accept_key_matches_the_rfc_example) {
    // RFC 6455 section 1.3.
    CHECK(accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE(websocket_sha1_matches_known_vectors) {
    auto hex = [](const std::string& input) {
        uint8_t digest[20];
        fernsdr::sha1(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest);
        static const char* d = "0123456789abcdef";
        std::string out;
        for (uint8_t b : digest) { out += d[b >> 4]; out += d[b & 15]; }
        return out;
    };
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: exercises the extra padding block.
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(std::string(1000, 'a')).size() == 40);
}

TEST_CASE(websocket_encodes_the_three_length_forms) {
    std::vector<uint8_t> out;
    encode_frame(Opcode::Binary, nullptr, 0, out);
    CHECK_EQ(out[0], 0x82);
    CHECK_EQ(out[1], 0);

    out.clear();
    std::vector<uint8_t> medium(200, 0xAB);
    encode_frame(Opcode::Binary, medium.data(), medium.size(), out);
    CHECK_EQ(out[1], 126);
    CHECK_EQ((out[2] << 8) | out[3], 200);
    CHECK_EQ(static_cast<long long>(out.size()), 4 + 200);

    out.clear();
    std::vector<uint8_t> large(70000, 0xCD);
    encode_frame(Opcode::Binary, large.data(), large.size(), out);
    CHECK_EQ(out[1], 127);
    // All eight length bytes, big-endian: 70000 is 0x11170.
    const uint8_t length[8] = {0, 0, 0, 0, 0, 0x01, 0x11, 0x70};
    for (int i = 0; i < 8; i++) CHECK_EQ(out[2 + i], length[i]);
    CHECK_EQ(static_cast<long long>(out.size()), 10 + 70000);
}

TEST_CASE(websocket_parses_a_masked_frame) {
    auto raw = masked_frame(Opcode::Text, "Hello");
    Frame frame;
    size_t consumed = 0;
    CHECK(parse_frame(raw.data(), raw.size(), frame, consumed) == ParseResult::Ok);
    CHECK_EQ(consumed, static_cast<long long>(raw.size()));
    CHECK(frame.opcode == Opcode::Text);
    CHECK(frame.fin);
    CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "Hello");
}

TEST_CASE(websocket_parser_handles_arbitrary_segmentation) {
    // TCP gives no framing guarantees, so every prefix must report NeedMore
    // rather than mis-parsing or reading past the buffer.
    auto raw = masked_frame(Opcode::Binary, std::string(300, 'x'));
    Frame frame;
    size_t consumed = 0;
    for (size_t prefix = 0; prefix < raw.size(); prefix++) {
        CHECK(parse_frame(raw.data(), prefix, frame, consumed) == ParseResult::NeedMore);
    }
    CHECK(parse_frame(raw.data(), raw.size(), frame, consumed) == ParseResult::Ok);
    CHECK_EQ(static_cast<long long>(frame.payload.size()), 300);
}

TEST_CASE(websocket_rejects_unmasked_client_frames) {
    // An unmasked client frame is a protocol violation and must not be
    // silently accepted.
    std::vector<uint8_t> raw;
    encode_frame(Opcode::Text, reinterpret_cast<const uint8_t*>("hi"), 2, raw);
    Frame frame;
    size_t consumed = 0;
    CHECK(parse_frame(raw.data(), raw.size(), frame, consumed) == ParseResult::Error);
}

TEST_CASE(websocket_rejects_malformed_frames) {
    Frame frame;
    size_t consumed = 0;

    // Reserved bits set.
    std::vector<uint8_t> reserved = {0xC1, 0x80, 0, 0, 0, 0};
    CHECK(parse_frame(reserved.data(), reserved.size(), frame, consumed) == ParseResult::Error);

    // Unknown opcode.
    std::vector<uint8_t> bad_opcode = {0x85, 0x80, 0, 0, 0, 0};
    CHECK(parse_frame(bad_opcode.data(), bad_opcode.size(), frame, consumed) == ParseResult::Error);

    // Fragmented control frame.
    auto fragmented_ping = masked_frame(Opcode::Ping, "x", false);
    CHECK(parse_frame(fragmented_ping.data(), fragmented_ping.size(), frame, consumed) == ParseResult::Error);

    // Oversized control frame.
    auto big_ping = masked_frame(Opcode::Ping, std::string(200, 'p'));
    CHECK(parse_frame(big_ping.data(), big_ping.size(), frame, consumed) == ParseResult::Error);
}

TEST_CASE(websocket_reassembles_fragmented_messages) {
    MessageAssembler assembler;
    std::vector<uint8_t> message;
    Opcode opcode;

    Frame first{Opcode::Text, false, {'a', 'b'}};
    Frame middle{Opcode::Continuation, false, {'c'}};
    Frame last{Opcode::Continuation, true, {'d', 'e'}};

    CHECK(assembler.feed(first, message, opcode) == MessageAssembler::Status::Incomplete);
    CHECK(assembler.feed(middle, message, opcode) == MessageAssembler::Status::Incomplete);
    CHECK(assembler.feed(last, message, opcode) == MessageAssembler::Status::Complete);
    CHECK(std::string(message.begin(), message.end()) == "abcde");
    CHECK(opcode == Opcode::Text);
}

TEST_CASE(websocket_control_frames_pass_through_a_fragmented_message) {
    MessageAssembler assembler;
    std::vector<uint8_t> message;
    Opcode opcode;

    Frame first{Opcode::Text, false, {'a'}};
    Frame ping{Opcode::Ping, true, {'p'}};
    Frame last{Opcode::Continuation, true, {'b'}};

    CHECK(assembler.feed(first, message, opcode) == MessageAssembler::Status::Incomplete);
    CHECK(assembler.feed(ping, message, opcode) == MessageAssembler::Status::Complete);
    CHECK(opcode == Opcode::Ping);
    // The interleaved ping must not have disturbed the partial message.
    CHECK(assembler.feed(last, message, opcode) == MessageAssembler::Status::Complete);
    CHECK(std::string(message.begin(), message.end()) == "ab");
}

TEST_CASE(websocket_assembler_enforces_a_size_limit) {
    // Otherwise a client could open a fragmented message and never end it.
    MessageAssembler assembler(100);
    std::vector<uint8_t> message;
    Opcode opcode;

    Frame chunk{Opcode::Binary, false, std::vector<uint8_t>(60, 0)};
    CHECK(assembler.feed(chunk, message, opcode) == MessageAssembler::Status::Incomplete);
    CHECK(assembler.feed(chunk, message, opcode) == MessageAssembler::Status::Error);
}

TEST_CASE(websocket_assembler_rejects_interleaved_data_messages) {
    MessageAssembler assembler;
    std::vector<uint8_t> message;
    Opcode opcode;
    Frame open{Opcode::Text, false, {'a'}};
    CHECK(assembler.feed(open, message, opcode) == MessageAssembler::Status::Incomplete);
    CHECK(assembler.feed(open, message, opcode) == MessageAssembler::Status::Error);

    // A continuation with no message in progress is equally invalid.
    MessageAssembler fresh;
    Frame orphan{Opcode::Continuation, true, {'x'}};
    CHECK(fresh.feed(orphan, message, opcode) == MessageAssembler::Status::Error);
}

TEST_CASE(base64_decodes_what_it_encodes_and_nothing_else) {
    std::string out;
    for (const std::string text : {std::string(), std::string("f"), std::string("fo"), std::string("foo"),
                                   std::string("foob"), std::string("\0\xff\x10 bytes", 8)}) {
        const std::string encoded =
            fernsdr::base64_encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        CHECK(fernsdr::base64_decode(encoded, out));
        CHECK(out == text);
    }
    for (const char* bad : {"Zg", "Zg=", "Z===", "Zg=a", "Zm9v!A==", "Zg==Zm9v", "Zm 9v"}) {
        CHECK(!fernsdr::base64_decode(bad, out));
    }
}
