#pragma once

#include "nstu/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nstu::net {

enum class PacketDisposition : std::uint8_t {
    uninitialized,
    in_order,
    reordered,
    duplicate,
    too_late,
    wrong_stream,
    invalid_forward_jump,
};

struct PacketLossStats {
    std::uint32_t stream_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t finalized_received = 0;
    std::uint64_t confirmed_lost = 0;
    std::uint64_t unique_received = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t reordered = 0;
    std::uint64_t too_late = 0;
    std::uint64_t wrong_stream = 0;
    std::uint64_t invalid_forward_jumps = 0;

    [[nodiscard]] std::uint64_t finalized_sample_size() const noexcept;
    [[nodiscard]] std::uint32_t loss_per_mille() const noexcept;
};

[[nodiscard]] std::optional<PacketLossStats> packet_loss_delta(
    const PacketLossStats& newer, const PacketLossStats& older) noexcept;

// Tracks one authenticated video stream. Missing sequence numbers are not
// counted as lost until they leave the reorder window or the stream is flushed.
class PacketLossTracker {
public:
    explicit PacketLossTracker(std::size_t reorder_window = 256,
                               std::uint64_t max_forward_jump = 1'000'000);

    void reset(std::uint32_t stream_id,
               std::optional<std::uint64_t> initial_sequence = std::nullopt)
        noexcept;
    [[nodiscard]] PacketDisposition observe(
        const protocol::VideoPacketHeader& packet) noexcept;
    [[nodiscard]] bool finalize_through(std::uint64_t sequence) noexcept;
    void flush() noexcept;

    [[nodiscard]] const PacketLossStats& stats() const noexcept;
    [[nodiscard]] std::uint32_t stream_id() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    void advance(std::uint64_t count) noexcept;
    void finalize_slot(std::size_t index) noexcept;

    std::vector<std::uint8_t> received_;
    std::size_t head_ = 0;
    std::uint64_t window_start_ = 0;
    std::uint64_t highest_sequence_ = 0;
    std::uint64_t max_forward_jump_ = 0;
    std::uint32_t stream_id_ = 0;
    std::uint64_t generation_ = 0;
    bool stream_set_ = false;
    bool sequence_set_ = false;
    bool packet_seen_ = false;
    PacketLossStats stats_;
};

} // namespace nstu::net
