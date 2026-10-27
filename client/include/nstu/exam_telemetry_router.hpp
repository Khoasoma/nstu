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

    void handle_explicit_stop();
    void handle_disconnect();
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
