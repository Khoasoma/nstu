#pragma once

#include "nstu/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nstu::video {

struct ReassemblyLimits {
    std::size_t max_frames_in_flight = 8;
    std::uint16_t max_fragments_per_frame = 4096;
    std::size_t max_fragment_payload = 1400;
    std::size_t max_frame_bytes = 4u * 1024u * 1024u;
    std::size_t max_total_buffered_bytes = 8u * 1024u * 1024u;
    std::chrono::milliseconds frame_deadline{150};
};

struct CompletedFrame {
    std::uint32_t stream_id = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t capture_time_100ns = 0;
    bool keyframe = false;
    std::vector<std::byte> payload;
};

enum class FragmentDisposition : std::uint8_t {
    uninitialized,
    accepted,
    duplicate,
    frame_complete,
    late_completed_frame,
    wrong_stream,
    invalid,
    resource_limit,
};

struct ReassemblyResult {
    FragmentDisposition disposition = FragmentDisposition::invalid;
    std::optional<CompletedFrame> frame;
};

struct FrameReassemblyStats {
    std::uint64_t accepted_fragments = 0;
    std::uint64_t duplicate_fragments = 0;
    std::uint64_t late_completed_frame_fragments = 0;
    std::uint64_t conflicting_fragments = 0;
    std::uint64_t invalid_fragments = 0;
    std::uint64_t completed_frames = 0;
    std::uint64_t expired_incomplete_frames = 0;
    std::uint64_t resource_dropped_frames = 0;
    std::uint64_t resource_limit_rejections = 0;
    std::size_t buffered_bytes = 0;
    std::size_t peak_buffered_bytes = 0;
};

class FrameReassembler {
public:
    explicit FrameReassembler(ReassemblyLimits limits = {});

    void reset(std::uint32_t stream_id) noexcept;
    [[nodiscard]] ReassemblyResult push(
        const protocol::VideoPacketHeader& header,
        std::span<const std::byte> payload,
        std::chrono::steady_clock::time_point now);
    std::size_t expire(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<std::vector<std::uint16_t>> missing_fragments(
        std::uint64_t frame_id) const;

    [[nodiscard]] const FrameReassemblyStats& stats() const noexcept;
    [[nodiscard]] std::size_t frames_in_flight() const noexcept;

private:
    struct PartialFrame {
        std::uint16_t fragment_count = 0;
        std::uint16_t received_count = 0;
        std::uint64_t capture_time_100ns = 0;
        bool keyframe = false;
        std::size_t total_bytes = 0;
        std::chrono::steady_clock::time_point deadline{};
        std::vector<std::vector<std::byte>> fragments;
        std::vector<std::uint8_t> received;
    };

    void remember_completed(std::uint64_t frame_id);
    void drop_frame(std::unordered_map<std::uint64_t, PartialFrame>::iterator it,
                    bool resource_drop) noexcept;

    ReassemblyLimits limits_;
    std::uint32_t stream_id_ = 0;
    bool stream_set_ = false;
    std::unordered_map<std::uint64_t, PartialFrame> frames_;
    std::deque<std::uint64_t> completed_order_;
    std::unordered_set<std::uint64_t> completed_recent_;
    FrameReassemblyStats stats_;
};

} // namespace nstu::video
