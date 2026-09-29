#include "../src/net/output_queue.h"
#include "test_util.h"
#include <algorithm>
#include <cstring>

namespace {
using fernsdr::OutputQueue;
using fernsdr::ws::Opcode;

struct Fixture {
    OutputQueue queue;
    std::vector<uint8_t> wire;
    struct Span { size_t start, end; bool expired; };
    std::vector<Span> binary;

    void raw(const std::string& value) {
        queue.append(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        wire.insert(wire.end(), value.begin(), value.end());
    }
    void frame(Opcode kind, size_t size, uint8_t value, int64_t at = 0) {
        std::vector<uint8_t> payload(size, value);
        const size_t start = wire.size();
        queue.frame(kind, payload.data(), payload.size(), at);
        fernsdr::ws::encode_frame(kind, payload.data(), payload.size(), wire);
        if (kind == Opcode::Binary) binary.push_back({start, wire.size(), at < 250});
    }
};

void check_prune(Fixture& f, size_t consumed) {
    std::vector<uint8_t> emitted(f.wire.begin(), f.wire.begin() + static_cast<ptrdiff_t>(consumed));
    f.queue.consume(consumed);
    f.queue.prune_binary(500, 250, [](uint8_t*, size_t, int64_t age) { return age < 250; });
    if (f.queue.size()) emitted.insert(emitted.end(), f.queue.data(), f.queue.data() + f.queue.size());
    std::vector<uint8_t> expected;
    size_t cursor = 0;
    for (const auto& span : f.binary) {
        if (!span.expired || span.start < consumed) continue;
        expected.insert(expected.end(), f.wire.begin() + static_cast<ptrdiff_t>(cursor),
                         f.wire.begin() + static_cast<ptrdiff_t>(span.start));
        cursor = span.end;
    }
    expected.insert(expected.end(), f.wire.begin() + static_cast<ptrdiff_t>(cursor), f.wire.end());
    CHECK(emitted == expected);
    const auto remaining = f.queue.size();
    f.queue.consume(remaining);
    CHECK_EQ(f.queue.size(), 0);
}
}

TEST_CASE(output_queue_preserves_partial_frames_and_controls_at_every_byte_boundary) {
    const auto fixture = [] {
        Fixture f;
        f.raw("HTTP/1.1 101 Switching Protocols\r\n\r\n");
        f.frame(Opcode::Text, 17, 'c');
        f.frame(Opcode::Binary, 125, 'a');
        f.frame(Opcode::Pong, 9, 'p');
        f.frame(Opcode::Binary, 126, 'b');
        f.frame(Opcode::Text, 3, 't');
        f.frame(Opcode::Binary, 399, 'd');
        f.frame(Opcode::Binary, 128, 'n', 490);
        f.frame(Opcode::Close, 2, 0);
        return f;
    };
    const size_t bytes = fixture().wire.size();
    for (size_t sent = 0; sent <= bytes; sent++) {
        auto f = fixture();
        check_prune(f, sent);
    }
}

TEST_CASE(output_queue_retains_a_partial_frame_after_prefix_compaction) {
    for (size_t boundary : {size_t{0}, size_t{1}, size_t{2}, size_t{9}, size_t{10}, size_t{11},
                            size_t{65535}, size_t{65536}, size_t{65545}, size_t{65546}}) {
        Fixture f;
        f.raw(std::string(65537, 'h'));
        f.frame(Opcode::Binary, 65536, 'a');
        f.frame(Opcode::Text, 17, 'c');
        f.frame(Opcode::Binary, 125, 'b');
        f.frame(Opcode::Binary, 126, 'n', 490);
        check_prune(f, 65537 + boundary);
    }
}

TEST_CASE(output_queue_reuses_compacted_boundaries_and_never_edits_a_started_payload) {
    OutputQueue queue;
    std::vector<uint8_t> payload(70000, 1);
    queue.frame(Opcode::Binary, payload.data(), payload.size(), 0);
    queue.consume(66000);
    std::vector<uint8_t> partial(queue.data(), queue.data() + queue.size());
    payload.resize(128);
    queue.frame(Opcode::Binary, payload.data(), payload.size(), 10);
    queue.frame(Opcode::Binary, payload.data(), payload.size(), 490);
    int visits = 0;
    CHECK_EQ(queue.prune_binary(500, 250, [&](uint8_t* data, size_t size, int64_t age) {
        visits++;
        CHECK_EQ(size, 128);
        if (age >= 250) return false;
        data[0] = 9;
        return true;
    }), 1);
    CHECK_EQ(visits, 2);
    CHECK(std::equal(partial.begin(), partial.end(), queue.data()));
    queue.consume(partial.size());
    CHECK_EQ(queue.data()[4], 9);
    CHECK_EQ(queue.prune_binary(800, 250, [](uint8_t*, size_t, int64_t) { return false; }), 1);
    CHECK_EQ(queue.size(), 0);
    queue.frame(Opcode::Binary, payload.data(), payload.size(), 900);
    CHECK_EQ(queue.prune_binary(910, 250, [](uint8_t*, size_t, int64_t) {
        CHECK(false);
        return false;
    }), 0);
    queue.consume(queue.size());
    CHECK_EQ(queue.size(), 0);
}

namespace {
std::vector<uint8_t> frames_of(std::initializer_list<std::pair<Opcode, std::pair<size_t, uint8_t>>> list) {
    std::vector<uint8_t> wire;
    for (const auto& [kind, shape] : list) {
        std::vector<uint8_t> payload(shape.first, shape.second);
        fernsdr::ws::encode_frame(kind, payload.data(), payload.size(), wire);
    }
    return wire;
}
std::vector<uint8_t> queued(const OutputQueue& queue) {
    return std::vector<uint8_t>(queue.data(), queue.data() + queue.size());
}
void add(OutputQueue& queue, Opcode kind, size_t size, uint8_t value, bool bulk, int64_t at = 0) {
    std::vector<uint8_t> payload(size, value);
    queue.frame(kind, payload.data(), payload.size(), at, bulk);
}
}

TEST_CASE(output_queue_lets_audio_pass_waterfall_rows_that_are_waiting) {
    OutputQueue queue;
    add(queue, Opcode::Binary, 300, 'w', true);
    add(queue, Opcode::Binary, 200, 'x', true);
    add(queue, Opcode::Binary, 40, 'a', false);
    CHECK(queued(queue) == frames_of({{Opcode::Binary, {40, 'a'}}, {Opcode::Binary, {300, 'w'}},
                                      {Opcode::Binary, {200, 'x'}}}));
}

TEST_CASE(output_queue_keeps_each_kind_in_order) {
    OutputQueue queue;
    add(queue, Opcode::Binary, 300, 'w', true);
    add(queue, Opcode::Binary, 40, 'a', false);
    add(queue, Opcode::Binary, 200, 'x', true);
    add(queue, Opcode::Binary, 41, 'b', false);
    CHECK(queued(queue) == frames_of({{Opcode::Binary, {40, 'a'}}, {Opcode::Binary, {41, 'b'}},
                                      {Opcode::Binary, {300, 'w'}}, {Opcode::Binary, {200, 'x'}}}));
}

TEST_CASE(output_queue_never_moves_audio_past_text_or_a_started_frame) {
    // A text message can describe the audio after it: a new rate, a new
    // generation. Nothing moves in front of one.
    OutputQueue queue;
    add(queue, Opcode::Binary, 300, 'w', true);
    add(queue, Opcode::Text, 20, 't', false);
    add(queue, Opcode::Binary, 40, 'a', false);
    CHECK(queued(queue) == frames_of({{Opcode::Binary, {300, 'w'}}, {Opcode::Text, {20, 't'}},
                                      {Opcode::Binary, {40, 'a'}}}));

    // Once a byte of a row has gone, the rest of it has to follow.
    OutputQueue started;
    add(started, Opcode::Binary, 300, 'w', true);
    started.consume(3);
    add(started, Opcode::Binary, 40, 'a', false);
    auto expected = frames_of({{Opcode::Binary, {300, 'w'}}, {Opcode::Binary, {40, 'a'}}});
    expected.erase(expected.begin(), expected.begin() + 3);
    CHECK(queued(started) == expected);
}

TEST_CASE(output_queue_still_expires_a_row_waiting_behind_audio) {
    OutputQueue queue;
    add(queue, Opcode::Binary, 300, 'w', true, 0);
    add(queue, Opcode::Binary, 40, 'a', false, 400);
    // The row is older than the audio now in front of it, and the check must
    // still find it.
    CHECK(queue.has_expired_binary(500, 250));
    const size_t dropped = queue.prune_binary(500, 250, [](uint8_t*, size_t, int64_t age) { return age < 250; });
    CHECK_EQ(dropped, 1);
    CHECK(queued(queue) == frames_of({{Opcode::Binary, {40, 'a'}}}));
}
