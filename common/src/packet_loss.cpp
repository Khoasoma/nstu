#include "nstu/packet_loss.hpp"

#include <algorithm>
#include <limits>

namespace nstu::net {
namespace {

void saturating_add(std::uint64_t& value, std::uint64_t amount) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value = amount > maximum - value ? maximum : value + amount;
}

} // namespace

std::uint64_t PacketLossStats::finalized_sample_size() const noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return confirmed_lost > maximum - finalized_received
               ? maximum
               : finalized_received + confirmed_lost;
}

std::uint32_t PacketLossStats::loss_per_mille() const noexcept {
    const auto sample_size = finalized_sample_size();
    if (sample_size == 0) {
        return 0;
    }
    const long double ratio = static_cast<long double>(confirmed_lost) /
                              static_cast<long double>(sample_size);
    return static_cast<std::uint32_t>(ratio * 1000.0L);
}

std::optional<PacketLossStats> packet_loss_delta(
    const PacketLossStats& newer, const PacketLossStats& older) noexcept {
    if (newer.stream_id != older.stream_id ||
        newer.generation != older.generation) {
        return std::nullopt;
    }
    const std::uint64_t PacketLossStats::* fields[] = {
        &PacketLossStats::finalized_received,
        &PacketLossStats::confirmed_lost,
        &PacketLossStats::unique_received,
        &PacketLossStats::duplicates,
        &PacketLossStats::reordered,
        &PacketLossStats::too_late,
        &PacketLossStats::wrong_stream,
        &PacketLossStats::invalid_forward_jumps,
    };
    for (const auto field : fields) {
        if (newer.*field < older.*field) {
            return std::nullopt;
        }
    }
    PacketLossStats delta;
    delta.stream_id = newer.stream_id;
    delta.generation = newer.generation;
    delta.finalized_received =
        newer.finalized_received - older.finalized_received;
    delta.confirmed_lost = newer.confirmed_lost - older.confirmed_lost;
    delta.unique_received = newer.unique_received - older.unique_received;
    delta.duplicates = newer.duplicates - older.duplicates;
    delta.reordered = newer.reordered - older.reordered;
    delta.too_late = newer.too_late - older.too_late;
    delta.wrong_stream = newer.wrong_stream - older.wrong_stream;
    delta.invalid_forward_jumps =
        newer.invalid_forward_jumps - older.invalid_forward_jumps;
    return delta;
}

PacketLossTracker::PacketLossTracker(std::size_t reorder_window,
                                     std::uint64_t max_forward_jump)
    : received_(std::clamp<std::size_t>(reorder_window, 8, 65'536),
                std::uint8_t{0}),
      max_forward_jump_(std::max<std::uint64_t>(max_forward_jump, 1)) {}

void PacketLossTracker::reset(
    std::uint32_t stream_id,
    std::optional<std::uint64_t> initial_sequence) noexcept {
    std::fill(received_.begin(), received_.end(), std::uint8_t{0});
    head_ = 0;
    window_start_ = initial_sequence.value_or(0);
    highest_sequence_ = initial_sequence.value_or(0);
    stream_id_ = stream_id;
    ++generation_;
    stream_set_ = true;
    sequence_set_ = initial_sequence.has_value();
    packet_seen_ = false;
    stats_ = {};
    stats_.stream_id = stream_id;
    stats_.generation = generation_;
}

PacketDisposition PacketLossTracker::observe(
    const protocol::VideoPacketHeader& packet) noexcept {
    if (!stream_set_) {
        return PacketDisposition::uninitialized;
    }
    if (packet.stream_id != stream_id_) {
        saturating_add(stats_.wrong_stream, 1);
        return PacketDisposition::wrong_stream;
    }

    const std::uint64_t sequence = packet.packet_sequence;
    if (!sequence_set_) {
        sequence_set_ = true;
        packet_seen_ = true;
        window_start_ = sequence;
        highest_sequence_ = sequence;
        received_[head_] = 1;
        saturating_add(stats_.unique_received, 1);
        return PacketDisposition::in_order;
    }

    if (sequence < window_start_) {
        saturating_add(stats_.too_late, 1);
        return PacketDisposition::too_late;
    }
    const std::uint64_t forward_reference =
        packet_seen_ ? highest_sequence_ : window_start_;
    if (sequence > forward_reference &&
        sequence - forward_reference > max_forward_jump_) {
        saturating_add(stats_.invalid_forward_jumps, 1);
        return PacketDisposition::invalid_forward_jump;
    }

    const bool below_highest = packet_seen_ && sequence < highest_sequence_;
    const std::uint64_t offset_before_advance = sequence - window_start_;
    if (offset_before_advance >= received_.size()) {
        advance(offset_before_advance - received_.size() + 1);
    }
    const auto offset = static_cast<std::size_t>(sequence - window_start_);
    const auto index = (head_ + offset) % received_.size();
    if (received_[index] != 0) {
        saturating_add(stats_.duplicates, 1);
        return PacketDisposition::duplicate;
    }

    received_[index] = 1;
    saturating_add(stats_.unique_received, 1);
    if (!packet_seen_ || sequence > highest_sequence_) {
        highest_sequence_ = sequence;
    }
    packet_seen_ = true;
    if (below_highest) {
        saturating_add(stats_.reordered, 1);
        return PacketDisposition::reordered;
    }
    return PacketDisposition::in_order;
}

bool PacketLossTracker::finalize_through(std::uint64_t sequence) noexcept {
    if (!packet_seen_ || sequence < window_start_ ||
        sequence > highest_sequence_) {
        return false;
    }
    advance(sequence - window_start_ + 1);
    return true;
}

void PacketLossTracker::flush() noexcept {
    if (packet_seen_ && highest_sequence_ >= window_start_) {
        advance(highest_sequence_ - window_start_ + 1);
    }
}

const PacketLossStats& PacketLossTracker::stats() const noexcept {
    return stats_;
}

std::uint32_t PacketLossTracker::stream_id() const noexcept {
    return stream_id_;
}

bool PacketLossTracker::initialized() const noexcept {
    return stream_set_ && packet_seen_;
}

void PacketLossTracker::advance(std::uint64_t count) noexcept {
    const auto window_size = static_cast<std::uint64_t>(received_.size());
    if (count >= window_size) {
        for (std::size_t index = 0; index < received_.size(); ++index) {
            finalize_slot(index);
        }
        saturating_add(stats_.confirmed_lost, count - window_size);
        std::fill(received_.begin(), received_.end(), std::uint8_t{0});
        head_ = 0;
    } else {
        for (std::uint64_t index = 0; index < count; ++index) {
            finalize_slot(head_);
            received_[head_] = 0;
            head_ = (head_ + 1) % received_.size();
        }
    }
    window_start_ += count;
}

void PacketLossTracker::finalize_slot(std::size_t index) noexcept {
    if (received_[index] != 0) {
        saturating_add(stats_.finalized_received, 1);
    } else {
        saturating_add(stats_.confirmed_lost, 1);
    }
}

} // namespace nstu::net
