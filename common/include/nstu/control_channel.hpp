#pragma once

#include "nstu/auth.hpp"
#include "nstu/network.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::control {

class AuthenticatedSession {
public:
    AuthenticatedSession(security::Sha256Digest session_key,
                         security::ClientId client_id,
                         std::uint32_t key_id) noexcept;
    ~AuthenticatedSession();
    AuthenticatedSession(AuthenticatedSession&& other) noexcept;
    AuthenticatedSession& operator=(AuthenticatedSession&& other) noexcept;
    AuthenticatedSession(const AuthenticatedSession&) = delete;
    AuthenticatedSession& operator=(const AuthenticatedSession&) = delete;

    security::Sha256Digest session_key{};
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
};

using KeyResolver = std::function<std::optional<std::vector<std::byte>>(
    const security::ClientId&, std::uint32_t key_id)>;

[[nodiscard]] std::optional<AuthenticatedSession> client_handshake(
    net::TcpSocket& socket, const security::ClientId& client_id,
    std::uint32_t key_id, std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120),
    std::string* error = nullptr);

[[nodiscard]] std::optional<AuthenticatedSession> server_handshake(
    net::TcpSocket& socket, const KeyResolver& key_resolver,
    security::ReplayProtector& replay_protector,
    std::uint64_t now_unix_seconds, std::string* error = nullptr);

struct AuthenticatedCommand {
    protocol::CommandEnvelope envelope;
    std::uint64_t sequence = 0;
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encode_authenticated_command(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload);

[[nodiscard]] std::optional<AuthenticatedCommand> decode_authenticated_command(
    std::span<const std::byte> session_key,
    const protocol::TcpFrame& wire_frame, std::string* error = nullptr);

class AuthenticatedControlChannel {
public:
    AuthenticatedControlChannel(net::TcpSocket socket,
                                security::Sha256Digest session_key) noexcept;
    AuthenticatedControlChannel(net::TcpSocket socket,
                                AuthenticatedSession&& session) noexcept;
    ~AuthenticatedControlChannel();
    AuthenticatedControlChannel(AuthenticatedControlChannel&&) noexcept;
    AuthenticatedControlChannel& operator=(AuthenticatedControlChannel&&) noexcept;
    AuthenticatedControlChannel(const AuthenticatedControlChannel&) = delete;
    AuthenticatedControlChannel& operator=(const AuthenticatedControlChannel&) = delete;

    [[nodiscard]] bool send(protocol::CommandType type,
                            std::uint64_t request_id,
                            std::span<const std::byte> payload,
                            std::string* error = nullptr);
    [[nodiscard]] std::optional<AuthenticatedCommand> receive(
        std::string* error = nullptr);
    [[nodiscard]] bool wait_readable(std::uint32_t timeout_ms,
                                     std::string* error = nullptr) const;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

private:
    net::TcpSocket socket_;
    security::Sha256Digest session_key_{};
    security::ControlSequenceGuard receive_sequence_;
    std::uint64_t send_sequence_ = 0;
    bool send_exhausted_ = false;
};

} // namespace nstu::control
