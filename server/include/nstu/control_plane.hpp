#pragma once

#include "nstu/client_registry.hpp"
#include "nstu/iocp_dispatcher.hpp"
#include "nstu/key_store.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::server {

struct ServerControlPlaneConfig {
    std::uint16_t port = 47001;
    std::wstring keyring_path;
    std::vector<std::byte> keyring_entropy;
    std::vector<std::byte> enrollment_secret;
    std::size_t maximum_clients = 512;
};

class ServerControlPlane {
public:
    ServerControlPlane(ClientRegistry& registry,
                       security::KeyStore& key_store);
    ~ServerControlPlane();
    ServerControlPlane(const ServerControlPlane&) = delete;
    ServerControlPlane& operator=(const ServerControlPlane&) = delete;

    [[nodiscard]] bool start(ServerControlPlaneConfig config,
                             std::string* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool send_command(
        std::uint64_t client_id, protocol::CommandType type,
        std::span<const std::byte> payload = {},
        std::string* error = nullptr);
    [[nodiscard]] bool set_locked(std::uint64_t client_id, bool locked,
                                  std::string* error = nullptr);
    [[nodiscard]] bool set_streaming(std::uint64_t client_id, bool enabled,
                                     std::uint8_t frames_per_second,
                                     std::string* error = nullptr);
    [[nodiscard]] bool set_snapshots(std::uint64_t client_id, bool enabled,
                                     std::uint16_t interval_seconds,
                                     std::string* error = nullptr);
    [[nodiscard]] bool send_overlay_stroke(
        std::uint64_t client_id, const control::OverlayStroke& stroke,
        std::string* error = nullptr);
    [[nodiscard]] bool clear_overlay(std::uint64_t client_id,
                                     std::string* error = nullptr);
    [[nodiscard]] bool broadcast_host_snapshot(
        const control::SnapshotFrame& frame,
        std::string* error = nullptr);
    [[nodiscard]] bool stop_host_broadcast(std::string* error = nullptr);
    [[nodiscard]] bool request_keyframe(std::uint64_t client_id,
                                        std::string* error = nullptr);
    [[nodiscard]] bool send_chat(std::uint64_t client_id,
                                 std::string_view utf8_message,
                                 std::string* error = nullptr);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::size_t authenticated_client_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
