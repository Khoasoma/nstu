#pragma once

#include "nstu/agent_protocol.hpp"

#include <optional>
#include <string_view>

namespace nstu::client {

inline constexpr std::uint64_t kMaximumPairingConfirmationMs = 180000;

// Caller serializes access between the pipe and UI threads. Receipt does not
// prove presentation: the actual UI must acknowledge the current token after
// displaying the SAS and creating usable controls. No methods perform I/O.
class AgentPairingConfirmation {
public:
    [[nodiscard]] bool receive(const AgentPairingConfirmationRequest& request,
                               std::uint64_t now_ms);
    [[nodiscard]] bool mark_presented(const AgentPairingAttemptToken& token,
                                      std::uint64_t now_ms) noexcept;
    [[nodiscard]] std::optional<AgentPairingConfirmationReply> choose(
        const AgentPairingAttemptToken& token, AgentPairingConfirmationChoice choice,
        std::string_view entered_teacher_code, std::uint64_t now_ms);
    [[nodiscard]] std::optional<AgentPairingConfirmationReply> expire(std::uint64_t now_ms);
    [[nodiscard]] bool end(const AgentPairingAttemptToken& token) noexcept;
    void disconnect() noexcept;
    [[nodiscard]] const std::optional<AgentPairingConfirmationRequest>& request() const noexcept {
        return request_;
    }
private:
    std::optional<AgentPairingConfirmationRequest> request_;
    bool presented_ = false;
};

} // namespace nstu::client
