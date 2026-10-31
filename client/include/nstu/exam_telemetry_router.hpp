#pragma once

#include "nstu/agent_protocol.hpp"
#include "nstu/client_control.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_telemetry.hpp"
#include "nstu/exam_telemetry_lifecycle.hpp"
#include "nstu/exam_telemetry_service.hpp"
#include "nstu/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::client {

// Dispatch identifies callback invocation; the outcome describes stop and cleanup facts.
enum class TelemetryStopDispatch : std::uint8_t {
    not_configured, legacy_callback_invoked, outcome_callback_invoked
};

struct TelemetryStopDispatchResult {
    TelemetryStopDispatch dispatch = TelemetryStopDispatch::not_configured;
    std::optional<TelemetryStopOutcome> outcome = std::nullopt;
};

// Dispatch reports invocation; the outcome preserves the coordinator's current disconnect facts.
enum class TelemetryDisconnectDispatch : std::uint8_t {
    not_configured, legacy_callback_invoked, outcome_callback_invoked
};

struct TelemetryDisconnectDispatchResult {
    TelemetryDisconnectDispatch dispatch = TelemetryDisconnectDispatch::not_configured;
    std::optional<TelemetryDisconnectOutcome> outcome = std::nullopt;
};

// Two ordered operation snapshots; separate coordinator locks allow interleaving.
// Neither a transaction nor a shared context/merged publication is implied.
struct TelemetryShutdownCloseOutcome {
    TelemetryShutdownOutcome shutdown{};
    TelemetryCloseOutcome close{};
};

enum class TelemetryShutdownDispatch : std::uint8_t {
    not_configured, legacy_callback_invoked, outcome_callback_invoked
};

struct TelemetryShutdownDispatchResult {
    TelemetryShutdownDispatch dispatch = TelemetryShutdownDispatch::not_configured;
    std::optional<TelemetryShutdownCloseOutcome> outcome = std::nullopt;
};

struct ExamTelemetryRouterCallbacks {
    std::function<bool(const exam::TelemetryEnable& enable, std::string* error)> on_enable;
    std::function<bool(const std::vector<exam::TelemetrySample>& samples, std::uint64_t now_ms, std::string* error)> ingest_samples;
    std::function<exam::TelemetryAckApplyResult(const exam::TelemetryAck& ack, std::uint64_t now_ms, std::string* error)> on_ack;
    std::function<void()> on_explicit_stop;
    std::function<void()> on_disconnect;
    std::function<void()> on_shutdown;
    std::function<bool(const std::filesystem::path& outbox_path, std::string* error)> on_restart;
    std::function<bool(std::uint64_t now_ms, std::string* error)> on_maintenance;
    std::function<void(std::span<const std::byte> enable_payload)> queue_agent_enable;
    std::function<std::optional<exam::TelemetryBatch>(std::uint64_t now_ms, std::uint64_t timeout_ms)> next_batch_to_send;
    std::function<TelemetryStopOutcome()> on_explicit_stop_with_outcome = {};
    std::function<TelemetryDisconnectOutcome()> on_disconnect_with_outcome = {};
    std::function<TelemetryShutdownCloseOutcome()> on_shutdown_with_outcome = {};
};

class ExamTelemetryRouter {
public:
    ExamTelemetryRouter() = default;
    explicit ExamTelemetryRouter(ExamTelemetryRouterCallbacks callbacks);
    ExamTelemetryRouter(ExamTelemetryServiceCoordinator& coordinator,
                        std::function<void(std::span<const std::byte>)> queue_agent_enable);

    ~ExamTelemetryRouter() = default;
    ExamTelemetryRouter(ExamTelemetryRouter&&) noexcept = default;
    ExamTelemetryRouter& operator=(ExamTelemetryRouter&&) noexcept = default;
    ExamTelemetryRouter(const ExamTelemetryRouter&) = delete;
    ExamTelemetryRouter& operator=(const ExamTelemetryRouter&) = delete;

    void set_callbacks(ExamTelemetryRouterCallbacks callbacks);

    bool handle_server_enable(std::span<const std::byte> payload, std::string* error = nullptr);
    bool handle_server_enable(const exam::TelemetryEnable& enable,
                              std::span<const std::byte> raw_payload,
                              std::string* error = nullptr);

    bool handle_agent_samples(std::span<const std::byte> payload,
                              std::uint64_t now_unix_ms,
                              std::string* error = nullptr);
    bool handle_agent_samples(const std::vector<exam::TelemetrySample>& samples,
                              std::uint64_t now_unix_ms,
                              std::string* error = nullptr);

    exam::TelemetryAckApplyResult handle_server_ack(std::span<const std::byte> payload,
                                                   std::uint64_t now_unix_ms,
                                                   std::string* error = nullptr);
    exam::TelemetryAckApplyResult handle_server_ack(const exam::TelemetryAck& ack,
                                                   std::uint64_t now_unix_ms,
                                                   std::string* error = nullptr);

    // Typed callback takes precedence; absent/legacy callbacks leave the outcome empty.
    // Exceptions propagate. Callback invocation does not guarantee completed cleanup.
    [[nodiscard]] TelemetryStopDispatchResult handle_explicit_stop_with_outcome();
    void handle_explicit_stop();
    // Typed callback takes precedence; absent/legacy callbacks leave the outcome empty.
    // Exceptions propagate. Callback invocation does not guarantee current publication or cleanup.
    [[nodiscard]] TelemetryDisconnectDispatchResult handle_disconnect_with_outcome();
    void handle_disconnect();
    // Typed callback precedes legacy; missing/legacy invocation has no pair.
    // Exceptions propagate, and the two snapshots retain independent facts.
    [[nodiscard]] TelemetryShutdownDispatchResult handle_shutdown_with_outcome();
    void handle_shutdown();
    // Restores the coordinator from its protected local storage. Restore alone
    // never authorizes upload; a matching server enable is still required.
    bool handle_restart(const std::filesystem::path& outbox_path,
                        std::string* error = nullptr);
    // Callable while offline/disabled; propagates durable cleanup failures.
    bool handle_maintenance(std::uint64_t now_unix_ms = 0,
                            std::string* error = nullptr);

    std::optional<ClientOutboundCommand> poll_outbound_batch(
        std::uint64_t now_unix_ms,
        std::uint64_t inflight_timeout_ms = 5000);

private:
    ExamTelemetryRouterCallbacks callbacks_;
};

} // namespace nstu::client
