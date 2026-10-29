#include "nstu/exam_telemetry_router.hpp"

#include <utility>

namespace nstu::client {

namespace {

inline void set_error(std::string* error, std::string_view message) {
    if (error && error->empty()) {
        *error = std::string(message);
    }
}

} // namespace

ExamTelemetryRouter::ExamTelemetryRouter(ExamTelemetryRouterCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

ExamTelemetryRouter::ExamTelemetryRouter(
    ExamTelemetryServiceCoordinator& coordinator,
    std::function<void(std::span<const std::byte>)> queue_agent_enable)
    : callbacks_{
        .on_enable = [&coordinator](const exam::TelemetryEnable& enable, std::string* err) {
            return coordinator.on_server_enable(enable, err);
        },
        .ingest_samples = [&coordinator](const std::vector<exam::TelemetrySample>& samples, std::uint64_t now_ms, std::string* err) {
            return coordinator.ingest_samples(samples, now_ms, err);
        },
        .on_ack = [&coordinator](const exam::TelemetryAck& ack, std::uint64_t now_ms, std::string* err) {
            return coordinator.on_server_ack(ack, now_ms, err);
        },
        .on_explicit_stop = [&coordinator]() {
            coordinator.on_explicit_stop();
        },
        .on_disconnect = [&coordinator]() {
            coordinator.on_disconnect();
        },
        .on_shutdown = [&coordinator]() {
            coordinator.on_service_shutdown();
            coordinator.close();
        },
        .on_restart = [&coordinator](const std::filesystem::path& path, std::string* err) {
            return coordinator.open(path, err);
        },
        .on_maintenance = [&coordinator](std::uint64_t now_ms, std::string* err) {
            return coordinator.maintain(now_ms, err);
        },
        .queue_agent_enable = std::move(queue_agent_enable),
        .next_batch_to_send = [&coordinator](std::uint64_t now_ms, std::uint64_t timeout_ms) {
            return coordinator.next_batch_to_send(now_ms, timeout_ms);
        },
        .on_explicit_stop_with_outcome = [&coordinator]() {
            return coordinator.on_explicit_stop_with_outcome();
        }
    } {}

void ExamTelemetryRouter::set_callbacks(ExamTelemetryRouterCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

bool ExamTelemetryRouter::handle_server_enable(std::span<const std::byte> payload, std::string* error) {
    const auto enable = exam::decode_telemetry_enable(payload, error);
    if (!enable.has_value()) {
        set_error(error, "malformed telemetry enable command");
        return false;
    }
    return handle_server_enable(*enable, payload, error);
}

bool ExamTelemetryRouter::handle_server_enable(const exam::TelemetryEnable& enable,
                                              std::span<const std::byte> raw_payload,
                                              std::string* error) {
    if (!callbacks_.on_enable) {
        set_error(error, "no on_enable callback configured");
        return false;
    }
    if (!callbacks_.on_enable(enable, error)) {
        return false;
    }
    if (callbacks_.queue_agent_enable) {
        callbacks_.queue_agent_enable(raw_payload);
    }
    return true;
}

bool ExamTelemetryRouter::handle_agent_samples(std::span<const std::byte> payload,
                                              std::uint64_t now_unix_ms,
                                              std::string* error) {
    const auto samples = decode_exam_telemetry_samples_payload(payload, error);
    if (!samples.has_value()) {
        set_error(error, "malformed telemetry samples payload");
        return false;
    }
    return handle_agent_samples(*samples, now_unix_ms, error);
}

bool ExamTelemetryRouter::handle_agent_samples(const std::vector<exam::TelemetrySample>& samples,
                                              std::uint64_t now_unix_ms,
                                              std::string* error) {
    if (!callbacks_.ingest_samples) {
        set_error(error, "no ingest_samples callback configured");
        return false;
    }
    return callbacks_.ingest_samples(samples, now_unix_ms, error);
}

exam::TelemetryAckApplyResult ExamTelemetryRouter::handle_server_ack(std::span<const std::byte> payload,
                                                                   std::uint64_t now_unix_ms,
                                                                   std::string* error) {
    const auto ack = exam::decode_telemetry_ack(payload, error);
    if (!ack.has_value()) {
        set_error(error, "malformed telemetry ack command");
        return exam::TelemetryAckApplyResult::ignored_mismatch;
    }
    return handle_server_ack(*ack, now_unix_ms, error);
}

exam::TelemetryAckApplyResult ExamTelemetryRouter::handle_server_ack(const exam::TelemetryAck& ack,
                                                                   std::uint64_t now_unix_ms,
                                                                   std::string* error) {
    if (!callbacks_.on_ack) {
        set_error(error, "no on_ack callback configured");
        return exam::TelemetryAckApplyResult::ignored_mismatch;
    }
    return callbacks_.on_ack(ack, now_unix_ms, error);
}

TelemetryStopDispatchResult ExamTelemetryRouter::handle_explicit_stop_with_outcome() {
    if (callbacks_.on_explicit_stop_with_outcome) {
        auto outcome = callbacks_.on_explicit_stop_with_outcome();
        return {TelemetryStopDispatch::outcome_callback_invoked, std::move(outcome)};
    }
    if (callbacks_.on_explicit_stop) {
        callbacks_.on_explicit_stop();
        return {TelemetryStopDispatch::legacy_callback_invoked, std::nullopt};
    }
    return {};
}

void ExamTelemetryRouter::handle_explicit_stop() {
    (void)handle_explicit_stop_with_outcome();
}

void ExamTelemetryRouter::handle_disconnect() {
    if (callbacks_.on_disconnect) {
        callbacks_.on_disconnect();
    }
}

void ExamTelemetryRouter::handle_shutdown() {
    if (callbacks_.on_shutdown) {
        callbacks_.on_shutdown();
    }
}

bool ExamTelemetryRouter::handle_restart(const std::filesystem::path& outbox_path,
                                        std::string* error) {
    if (callbacks_.on_restart) {
        return callbacks_.on_restart(outbox_path, error);
    }
    set_error(error, "no restart callback configured");
    return false;
}

bool ExamTelemetryRouter::handle_maintenance(std::uint64_t now_unix_ms,
                                            std::string* error) {
    if (callbacks_.on_maintenance) {
        return callbacks_.on_maintenance(now_unix_ms, error);
    }
    set_error(error, "no maintenance callback configured");
    return false;
}

std::optional<ClientOutboundCommand> ExamTelemetryRouter::poll_outbound_batch(
    std::uint64_t now_unix_ms,
    std::uint64_t inflight_timeout_ms) {
    if (!callbacks_.next_batch_to_send) {
        return std::nullopt;
    }
    auto batch = callbacks_.next_batch_to_send(now_unix_ms, inflight_timeout_ms);
    if (!batch.has_value()) {
        return std::nullopt;
    }
    auto encoded = exam::encode_telemetry_batch(*batch);
    if (encoded.empty()) {
        return std::nullopt;
    }
    return ClientOutboundCommand{
        protocol::CommandType::exam_telemetry_batch,
        std::move(encoded)
    };
}

} // namespace nstu::client
