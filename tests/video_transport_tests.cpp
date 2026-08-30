#include "nstu/frame_reassembler.hpp"
#include "nstu/packet_loss.hpp"
#include "nstu/video_transport.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x30 + index);
    }
    std::vector<std::byte> access_unit(5000);
    for (std::size_t index = 0; index < access_unit.size(); ++index) {
        access_unit[index] = static_cast<std::byte>(index & 0xffu);
    }

    nstu::video::VideoPacketizer packetizer({.maximum_datagram_bytes = 600});
    packetizer.reset(17, 100, 50);
    std::string error;
    const auto packets = packetizer.packetize(access_unit, 123'456, true, key,
                                               &error);
    assert(packets.has_value() && packets->size() > 5);
    assert(packetizer.next_packet_sequence() == 100 + packets->size());
    assert(packetizer.next_frame_id() == 51);

    std::vector<nstu::video::VideoDatagram> decoded;
    decoded.reserve(packets->size());
    for (const auto& packet : *packets) {
        const auto wire = packet.wire();
        assert(wire.size() <= 600);
        const auto parsed = nstu::video::decode_video_datagram(wire, key, &error);
        assert(parsed.has_value());
        decoded.push_back(*parsed);
        auto tampered = wire;
        tampered.back() ^= std::byte{1};
        assert(!nstu::video::decode_video_datagram(tampered, key, &error)
                    .has_value());
    }

    nstu::net::PacketLossTracker loss(32);
    loss.reset(17, 100);
    nstu::video::FrameReassembler reassembler({
        .max_frames_in_flight = 4,
        .max_fragments_per_frame = 128,
        .max_fragment_payload = 1024,
        .max_frame_bytes = 16 * 1024,
        .max_total_buffered_bytes = 32 * 1024,
        .frame_deadline = 100ms,
    });
    reassembler.reset(17);
    const auto start = std::chrono::steady_clock::time_point{1s};
    std::swap(decoded[1], decoded[2]);
    std::optional<nstu::video::CompletedFrame> completed;
    for (const auto& packet : decoded) {
        (void)loss.observe(packet.header);
        auto result = reassembler.push(packet.header, packet.payload, start);
        if (result.frame) {
            completed = std::move(result.frame);
        }
    }
    loss.flush();
    assert(loss.stats().confirmed_lost == 0);
    assert(loss.stats().reordered == 1);
    assert(completed.has_value());
    assert(completed->payload == access_unit);
    assert(completed->keyframe);

    nstu::video::FrameJitterBuffer jitter({.target_delay = 40ms,
                                           .maximum_frames = 4});
    jitter.reset(17);
    auto later = *completed;
    later.frame_id = 52;
    auto earlier = *completed;
    earlier.frame_id = 51;
    assert(jitter.push(std::move(later), start));
    assert(jitter.push(std::move(earlier), start + 2ms));
    assert(!jitter.pop_ready(start + 39ms).has_value());
    auto first = jitter.pop_ready(start + 42ms);
    assert(first.has_value() && first->frame_id == 51);
    auto second = jitter.pop_ready(start + 42ms);
    assert(second.has_value() && second->frame_id == 52);

    nstu::video::NackPolicy nack_policy({
        .initial_delay = 10ms,
        .retry_interval = 20ms,
        .maximum_attempts = 2,
        .maximum_fragments_per_request = 3,
    });
    nack_policy.reset(17);
    const std::vector<std::uint16_t> missing{1, 3, 5, 7};
    assert(!nack_policy.consider(60, missing, start, start + 100ms,
                                 start + 9ms));
    const auto first_nack = nack_policy.consider(
        60, missing, start, start + 100ms, start + 10ms);
    assert(first_nack.has_value() && first_nack->attempt == 1);
    assert(first_nack->missing_fragments ==
           std::vector<std::uint16_t>({1, 3, 5}));
    const auto nack_wire = nstu::video::encode_video_nack(*first_nack);
    const auto decoded_nack = nstu::video::decode_video_nack(nack_wire);
    assert(decoded_nack.has_value());
    assert(decoded_nack->missing_fragments == first_nack->missing_fragments);
    const auto second_nack = nack_policy.consider(
        60, missing, start, start + 100ms, start + 30ms);
    assert(second_nack.has_value() && second_nack->attempt == 2);
    assert(nack_policy.exhausted(60));
    assert(!nack_policy.consider(60, missing, start, start + 100ms,
                                 start + 60ms));

    nstu::video::KeyframeScheduler scheduler({
        .minimum_interval = 100ms,
        .maximum_interval = 1000ms,
        .loss_threshold_per_mille = 50,
    });
    scheduler.reset(start);
    assert(scheduler.due(start) == nstu::video::KeyframeReason::stream_start);
    scheduler.mark_emitted(start);
    scheduler.observe_loss(60);
    assert(!scheduler.due(start + 99ms).has_value());
    assert(scheduler.due(start + 100ms) ==
           nstu::video::KeyframeReason::packet_loss);
    scheduler.mark_emitted(start + 100ms);
    assert(scheduler.due(start + 1100ms) ==
           nstu::video::KeyframeReason::periodic);

    nstu::security::secure_zero(key);
    return 0;
}
