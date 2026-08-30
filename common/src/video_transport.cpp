#include "nstu/video_transport.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset + sizeof(T) > input.size()) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

} // namespace

std::vector<std::byte> VideoDatagram::wire() const {
    if (header.payload_bytes != payload.size()) {
        return {};
    }
    const auto header_wire = protocol::encode_video_header(header);
    if (header_wire.empty()) {
        return {};
    }
    std::vector<std::byte> output;
    output.reserve(header_wire.size() + tag.size() + payload.size());
    output.insert(output.end(), header_wire.begin(), header_wire.end());
    output.insert(output.end(), tag.begin(), tag.end());
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

VideoPacketizer::VideoPacketizer(PacketizerConfig config) : config_(config) {
    const auto minimum = protocol::kVideoHeaderBytes +
                         security::kVideoAuthTagBytes + 1;
    config_.maximum_datagram_bytes = std::clamp<std::size_t>(
        config_.maximum_datagram_bytes, minimum, 65'507);
    config_.maximum_fragments = std::clamp<std::uint16_t>(
        config_.maximum_fragments, 1, 16'384);
}

void VideoPacketizer::reset(std::uint32_t stream_id,
                            std::uint64_t first_packet_sequence,
                            std::uint64_t first_frame_id) noexcept {
    stream_id_ = stream_id;
    next_packet_sequence_ = first_packet_sequence;
    next_frame_id_ = first_frame_id;
    initialized_ = stream_id != 0;
}

std::optional<std::vector<VideoDatagram>> VideoPacketizer::packetize(
    std::span<const std::byte> access_unit,
    std::uint64_t capture_time_100ns, bool keyframe,
    std::span<const std::byte> authentication_key,
    std::string* error) {
    if (!initialized_ || access_unit.empty() ||
        authentication_key.size() < security::kMinimumProtocolKeyBytes) {
        set_error(error, "invalid packetizer input");
        return std::nullopt;
    }
    const std::size_t payload_limit = config_.maximum_datagram_bytes -
        protocol::kVideoHeaderBytes - security::kVideoAuthTagBytes;
    const std::size_t fragment_count =
        (access_unit.size() + payload_limit - 1) / payload_limit;
    if (fragment_count == 0 || fragment_count > config_.maximum_fragments ||
        fragment_count > std::numeric_limits<std::uint16_t>::max() ||
        fragment_count - 1 >
            std::numeric_limits<std::uint64_t>::max() - next_packet_sequence_) {
        set_error(error, "access unit exceeds packetizer limits");
        return std::nullopt;
    }
    if (next_frame_id_ == std::numeric_limits<std::uint64_t>::max()) {
        set_error(error, "frame ID space is exhausted");
        return std::nullopt;
    }

    std::vector<VideoDatagram> datagrams;
    datagrams.reserve(fragment_count);
    std::size_t offset = 0;
    for (std::size_t index = 0; index < fragment_count; ++index) {
        const auto bytes = std::min(payload_limit, access_unit.size() - offset);
        VideoDatagram datagram;
        datagram.header.stream_id = stream_id_;
        datagram.header.packet_sequence = next_packet_sequence_ + index;
        datagram.header.frame_id = next_frame_id_;
        datagram.header.fragment_index = static_cast<std::uint16_t>(index);
        datagram.header.fragment_count =
            static_cast<std::uint16_t>(fragment_count);
        datagram.header.payload_bytes = static_cast<std::uint32_t>(bytes);
        datagram.header.capture_time_100ns = capture_time_100ns;
        auto flags = keyframe ? protocol::VideoFlags::keyframe
                              : protocol::VideoFlags::none;
        if (index + 1 == fragment_count) {
            flags = static_cast<protocol::VideoFlags>(
                static_cast<std::uint16_t>(flags) |
                static_cast<std::uint16_t>(protocol::VideoFlags::end_of_frame));
        }
        datagram.header.flags = flags;
        datagram.payload.assign(
            access_unit.begin() + static_cast<std::ptrdiff_t>(offset),
            access_unit.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
        const auto tag = security::compute_video_auth_tag(
            authentication_key, datagram.header, datagram.payload);
        if (!tag) {
            set_error(error, "video authentication tag generation failed");
            return std::nullopt;
        }
        datagram.tag = *tag;
        datagrams.push_back(std::move(datagram));
        offset += bytes;
    }
    next_packet_sequence_ += fragment_count;
    ++next_frame_id_;
    return datagrams;
}

std::uint64_t VideoPacketizer::next_packet_sequence() const noexcept {
    return next_packet_sequence_;
}

std::uint64_t VideoPacketizer::next_frame_id() const noexcept {
    return next_frame_id_;
}

std::optional<VideoDatagram> decode_video_datagram(
    std::span<const std::byte> wire,
    std::span<const std::byte> authentication_key,
    std::string* error) {
    const auto overhead = protocol::kVideoHeaderBytes +
                          security::kVideoAuthTagBytes;
    if (wire.size() <= overhead || wire.size() > 65'507) {
        set_error(error, "invalid video datagram length");
        return std::nullopt;
    }
    const auto header = protocol::decode_video_header(
        wire.first(protocol::kVideoHeaderBytes));
    if (!header || header->payload_bytes != wire.size() - overhead) {
        set_error(error, "invalid video datagram header");
        return std::nullopt;
    }
    const auto tag = wire.subspan(protocol::kVideoHeaderBytes,
                                  security::kVideoAuthTagBytes);
    const auto payload = wire.subspan(overhead);
    if (!security::verify_video_auth_tag(authentication_key, *header, payload,
                                         tag)) {
        set_error(error, "video datagram authentication failed");
        return std::nullopt;
    }
    VideoDatagram datagram;
    datagram.header = *header;
    std::copy(tag.begin(), tag.end(), datagram.tag.begin());
    datagram.payload.assign(payload.begin(), payload.end());
    return datagram;
}

FrameJitterBuffer::FrameJitterBuffer(JitterBufferConfig config)
    : config_(config) {
    config_.target_delay = std::clamp(config_.target_delay,
                                      std::chrono::milliseconds(1),
                                      std::chrono::milliseconds(1000));
    config_.maximum_frames = std::clamp<std::size_t>(config_.maximum_frames,
                                                     2, 256);
}

void FrameJitterBuffer::reset(std::uint32_t stream_id) noexcept {
    stream_id_ = stream_id;
    stream_set_ = stream_id != 0;
    last_released_frame_id_.reset();
    frames_.clear();
    stats_ = {};
}

bool FrameJitterBuffer::push(
    CompletedFrame frame,
    std::chrono::steady_clock::time_point arrival_time) noexcept {
    if (!stream_set_ || frame.stream_id != stream_id_) {
        return false;
    }
    if (last_released_frame_id_ && frame.frame_id <= *last_released_frame_id_) {
        ++stats_.late;
        return false;
    }
    if (frames_.contains(frame.frame_id)) {
        ++stats_.duplicate;
        return false;
    }
    if (frames_.size() >= config_.maximum_frames) {
        frames_.erase(frames_.begin());
        ++stats_.resource_dropped;
    }
    frames_.emplace(frame.frame_id,
                    BufferedFrame{std::move(frame),
                                  arrival_time + config_.target_delay});
    ++stats_.accepted;
    return true;
}

std::optional<CompletedFrame> FrameJitterBuffer::pop_ready(
    std::chrono::steady_clock::time_point now) noexcept {
    if (frames_.empty() || frames_.begin()->second.ready_at > now) {
        return std::nullopt;
    }
    auto frame = std::move(frames_.begin()->second.frame);
    frames_.erase(frames_.begin());
    last_released_frame_id_ = frame.frame_id;
    ++stats_.released;
    return frame;
}

std::size_t FrameJitterBuffer::size() const noexcept { return frames_.size(); }

const JitterBufferStats& FrameJitterBuffer::stats() const noexcept {
    return stats_;
}

NackPolicy::NackPolicy(NackPolicyConfig config) : config_(config) {
    config_.initial_delay = std::max(config_.initial_delay,
                                     std::chrono::milliseconds(1));
    config_.retry_interval = std::max(config_.retry_interval,
                                      std::chrono::milliseconds(1));
    config_.maximum_attempts = std::clamp<std::uint8_t>(
        config_.maximum_attempts, 1, 8);
    config_.maximum_fragments_per_request = std::clamp<std::size_t>(
        config_.maximum_fragments_per_request, 1, 1024);
}

void NackPolicy::reset(std::uint32_t stream_id) noexcept {
    stream_id_ = stream_id;
    stream_set_ = stream_id != 0;
    states_.clear();
}

std::optional<VideoNack> NackPolicy::consider(
    std::uint64_t frame_id, std::span<const std::uint16_t> missing,
    std::chrono::steady_clock::time_point first_fragment_time,
    std::chrono::steady_clock::time_point frame_deadline,
    std::chrono::steady_clock::time_point now) {
    if (!stream_set_ || missing.empty() || now >= frame_deadline) {
        return std::nullopt;
    }
    auto [found, inserted] = states_.try_emplace(
        frame_id, State{0, first_fragment_time + config_.initial_delay,
                        frame_deadline});
    auto& state = found->second;
    if (inserted && state.next_request >= state.deadline) {
        state.attempts = config_.maximum_attempts;
        return std::nullopt;
    }
    if (state.attempts >= config_.maximum_attempts ||
        now < state.next_request || now >= state.deadline) {
        return std::nullopt;
    }
    ++state.attempts;
    state.next_request = now + config_.retry_interval;
    VideoNack nack;
    nack.stream_id = stream_id_;
    nack.frame_id = frame_id;
    nack.attempt = state.attempts;
    const auto count = std::min(missing.size(),
                                config_.maximum_fragments_per_request);
    nack.missing_fragments.assign(missing.begin(), missing.begin() + count);
    return nack;
}

void NackPolicy::complete(std::uint64_t frame_id) noexcept {
    states_.erase(frame_id);
}

bool NackPolicy::exhausted(std::uint64_t frame_id) const noexcept {
    const auto found = states_.find(frame_id);
    return found != states_.end() &&
           found->second.attempts >= config_.maximum_attempts;
}

std::size_t NackPolicy::expire(
    std::chrono::steady_clock::time_point now) noexcept {
    std::size_t expired = 0;
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->second.deadline <= now) {
            it = states_.erase(it);
            ++expired;
        } else {
            ++it;
        }
    }
    return expired;
}

std::vector<std::byte> encode_video_nack(const VideoNack& nack) {
    if (nack.stream_id == 0 || nack.attempt == 0 ||
        nack.missing_fragments.empty() ||
        nack.missing_fragments.size() > 1024) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(sizeof(nack.stream_id) + sizeof(nack.frame_id) +
                    sizeof(nack.attempt) + sizeof(std::uint16_t) +
                    nack.missing_fragments.size() * sizeof(std::uint16_t));
    append_le(payload, nack.stream_id);
    append_le(payload, nack.frame_id);
    append_le(payload, nack.attempt);
    append_le(payload,
              static_cast<std::uint16_t>(nack.missing_fragments.size()));
    for (const auto fragment : nack.missing_fragments) {
        append_le(payload, fragment);
    }
    return payload;
}

std::optional<VideoNack> decode_video_nack(
    std::span<const std::byte> payload) {
    VideoNack nack;
    std::size_t offset = 0;
    std::uint16_t count = 0;
    if (!read_le(payload, offset, nack.stream_id) ||
        !read_le(payload, offset, nack.frame_id) ||
        !read_le(payload, offset, nack.attempt) ||
        !read_le(payload, offset, count) || nack.stream_id == 0 ||
        nack.attempt == 0 || count == 0 || count > 1024 ||
        payload.size() - offset != count * sizeof(std::uint16_t)) {
        return std::nullopt;
    }
    nack.missing_fragments.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint16_t fragment = 0;
        if (!read_le(payload, offset, fragment)) {
            return std::nullopt;
        }
        nack.missing_fragments.push_back(fragment);
    }
    return nack;
}

KeyframeScheduler::KeyframeScheduler(KeyframeScheduleConfig config)
    : config_(config) {
    config_.minimum_interval = std::max(config_.minimum_interval,
                                        std::chrono::milliseconds(1));
    config_.maximum_interval = std::max(config_.maximum_interval,
                                        config_.minimum_interval);
    config_.loss_threshold_per_mille = std::clamp(
        config_.loss_threshold_per_mille, 1u, 1000u);
}

void KeyframeScheduler::reset(
    std::chrono::steady_clock::time_point now) noexcept {
    last_keyframe_ = now - config_.maximum_interval;
    pending_reason_ = KeyframeReason::stream_start;
    initialized_ = true;
}

void KeyframeScheduler::observe_loss(std::uint32_t loss_per_mille) noexcept {
    if (loss_per_mille >= config_.loss_threshold_per_mille) {
        pending_reason_ = KeyframeReason::packet_loss;
    }
}

void KeyframeScheduler::notify_nack_exhausted() noexcept {
    pending_reason_ = KeyframeReason::nack_exhausted;
}

void KeyframeScheduler::notify_decoder_recovery() noexcept {
    pending_reason_ = KeyframeReason::decoder_recovery;
}

std::optional<KeyframeReason> KeyframeScheduler::due(
    std::chrono::steady_clock::time_point now) const noexcept {
    if (!initialized_ || now - last_keyframe_ < config_.minimum_interval) {
        return std::nullopt;
    }
    if (pending_reason_) {
        return pending_reason_;
    }
    if (now - last_keyframe_ >= config_.maximum_interval) {
        return KeyframeReason::periodic;
    }
    return std::nullopt;
}

void KeyframeScheduler::mark_emitted(
    std::chrono::steady_clock::time_point now) noexcept {
    last_keyframe_ = now;
    pending_reason_.reset();
}

} // namespace nstu::video
