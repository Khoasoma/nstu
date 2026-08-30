#pragma once

#include "nstu/protocol.hpp"
#include "nstu/rate_limiter.hpp"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nstu::net {

using ConnectionId = std::uint64_t;

enum class AuditEventKind : std::uint8_t {
    accepted,
    admission_blocked,
    handshake_succeeded,
    handshake_failed,
    protocol_error,
    transport_error,
    disconnected,
    capacity_rejected,
};

struct AuditEvent {
    AuditEventKind kind = AuditEventKind::transport_error;
    ConnectionId connection_id = 0;
    std::string source;
    std::string detail;
};

struct IocpDispatcherConfig {
    std::uint16_t port = 47001;
    int backlog = 128;
    std::size_t maximum_connections = 512;
    std::size_t accept_depth = 16;
    std::size_t receive_buffer_bytes = 16 * 1024;
    std::size_t maximum_pending_send_bytes = 1024 * 1024;
    std::size_t worker_threads = 0;
    std::chrono::milliseconds idle_timeout{30'000};
    security::HandshakeRateLimitPolicy handshake_rate_limit{};
};

struct IocpDispatcherCallbacks {
    std::function<void(ConnectionId, const std::string&)> on_connected;
    // When set, raw receive chunks are delivered here and on_frame is bypassed.
    // This supports protocols with a fixed preamble before framed messages.
    std::function<void(ConnectionId, std::vector<std::byte>)> on_bytes;
    std::function<void(ConnectionId, protocol::TcpFrame)> on_frame;
    std::function<void(ConnectionId)> on_disconnected;
    std::function<void(const AuditEvent&)> on_audit;
};

class IocpDispatcher {
public:
    IocpDispatcher();
    ~IocpDispatcher();
    IocpDispatcher(const IocpDispatcher&) = delete;
    IocpDispatcher& operator=(const IocpDispatcher&) = delete;

    [[nodiscard]] bool start(const IocpDispatcherConfig& config,
                             IocpDispatcherCallbacks callbacks,
                             std::string* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool send(ConnectionId connection_id,
                            std::span<const std::byte> wire,
                            std::string* error = nullptr);
    void disconnect(ConnectionId connection_id) noexcept;
    void record_handshake_success(ConnectionId connection_id);
    void record_handshake_failure(ConnectionId connection_id,
                                  std::string detail);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::net
