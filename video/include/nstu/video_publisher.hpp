#pragma once

#include "nstu/multicast.hpp"
#include "nstu/video_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nstu::video {

struct VideoMulticastPublisherConfig {
    net::MulticastConfig multicast{};
    PacketizerConfig packetizer{};
    std::uint32_t stream_id = 1;
    std::uint64_t first_packet_sequence = 0;
    std::uint64_t first_frame_id = 0;
};

// Publishes authenticated H.264 access units through the existing UDP
// multicast transport. The access unit remains in caller-owned memory until
// packetization completes; no staging copy is retained between calls.
class VideoMulticastPublisher {
public:
    VideoMulticastPublisher() = default;
    ~VideoMulticastPublisher() = default;
    VideoMulticastPublisher(const VideoMulticastPublisher&) = delete;
    VideoMulticastPublisher& operator=(const VideoMulticastPublisher&) = delete;

    [[nodiscard]] bool open(VideoMulticastPublisherConfig config,
                            std::span<const std::byte> authentication_key,
                            std::string* error = nullptr);
    [[nodiscard]] bool publish(std::span<const std::byte> access_unit,
                               std::uint64_t capture_time_100ns,
                               bool keyframe,
                               std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    net::WinsockRuntime winsock_;
    net::UdpMulticastSocket socket_;
    VideoPacketizer packetizer_;
    std::vector<std::byte> authentication_key_;
    bool open_ = false;
};

} // namespace nstu::video
