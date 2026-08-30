#pragma once

#include "nstu/named_pipe.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::client {

enum class AgentMessageType : std::uint16_t {
    lock = 1,
    unlock = 2,
    chat = 3,
    status_request = 4,
    status_report = 5,
    start_stream = 6,
    stop_stream = 7,
    keyframe_request = 8,
};

struct AgentMessage {
    AgentMessageType type = AgentMessageType::status_request;
    std::vector<std::byte> payload;
};

struct AgentStatus {
    bool locked = false;
    bool streaming = false;
    std::uint8_t frames_per_second = 0;
    std::uint32_t session_id = 0;
};

inline constexpr std::size_t kMaximumAgentPayloadBytes = 4096;

[[nodiscard]] std::vector<std::byte> encode_agent_message(
    const AgentMessage& message);
[[nodiscard]] std::optional<AgentMessage> decode_agent_message(
    std::span<const std::byte> wire);
[[nodiscard]] bool send_agent_message(const NamedPipe& pipe,
                                      const AgentMessage& message,
                                      std::string* error = nullptr);
[[nodiscard]] std::optional<AgentMessage> receive_agent_message(
    const NamedPipe& pipe, std::string* error = nullptr);

[[nodiscard]] std::vector<std::byte> encode_agent_status(
    const AgentStatus& status);
[[nodiscard]] std::optional<AgentStatus> decode_agent_status(
    std::span<const std::byte> payload);

} // namespace nstu::client
