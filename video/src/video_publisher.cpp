#include "nstu/video_publisher.hpp"

#include <algorithm>
#include <utility>

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool VideoMulticastPublisher::open(
    VideoMulticastPublisherConfig config,
    std::span<const std::byte> authentication_key, std::string* error) {
    close();
    if (!winsock_.ready()) {
        set_error(error, "Winsock initialization failed");
        return false;
    }
    if (config.stream_id == 0 || authentication_key.empty()) {
        set_error(error, "invalid multicast publisher configuration");
        return false;
    }
    if (!socket_.open_sender(config.multicast, error)) {
        return false;
    }
    packetizer_ = VideoPacketizer(config.packetizer);
    packetizer_.reset(config.stream_id, config.first_packet_sequence,
                      config.first_frame_id);
    authentication_key_.assign(authentication_key.begin(), authentication_key.end());
    open_ = true;
    return true;
}

bool VideoMulticastPublisher::publish(std::span<const std::byte> access_unit,
                                      std::uint64_t capture_time_100ns,
                                      bool keyframe, std::string* error) {
    if (!open_) {
        set_error(error, "multicast publisher is not open");
        return false;
    }
    if (access_unit.empty()) {
        set_error(error, "encoded access unit is empty");
        return false;
    }
    const auto datagrams = packetizer_.packetize(
        access_unit, capture_time_100ns, keyframe, authentication_key_, error);
    if (!datagrams) {
        return false;
    }
    for (const auto& datagram : *datagrams) {
        const auto wire = datagram.wire();
        if (socket_.send(wire, error) !=
            static_cast<int>(wire.size())) {
            if (error != nullptr && error->empty()) {
                *error = "multicast datagram send was incomplete";
            }
            return false;
        }
    }
    return true;
}

void VideoMulticastPublisher::close() noexcept {
    socket_.close();
    std::fill(authentication_key_.begin(), authentication_key_.end(),
              std::byte{0});
    authentication_key_.clear();
    open_ = false;
}

bool VideoMulticastPublisher::is_open() const noexcept {
    return open_ && socket_.is_open();
}

} // namespace nstu::video
