#pragma once

#include "nstu/auth.hpp"
#include "nstu/frame_reassembler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::video {

struct VideoDatagram {
    protocol::VideoPacketHeader header;
    security::VideoAuthTag tag{};
    std::vector<std::byte> payload;

    [[nodiscard]] std::vector<std::byte> wire() const;
};

struct PacketizerConfig {
    std::size_t maximum_datagram_bytes = 1200;
    std::uint16_t maximum_fragments = 4096;
};

class VideoPacketizer {
public:
    explicit VideoPacketizer(PacketizerConfig config = {});

    void reset(std::uint32_t stream_id,
               std::uint64_t first_packet_sequence = 0,
               std::uint64_t first_frame_id = 0) noexcept;
    [[nodiscard]] std::optional<std::vector<VideoDatagram>> packetize(
        std::span<const std::byte> access_unit,
        std::uint64_t capture_time_100ns, bool keyframe,
        std::span<const std::byte> authentication_key,
        std::string* error = nullptr);
    [[nodiscard]] std::uint64_t next_packet_sequence() const noexcept;
    [[nodiscard]] std::uint64_t next_frame_id() const noexcept;

private:
    PacketizerConfig config_;
    std::uint32_t stream_id_ = 0;
    std::uint64_t next_packet_sequence_ = 0;
    std::uint64_t next_frame_id_ = 0;
    bool initialized_ = false;
};

[[nodiscard]] std::optional<VideoDatagram> decode_video_datagram(
    std::span<const std::byte> wire,
    std::span<const std::byte> authentication_key,
    std::string* error = nullptr);

struct JitterBufferConfig {
    std::chrono::milliseconds target_delay{80};
    std::size_t maximum_frames = 32;
};

struct JitterBufferStats {
    std::uint64_t accepted = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t late = 0;
    std::uint64_t resource_dropped = 0;
    std::uint64_t released = 0;
};

class FrameJitterBuffer {
public:
    explicit FrameJitterBuffer(JitterBufferConfig config = {});

    void reset(std::uint32_t stream_id) noexcept;
    [[nodiscard]] bool push(
        CompletedFrame frame,
        std::chrono::steady_clock::time_point arrival_time) noexcept;
    [[nodiscard]] std::optional<CompletedFrame> pop_ready(
        std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const JitterBufferStats& stats() const noexcept;

private:
    struct BufferedFrame {
        CompletedFrame frame;
        std::chrono::steady_clock::time_point ready_at{};
    };

    JitterBufferConfig config_;
    std::uint32_t stream_id_ = 0;
    bool stream_set_ = false;
    std::optional<std::uint64_t> last_released_frame_id_;
    std::map<std::uint64_t, BufferedFrame> frames_;
    JitterBufferStats stats_;
};

struct NackPolicyConfig {
    std::chrono::milliseconds initial_delay{12};
    std::chrono::milliseconds retry_interval{20};
    std::uint8_t maximum_attempts = 2;
    std::size_t maximum_fragments_per_request = 128;
};

struct VideoNack {
    std::uint32_t stream_id = 0;
    std::uint64_t frame_id = 0;
    std::uint8_t attempt = 0;
    std::vector<std::uint16_t> missing_fragments;
};

class NackPolicy {
public:
    explicit NackPolicy(NackPolicyConfig config = {});

    void reset(std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::optional<VideoNack> consider(
        std::uint64_t frame_id, std::span<const std::uint16_t> missing,
        std::chrono::steady_clock::time_point first_fragment_time,
        std::chrono::steady_clock::time_point frame_deadline,
        std::chrono::steady_clock::time_point now);
    void complete(std::uint64_t frame_id) noexcept;
    [[nodiscard]] bool exhausted(std::uint64_t frame_id) const noexcept;
    std::size_t expire(std::chrono::steady_clock::time_point now) noexcept;

private:
    struct State {
        std::uint8_t attempts = 0;
        std::chrono::steady_clock::time_point next_request{};
        std::chrono::steady_clock::time_point deadline{};
    };

    NackPolicyConfig config_;
    std::uint32_t stream_id_ = 0;
    bool stream_set_ = false;
    std::map<std::uint64_t, State> states_;
};

[[nodiscard]] std::vector<std::byte> encode_video_nack(const VideoNack& nack);
[[nodiscard]] std::optional<VideoNack> decode_video_nack(
    std::span<const std::byte> payload);

enum class KeyframeReason : std::uint8_t {
    stream_start,
    periodic,
    packet_loss,
    nack_exhausted,
    decoder_recovery,
};

struct KeyframeScheduleConfig {
    std::chrono::milliseconds minimum_interval{250};
    std::chrono::milliseconds maximum_interval{2000};
    std::uint32_t loss_threshold_per_mille = 50;
};

class KeyframeScheduler {
public:
    explicit KeyframeScheduler(KeyframeScheduleConfig config = {});

    void reset(std::chrono::steady_clock::time_point now) noexcept;
    void observe_loss(std::uint32_t loss_per_mille) noexcept;
    void notify_nack_exhausted() noexcept;
    void notify_decoder_recovery() noexcept;
    [[nodiscard]] std::optional<KeyframeReason> due(
        std::chrono::steady_clock::time_point now) const noexcept;
    void mark_emitted(std::chrono::steady_clock::time_point now) noexcept;

private:
    KeyframeScheduleConfig config_;
    std::chrono::steady_clock::time_point last_keyframe_{};
    std::optional<KeyframeReason> pending_reason_;
    bool initialized_ = false;
};

} // namespace nstu::video
