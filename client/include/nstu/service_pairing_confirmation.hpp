#pragma once

#include "nstu/agent_protocol.hpp"
#include "nstu/client_pairing.hpp"
#include "nstu/agent_pairing_confirmation.hpp"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <deque>

namespace nstu::client {

// Issued only after the service validates its installed agent image and WTS
// session on this exact pipe connection. A generation cannot be transferred
// to a replacement connection, even when it belongs to the same WTS session.
struct PairingAgentPeer {
    std::uint32_t session_id = 0;
    std::uint64_t connection_generation = 0;
    bool operator==(const PairingAgentPeer&) const = default;
};

class ServicePairingConfirmation {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] PairingAgentPeer connect_agent(std::uint32_t validated_session_id);
    void disconnect_agent(PairingAgentPeer peer);
    [[nodiscard]] PairingAgentPeer peer() const;
    [[nodiscard]] std::optional<AgentPairingConfirmationRequest> begin_attempt(
        PairingAgentPeer peer, std::string_view code, std::string_view server_name,
        // A confirmed attempt relays later broker invalidation to the existing
        // transport stop token, outside locks. No network command is added.
        std::chrono::milliseconds timeout, std::stop_source transport_stop = {});
    [[nodiscard]] bool receive_reply(PairingAgentPeer peer, std::span<const std::byte> payload);
    [[nodiscard]] StudentConfirmation wait(const AgentPairingAttemptToken& token,
                                          Clock::time_point slice_deadline,
                                          std::stop_token stop_token);
    void cancel_attempt(const AgentPairingAttemptToken& token);
    [[nodiscard]] bool end_attempt(const AgentPairingAttemptToken& token);
    // Atomically consume the still-current decision before handing credentials
    // back to the service. Cancel/disconnect/supersession can revoke it while
    // the existing cryptographic exchange waits for teacher approval.
    [[nodiscard]] StudentConfirmation finish_attempt(const AgentPairingAttemptToken& token,
                                                     std::stop_token stop_token = {});
    [[nodiscard]] bool is_current_request(PairingAgentPeer peer,
                                          const AgentPairingAttemptToken& token) const;
private:
    struct Attempt {
        AgentPairingConfirmationRequest request;
        PairingAgentPeer peer;
        Clock::time_point deadline;
        std::optional<StudentConfirmation> decision;
        std::stop_source transport_stop;
    };
    mutable std::mutex mutex_;
    std::condition_variable signal_;
    PairingAgentPeer peer_;
    std::uint64_t generation_ = 0;
    std::uint64_t attempt_generation_ = 0;
    std::optional<Attempt> attempt_;
    [[nodiscard]] std::optional<std::stop_source> confirmed_transport_stop_locked() const;
};

using PairingAgentMessageSink = std::function<bool(AgentMessage)>;

// Callers serialize the queue and model with their existing UI/pipe mutexes.
// Pairing traffic alone is removed; exam answers and telemetry keep their order.
void discard_pairing_confirmation_messages(std::deque<AgentMessage>& queue,
    const std::optional<AgentPairingAttemptToken>& token = std::nullopt);
[[nodiscard]] bool pairing_agent_message_current(const ServicePairingConfirmation& broker,
    PairingAgentPeer peer, const AgentMessage& message);
[[nodiscard]] bool enqueue_pairing_agent_message(const ServicePairingConfirmation& broker,
    PairingAgentPeer peer, std::deque<AgentMessage>& queue, std::size_t capacity,
    AgentMessage message);

struct PairingNativeControls {
    HWND window = nullptr;
    HWND teacher_input = nullptr;
    HWND match = nullptr;
    HWND differ = nullptr;
    HWND reject = nullptr;
    std::optional<AgentPairingAttemptToken> displayed_token;
};

// UI-thread only. Unbind first; bind the new token only after a successful
// native reset and actual empty text on the still-live control.
[[nodiscard]] bool reset_pairing_native_input(HWND teacher_input,
    const AgentPairingAttemptToken& token,
    std::optional<AgentPairingAttemptToken>& displayed_token);

// Shared by the real Win32 window and regression harness. Receipt and a posted
// notification cannot acknowledge presentation. The actual successful paint,
// current SAS and visible usable native controls are all required. Approval
// reads the native edit control, never the displayed or computed SAS.
class AgentPairingNativeConfirmation {
public:
    [[nodiscard]] bool receive(const AgentPairingConfirmationRequest& request, std::uint64_t now_ms);
    [[nodiscard]] bool present(const PairingNativeControls& controls,
        const AgentPairingAttemptToken& token, std::string_view rendered_code,
        bool paint_succeeded, std::uint64_t now_ms);
    [[nodiscard]] std::optional<AgentPairingConfirmationReply> choose(
        const PairingNativeControls& controls, const AgentPairingAttemptToken& token,
        AgentPairingConfirmationChoice choice, std::uint64_t now_ms);
    [[nodiscard]] std::optional<AgentPairingConfirmationReply> expire(std::uint64_t now_ms);
    [[nodiscard]] bool end(const AgentPairingAttemptToken& token) noexcept;
    void disconnect() noexcept;
    [[nodiscard]] const std::optional<AgentPairingConfirmationRequest>& request() const noexcept {
        return visible_request_;
    }
    [[nodiscard]] bool awaiting_teacher() const noexcept { return confirmed_; }
private:
    AgentPairingConfirmation model_;
    std::optional<AgentPairingConfirmationRequest> visible_request_;
    bool confirmed_ = false;
};

[[nodiscard]] StudentConfirmation finish_interactive_confirmation(
    ServicePairingConfirmation& broker, const AgentPairingAttemptToken& token,
    const PairingAgentMessageSink& sink, std::stop_token stop_token);

// The normal service must invoke this seam, which owns construction of the
// real student callback. It never persists credentials or changes teacher
// approval. The sink is called outside broker locks and must enqueue promptly.
[[nodiscard]] PairingAttemptResult pair_with_interactive_agent(
    const discovery::PairingCandidate& candidate, std::string_view client_uuid,
    std::string_view hostname, ServicePairingConfirmation& broker,
    const PairingAgentMessageSink& sink, std::stop_token stop_token,
    const PairingAttemptOptions& timing = {}, std::string* error = nullptr);

} // namespace nstu::client
