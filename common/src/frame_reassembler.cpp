#include "nstu/frame_reassembler.hpp"

#include <algorithm>
#include <limits>

namespace nstu::video {
namespace {

bool has_flag(protocol::VideoFlags value, protocol::VideoFlags flag) noexcept {
    return (static_cast<std::uint16_t>(value) &
            static_cast<std::uint16_t>(flag)) != 0;
}

} // namespace

FrameReassembler::FrameReassembler(ReassemblyLimits limits) : limits_(limits) {
    limits_.max_frames_in_flight =
        std::clamp<std::size_t>(limits_.max_frames_in_flight, 1, 64);
    limits_.max_fragments_per_frame =
        std::clamp<std::uint16_t>(limits_.max_fragments_per_frame, 1, 16'384);
    limits_.max_fragment_payload =
        std::clamp<std::size_t>(limits_.max_fragment_payload, 1, 65'507);
    limits_.max_frame_bytes =
        std::max(limits_.max_frame_bytes, limits_.max_fragment_payload);
    limits_.max_total_buffered_bytes =
        std::max(limits_.max_total_buffered_bytes, limits_.max_frame_bytes);
    limits_.frame_deadline =
        std::max(limits_.frame_deadline, std::chrono::milliseconds(1));
}

void FrameReassembler::reset(std::uint32_t stream_id) noexcept {
    stream_id_ = stream_id;
    stream_set_ = true;
    frames_.clear();
    completed_order_.clear();
    completed_recent_.clear();
    stats_ = {};
}

ReassemblyResult FrameReassembler::push(
    const protocol::VideoPacketHeader& header,
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point now) {
    expire(now);
    if (!stream_set_) {
        return {FragmentDisposition::uninitialized, std::nullopt};
    }
    if (header.stream_id != stream_id_) {
        return {FragmentDisposition::wrong_stream, std::nullopt};
    }
    const bool end_marker =
        has_flag(header.flags, protocol::VideoFlags::end_of_frame);
    const bool is_last_fragment =
        header.fragment_count != 0 &&
        header.fragment_index == header.fragment_count - 1;
    if (header.version != protocol::kVideoVersion ||
        header.fragment_count == 0 ||
        header.fragment_count > limits_.max_fragments_per_frame ||
        header.fragment_index >= header.fragment_count ||
        header.payload_bytes != payload.size() ||
        payload.empty() ||
        payload.size() > limits_.max_fragment_payload ||
        end_marker != is_last_fragment) {
        ++stats_.invalid_fragments;
        return {FragmentDisposition::invalid, std::nullopt};
    }
    if (completed_recent_.contains(header.frame_id)) {
        ++stats_.late_completed_frame_fragments;
        return {FragmentDisposition::late_completed_frame, std::nullopt};
    }

    auto found = frames_.find(header.frame_id);
    if (found == frames_.end()) {
        if (frames_.size() >= limits_.max_frames_in_flight) {
            ++stats_.resource_limit_rejections;
            return {FragmentDisposition::resource_limit, std::nullopt};
        }
        PartialFrame partial;
        partial.fragment_count = header.fragment_count;
        partial.capture_time_100ns = header.capture_time_100ns;
        partial.keyframe =
            has_flag(header.flags, protocol::VideoFlags::keyframe);
        partial.deadline = now + limits_.frame_deadline;
        partial.fragments.resize(header.fragment_count);
        partial.received.resize(header.fragment_count, std::uint8_t{0});
        found = frames_.emplace(header.frame_id, std::move(partial)).first;
    }

    auto& partial = found->second;
    if (partial.fragment_count != header.fragment_count ||
        partial.capture_time_100ns != header.capture_time_100ns) {
        ++stats_.conflicting_fragments;
        return {FragmentDisposition::invalid, std::nullopt};
    }
    const auto fragment_index = static_cast<std::size_t>(header.fragment_index);
    if (partial.received[fragment_index] != 0) {
        const auto& existing = partial.fragments[fragment_index];
        if (existing.size() == payload.size() &&
            std::equal(existing.begin(), existing.end(), payload.begin())) {
            ++stats_.duplicate_fragments;
            return {FragmentDisposition::duplicate, std::nullopt};
        }
        ++stats_.conflicting_fragments;
        return {FragmentDisposition::invalid, std::nullopt};
    }
    if (payload.size() > limits_.max_frame_bytes -
                             std::min(partial.total_bytes,
                                      limits_.max_frame_bytes) ||
        payload.size() > limits_.max_total_buffered_bytes -
                             std::min(stats_.buffered_bytes,
                                      limits_.max_total_buffered_bytes)) {
        drop_frame(found, true);
        return {FragmentDisposition::resource_limit, std::nullopt};
    }

    partial.fragments[fragment_index].assign(payload.begin(), payload.end());
    partial.received[fragment_index] = 1;
    ++partial.received_count;
    partial.total_bytes += payload.size();
    partial.keyframe = partial.keyframe ||
        has_flag(header.flags, protocol::VideoFlags::keyframe);
    ++stats_.accepted_fragments;
    stats_.buffered_bytes += payload.size();
    stats_.peak_buffered_bytes =
        std::max(stats_.peak_buffered_bytes, stats_.buffered_bytes);

    if (partial.received_count != partial.fragment_count) {
        return {FragmentDisposition::accepted, std::nullopt};
    }
    CompletedFrame complete;
    complete.stream_id = stream_id_;
    complete.frame_id = header.frame_id;
    complete.capture_time_100ns = partial.capture_time_100ns;
    complete.keyframe = partial.keyframe;
    complete.payload.reserve(partial.total_bytes);
    for (const auto& fragment : partial.fragments) {
        complete.payload.insert(complete.payload.end(), fragment.begin(),
                                fragment.end());
    }
    stats_.buffered_bytes -= partial.total_bytes;
    frames_.erase(found);
    ++stats_.completed_frames;
    remember_completed(header.frame_id);
    return {FragmentDisposition::frame_complete, std::move(complete)};
}

std::size_t FrameReassembler::expire(
    std::chrono::steady_clock::time_point now) noexcept {
    std::size_t expired = 0;
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (it->second.deadline > now) {
            ++it;
            continue;
        }
        stats_.buffered_bytes -= it->second.total_bytes;
        it = frames_.erase(it);
        ++stats_.expired_incomplete_frames;
        ++expired;
    }
    return expired;
}

std::optional<std::vector<std::uint16_t>> FrameReassembler::missing_fragments(
    std::uint64_t frame_id) const {
    const auto found = frames_.find(frame_id);
    if (found == frames_.end()) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> missing;
    missing.reserve(found->second.fragment_count - found->second.received_count);
    for (std::uint16_t index = 0; index < found->second.fragment_count; ++index) {
        if (found->second.received[index] == 0) {
            missing.push_back(index);
        }
    }
    return missing;
}

const FrameReassemblyStats& FrameReassembler::stats() const noexcept {
    return stats_;
}

std::size_t FrameReassembler::frames_in_flight() const noexcept {
    return frames_.size();
}

void FrameReassembler::remember_completed(std::uint64_t frame_id) {
    constexpr std::size_t recent_capacity = 64;
    completed_recent_.insert(frame_id);
    completed_order_.push_back(frame_id);
    while (completed_order_.size() > recent_capacity) {
        completed_recent_.erase(completed_order_.front());
        completed_order_.pop_front();
    }
}

void FrameReassembler::drop_frame(
    std::unordered_map<std::uint64_t, PartialFrame>::iterator it,
    bool resource_drop) noexcept {
    stats_.buffered_bytes -= it->second.total_bytes;
    frames_.erase(it);
    if (resource_drop) {
        ++stats_.resource_dropped_frames;
    }
}

} // namespace nstu::video
