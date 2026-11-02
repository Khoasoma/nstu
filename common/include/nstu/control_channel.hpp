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

inline constexpr std::size_t kAeadKeyBytes = 32;
inline constexpr std::size_t kAeadNonceBytes = 12;
inline constexpr std::size_t kAeadAuthTagBytes = 16;

struct AeadSessionKeys {
    security::Sha256Digest client_to_server_key{};
    security::Sha256Digest server_to_client_key{};
    std::array<std::byte, kAeadNonceBytes> client_to_server_iv{};
    std::array<std::byte, kAeadNonceBytes> server_to_client_iv{};
};

[[nodiscard]] std::optional<AeadSessionKeys> derive_aead_session_keys(
    std::span<const std::byte> session_key) noexcept;

[[nodiscard]] std::array<std::byte, kAeadNonceBytes> construct_aead_nonce(
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    std::uint64_t sequence) noexcept;

class AuthenticatedSession {
public:
    AuthenticatedSession(security::Sha256Digest session_key,
                         security::ClientId client_id,
                         std::uint32_t key_id,
                         bool aead_enabled = false,
                         protocol::ConnectionRole role = protocol::ConnectionRole::client) noexcept;
    ~AuthenticatedSession();
    AuthenticatedSession(AuthenticatedSession&& other) noexcept;
    AuthenticatedSession& operator=(AuthenticatedSession&& other) noexcept;
    AuthenticatedSession(const AuthenticatedSession&) = delete;
    AuthenticatedSession& operator=(const AuthenticatedSession&) = delete;

    security::Sha256Digest session_key{};
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
    bool aead_enabled = false;
    protocol::ConnectionRole role = protocol::ConnectionRole::client;
};

using KeyResolver = std::function<std::optional<std::vector<std::byte>>(
    const security::ClientId&, std::uint32_t key_id)>;

[[nodiscard]] std::optional<AuthenticatedSession> client_handshake(
    net::TcpSocket& socket, const security::ClientId& client_id,
    std::uint32_t key_id, std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120),
    std::string* error = nullptr,
    bool request_aead = true);

[[nodiscard]] std::optional<AuthenticatedSession> server_handshake(
    net::TcpSocket& socket, const KeyResolver& key_resolver,
    security::ReplayProtector& replay_protector,
    std::uint64_t now_unix_seconds, std::string* error = nullptr,
    bool allow_aead = true);

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

[[nodiscard]] std::vector<std::byte> encode_authenticated_command_aead(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload);

[[nodiscard]] std::optional<AuthenticatedCommand> decode_authenticated_command_aead(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    const protocol::TcpFrame& wire_frame, std::string* error = nullptr);

// Socket I/O and command sequence state have one owner thread at a time.
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
    [[nodiscard]] bool send(protocol::CommandType type,
                            std::uint64_t request_id,
                            std::span<const std::byte> payload,
                            std::chrono::steady_clock::time_point deadline,
                            std::string* error = nullptr);
    [[nodiscard]] bool send_with_deadline(
        protocol::CommandType type,
        std::uint64_t request_id,
        std::span<const std::byte> payload,
        std::chrono::steady_clock::time_point deadline,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<AuthenticatedCommand> receive(
        std::string* error = nullptr);
    [[nodiscard]] bool wait_readable(std::uint32_t timeout_ms,
                                     std::string* error = nullptr) const;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_aead() const noexcept;
    void close() noexcept;

private:
    [[nodiscard]] bool send_impl(
        protocol::CommandType type, std::uint64_t request_id,
        std::span<const std::byte> payload,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        std::string* error);

    net::TcpSocket socket_;
    security::Sha256Digest session_key_{};
    bool aead_enabled_ = false;
    protocol::ConnectionRole role_ = protocol::ConnectionRole::client;
    security::Sha256Digest send_key_{};
    std::array<std::byte, kAeadNonceBytes> send_iv_base_{};
    security::Sha256Digest receive_key_{};
    std::array<std::byte, kAeadNonceBytes> receive_iv_base_{};
    security::ControlSequenceGuard receive_sequence_;
    std::uint64_t send_sequence_ = 0;
    bool send_exhausted_ = false;
};

} // namespace nstu::control
