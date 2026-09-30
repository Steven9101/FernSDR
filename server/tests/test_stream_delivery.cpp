#include "../src/core/stream_delivery.h"
#include "../src/core/protocol.h"
#include "../src/net/output_queue.h"
#include "test_util.h"

namespace {
using fernsdr::StreamDelivery;
using fernsdr::OutputQueue;
using fernsdr::ws::Opcode;

std::vector<uint8_t> audio(uint16_t sequence, uint8_t flags = 0x32) {
    std::vector<uint8_t> payload(64, 0);
    payload[0] = 1;
    payload[1] = flags;
    fernsdr::proto::write_u16(payload.data() + 2, sequence);
    return payload;
}
std::vector<uint8_t> waterfall(uint16_t sequence, int mode, bool adaptive = false) {
    std::vector<uint8_t> payload(23, 0);
    payload[0] = 2;
    payload[1] = adaptive ? 2 : 0;
    fernsdr::proto::write_u16(payload.data() + 2, sequence);
    payload[22] = static_cast<uint8_t>(mode << (adaptive ? 6 : 7));
    return payload;
}
// A range-coded (wfc5) line: flag 16, and the payload's first byte is 1 on a
// key row.
std::vector<uint8_t> ranged(uint16_t sequence, bool key) {
    std::vector<uint8_t> payload(23, 0);
    payload[0] = 2;
    payload[1] = 16;
    fernsdr::proto::write_u16(payload.data() + 2, sequence);
    payload[22] = key ? 1 : 0;
    return payload;
}
void enqueue(StreamDelivery& delivery, OutputQueue& queue, std::vector<uint8_t> payload, int64_t at) {
    if (delivery.prepare(payload)) queue.frame(Opcode::Binary, payload.data(), payload.size(), at);
}
size_t expire(StreamDelivery& delivery, OutputQueue& queue, int64_t now) {
    StreamDelivery::Repair repair;
    const size_t dropped = queue.prune_binary(now, 250, [&](uint8_t* data, size_t size, int64_t age) {
        return delivery.retain(data, size, age >= 250, repair);
    });
    delivery.finish_expiry(repair);
    return dropped;
}
std::vector<std::vector<uint8_t>> drain(OutputQueue& queue) {
    std::vector<std::vector<uint8_t>> payloads;
    while (queue.size()) {
        // These fixtures use short unmasked server frames. The production
        // input parser deliberately accepts only masked client frames.
        CHECK(queue.size() >= 2);
        if (queue.size() < 2) break;
        CHECK_EQ(queue.data()[0], 0x82);
        const size_t length = queue.data()[1];
        CHECK(length < 126 && length + 2 <= queue.size());
        if (length >= 126 || length + 2 > queue.size()) break;
        const size_t consumed = length + 2;
        payloads.emplace_back(queue.data() + 2, queue.data() + consumed);
        queue.consume(consumed);
    }
    return payloads;
}
}

TEST_CASE(stream_delivery_keeps_legacy_audio_and_marks_only_negotiated_discontinuities) {
    for (bool enabled : {false, true}) {
        StreamDelivery delivery;
        OutputQueue queue;
        if (enabled) delivery.set_audio_discontinuity(true);
        enqueue(delivery, queue, audio(65534), 0);
        enqueue(delivery, queue, audio(65535), 490);
        CHECK_EQ(expire(delivery, queue, 500), enabled ? 1 : 0);
        const auto packets = drain(queue);
        CHECK_EQ(packets.size(), enabled ? 1 : 2);
        CHECK_EQ(packets.back()[1], enabled ? 0x36 : 0x32);
        auto next = audio(0);
        CHECK(delivery.prepare(next));
        CHECK_EQ(next[1], 0x32);
    }
}

TEST_CASE(stream_delivery_carries_repair_across_repeated_expiry_and_partial_writes) {
    StreamDelivery delivery;
    delivery.set_audio_discontinuity(true);
    OutputQueue queue;
    enqueue(delivery, queue, audio(1), 0);
    enqueue(delivery, queue, audio(2), 490);
    CHECK_EQ(expire(delivery, queue, 500), 1);
    enqueue(delivery, queue, audio(3), 790);
    CHECK_EQ(expire(delivery, queue, 800), 1);
    CHECK_EQ(queue.data()[3] & 4, 4);
    const std::vector<uint8_t> marked(queue.data(), queue.data() + queue.size());
    queue.consume(1);
    enqueue(delivery, queue, audio(4), 800);
    CHECK_EQ(expire(delivery, queue, 1100), 1);
    CHECK_EQ(queue.size(), marked.size() - 1);
    CHECK(std::equal(marked.begin() + 1, marked.end(), queue.data()));
    queue.consume(queue.size());
    enqueue(delivery, queue, audio(5), 1110);
    const auto packets = drain(queue);
    CHECK_EQ(packets.size(), 1);
    CHECK_EQ(packets[0][1], 0x36);
}

TEST_CASE(stream_delivery_replaces_capabilities_without_rewriting_already_queued_frames) {
    StreamDelivery delivery;
    delivery.set_audio_discontinuity(true);
    OutputQueue queue;
    enqueue(delivery, queue, audio(1), 0);
    enqueue(delivery, queue, audio(2), 490);
    CHECK_EQ(expire(delivery, queue, 500), 1);
    delivery.set_audio_discontinuity(false);
    enqueue(delivery, queue, audio(3), 510);
    CHECK_EQ(expire(delivery, queue, 800), 0);
    const auto packets = drain(queue);
    CHECK_EQ(packets.size(), 2);
    CHECK_EQ(packets[0][1] & 4, 4);
    CHECK_EQ(packets[1][1] & 4, 0);
    auto gap = audio(9);
    CHECK(delivery.prepare(gap));
    CHECK_EQ(gap[1] & 4, 0);
    delivery.set_audio_discontinuity(true);
    auto next = audio(10);
    CHECK(delivery.prepare(next));
    CHECK_EQ(next[1] & 4, 0);
}

TEST_CASE(stream_delivery_recovers_both_waterfall_headers_after_queued_and_future_loss) {
    for (bool adaptive : {false, true}) {
        StreamDelivery delivery;
        OutputQueue queue;
        enqueue(delivery, queue, waterfall(0, adaptive ? 3 : 1, adaptive), 0);
        enqueue(delivery, queue, waterfall(1, 0), 490);
        enqueue(delivery, queue, waterfall(2, 2, true), 490);
        enqueue(delivery, queue, audio(1), 490);
        CHECK_EQ(expire(delivery, queue, 500), 3);
        CHECK(delivery.needs_keyframe());
        auto dependent = waterfall(3, 2, true);
        CHECK(!delivery.prepare(dependent));
        enqueue(delivery, queue, waterfall(4, adaptive ? 3 : 1, adaptive), 510);
        CHECK(!delivery.needs_keyframe());
        enqueue(delivery, queue, waterfall(5, 2, true), 520);
        const auto packets = drain(queue);
        CHECK_EQ(packets.size(), 3);
        CHECK_EQ(packets[0][1] & 4, 0);
        CHECK_EQ(packets[1][2], 4);
        CHECK_EQ(packets[2][2], 5);
    }
}

TEST_CASE(stream_delivery_recovers_range_coded_waterfalls_on_their_key_rows) {
    // A band switch restarts the line sequence, which reads as a loss; the
    // new band's first line is a key row and must end the wait for one.
    StreamDelivery delivery;
    for (uint16_t seq = 0; seq < 40; seq++) {
        auto line = ranged(seq, seq == 0);
        CHECK(delivery.prepare(line));
    }
    auto dependent = ranged(0, false);
    CHECK(!delivery.prepare(dependent));
    CHECK(delivery.needs_keyframe());
    auto key = ranged(1, true);
    CHECK(delivery.prepare(key));
    CHECK(!delivery.needs_keyframe());
    auto next = ranged(2, false);
    CHECK(delivery.prepare(next));
}

TEST_CASE(stream_delivery_does_not_let_an_earlier_keyframe_repair_a_later_tail_loss) {
    StreamDelivery delivery;
    delivery.set_audio_discontinuity(true);
    OutputQueue queue;
    enqueue(delivery, queue, audio(0), 0);
    enqueue(delivery, queue, waterfall(0, 1), 490);
    auto lost = waterfall(1, 0);
    CHECK(!delivery.prepare(lost, true));
    CHECK(delivery.needs_keyframe());
    CHECK_EQ(expire(delivery, queue, 500), 1);
    CHECK(delivery.needs_keyframe());
    auto dependent = waterfall(2, 0);
    CHECK(!delivery.prepare(dependent));
    enqueue(delivery, queue, waterfall(3, 3, true), 510);
    CHECK(!delivery.needs_keyframe());
    enqueue(delivery, queue, waterfall(4, 2, true), 790);
    CHECK_EQ(expire(delivery, queue, 800), 3);
    CHECK(delivery.needs_keyframe());
    CHECK_EQ(queue.size(), 0);
}

TEST_CASE(stream_delivery_detects_outbox_loss_without_confusing_wrap_or_configuration) {
    StreamDelivery delivery;
    delivery.set_audio_discontinuity(true);
    for (uint16_t seq : {uint16_t{65534}, uint16_t{65535}, uint16_t{0}}) {
        auto packet = audio(seq, 0xf3);
        CHECK(delivery.prepare(packet));
        CHECK_EQ(packet[1], 0xf3);
    }
    auto missing = audio(3, 0xf3);
    CHECK(delivery.prepare(missing));
    CHECK_EQ(missing[1], 0xf7);
    auto changed = audio(50, 0x01);
    CHECK(delivery.prepare(changed));
    CHECK_EQ(changed[1], 0x01);
    auto key = waterfall(1, 1);
    CHECK(delivery.prepare(key));
    auto dependent = waterfall(3, 2, true);
    CHECK(!delivery.prepare(dependent));
    CHECK(delivery.needs_keyframe());
    std::vector<uint8_t> meter(26, 0);
    meter[0] = 3;
    StreamDelivery::Repair repair;
    CHECK(delivery.retain(meter.data(), meter.size(), true, repair));
    CHECK(delivery.prepare(meter, true));
}

TEST_CASE(stream_delivery_follows_multi_frame_packets_by_their_frame_count) {
    // A NAC3 packet names its frame count in its first payload byte; the next
    // packet's sequence is that many frames on. Treating each packet as one
    // frame would mark every packet as a loss and discard the player's queue.
    StreamDelivery delivery;
    delivery.set_audio_discontinuity(true);
    OutputQueue queue;
    auto packet = [](uint16_t sequence, int frames) {
        auto payload = audio(sequence, 0x30 | fernsdr::proto::kAudioFlagPacket);
        payload[4] = static_cast<uint8_t>((frames - 1) << 6);
        return payload;
    };
    enqueue(delivery, queue, packet(100, 3), 0);
    enqueue(delivery, queue, packet(103, 2), 0);
    enqueue(delivery, queue, packet(105, 4), 0);
    enqueue(delivery, queue, packet(109, 1), 0);
    for (const auto& payload : drain(queue)) CHECK_EQ(payload[1] & fernsdr::proto::kAudioFlagDiscontinuity, 0);
    // A real gap: 110 expected, 113 sent.
    enqueue(delivery, queue, packet(113, 2), 0);
    const auto after_gap = drain(queue);
    CHECK_EQ(after_gap.size(), 1u);
    if (!after_gap.empty()) CHECK(after_gap[0][1] & fernsdr::proto::kAudioFlagDiscontinuity);
    CHECK_EQ(fernsdr::proto::audio_frames(packet(1, 4).data(), 64), 4);
    CHECK_EQ(fernsdr::proto::audio_frames(audio(1).data(), 64), 1);
}
