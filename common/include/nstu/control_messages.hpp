#pragma once

#include "nstu/auth.hpp"
#include "nstu/network.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::control {

struct ClientStatusReport {
    std::string hostname;
    bool locked = false;
    bool streaming = false;
    std::uint32_t session_id = 0;
    std::uint32_t latency_ms = 0;
    std::uint32_t packet_loss_per_mille = 0;
    std::uint64_t packet_loss_sample_size = 0;
    net::VideoDeliveryMode delivery = net::VideoDeliveryMode::multicast;
};

struct VideoGroupKeyMessage {
    std::uint32_t stream_id = 0;
    std::uint64_t first_packet_sequence = 0;
    security::Sha256Digest key{};
};

[[nodiscard]] std::vector<std::byte> encode_status_report(
    const ClientStatusReport& report);
[[nodiscard]] std::optional<ClientStatusReport> decode_status_report(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_start_stream_request(
    std::uint8_t frames_per_second);
[[nodiscard]] std::optional<std::uint8_t> decode_start_stream_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_video_group_key(
    const VideoGroupKeyMessage& message);
[[nodiscard]] std::optional<VideoGroupKeyMessage> decode_video_group_key(
    std::span<const std::byte> payload);

} // namespace nstu::control
