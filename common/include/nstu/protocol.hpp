#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::protocol {

inline constexpr std::uint32_t kMagic = 0x4E535455; // "NSTU"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kCommandHeaderBytes = 20;
inline constexpr std::size_t kVideoHeaderBytes = 36;
inline constexpr std::uint32_t kMaxCommandPayload = 64u * 1024u;
inline constexpr std::size_t kMaxTcpBufferedBytes =
    4u * (kCommandHeaderBytes + kMaxCommandPayload);

enum class CommandType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    heartbeat = 3,
    lock = 4,
    unlock = 5,
    chat = 6,
    keyframe_request = 7,
};

enum class VideoFlags : std::uint16_t {
    none = 0,
    keyframe = 1 << 0,
    end_of_frame = 1 << 1,
};

struct CommandEnvelope {
    std::uint16_t version = kVersion;
    CommandType type = CommandType::hello;
    std::uint32_t payload_bytes = 0;
    std::uint64_t request_id = 0;
};

struct VideoPacketHeader {
    std::uint16_t version = kVersion;
    VideoFlags flags = VideoFlags::none;
    std::uint32_t stream_id = 0;
    std::uint64_t frame_id = 0;
    std::uint16_t fragment_index = 0;
    std::uint16_t fragment_count = 0;
    std::uint32_t payload_bytes = 0;
    std::uint64_t capture_time_100ns = 0;
};

[[nodiscard]] std::vector<std::byte> encode_command_header(
    const CommandEnvelope& envelope);

[[nodiscard]] std::optional<CommandEnvelope> decode_command_header(
    std::span<const std::byte> wire);

[[nodiscard]] std::vector<std::byte> encode_video_header(
    const VideoPacketHeader& header);

[[nodiscard]] std::optional<VideoPacketHeader> decode_video_header(
    std::span<const std::byte> wire);

struct TcpFrame {
    CommandEnvelope envelope;
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encode_tcp_frame(
    const CommandEnvelope& envelope, std::span<const std::byte> payload);

class TcpFrameParser {
public:
    // Appends bytes and emits every complete frame currently in the buffer.
    // Returns false if a framing or payload limit is violated.
    bool feed(std::span<const std::byte> bytes, std::vector<TcpFrame>& frames);
    void reset() noexcept;

private:
    std::vector<std::byte> buffer_;
};

} // namespace nstu::protocol
