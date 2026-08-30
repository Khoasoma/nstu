#include "nstu/control_channel.hpp"
#include "nstu/enrollment.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"
#include "nstu/control_plane.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

bool receive_exact(nstu::net::TcpSocket& socket, std::span<std::byte> output) {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const int received = socket.receive(output.subspan(offset), nullptr);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

std::optional<nstu::protocol::TcpFrame> receive_frame(
    nstu::net::TcpSocket& socket) {
    std::array<std::byte, 4> prefix{};
    if (!receive_exact(socket, prefix)) {
        return std::nullopt;
    }
    const std::uint32_t body_bytes =
        std::to_integer<std::uint32_t>(prefix[0]) |
        (std::to_integer<std::uint32_t>(prefix[1]) << 8u) |
        (std::to_integer<std::uint32_t>(prefix[2]) << 16u) |
        (std::to_integer<std::uint32_t>(prefix[3]) << 24u);
    std::vector<std::byte> body(body_bytes);
    if (!receive_exact(socket, body)) {
        return std::nullopt;
    }
    std::vector<std::byte> wire(prefix.begin(), prefix.end());
    wire.insert(wire.end(), body.begin(), body.end());
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    return parser.feed(wire, frames) && frames.size() == 1
        ? std::optional{std::move(frames.front())}
        : std::nullopt;
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    nstu::security::ClientId client_id{};
    for (std::size_t index = 0; index < client_id.size(); ++index) {
        client_id[index] = static_cast<std::byte>(index + 1);
    }
    std::vector<std::byte> enrollment_secret(
        nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < enrollment_secret.size(); ++index) {
        enrollment_secret[index] = static_cast<std::byte>(0x20 + index);
    }
    const std::array<std::byte, 4> entropy{
        std::byte{1}, std::byte{3}, std::byte{5}, std::byte{7}};
    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto keyring_path = std::filesystem::path(temporary_directory) /
        (L"nstu-enrollment-keyring-" +
         std::to_wstring(GetCurrentProcessId()) + L".bin");

    nstu::security::KeyStore key_store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.keyring_path = keyring_path.wstring();
    config.keyring_entropy.assign(entropy.begin(), entropy.end());
    config.enrollment_secret = enrollment_secret;
    std::string error;
    assert(server.start(std::move(config), &error));

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto request = nstu::security::create_enrollment_request(
        client_id, 1, now, enrollment_secret);
    assert(request.has_value());
    const auto payload = nstu::security::encode_enrollment_request(*request);
    const nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::enrollment_request,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = 77,
    };
    const auto wire = nstu::protocol::encode_tcp_frame(envelope, payload);
    nstu::net::TcpSocket enrollment_socket;
    assert(enrollment_socket.connect("127.0.0.1", server.local_port(), &error));
    assert(enrollment_socket.set_io_timeouts(5000, 5000, &error));
    assert(enrollment_socket.send_all(wire, &error) ==
           static_cast<int>(wire.size()));
    const auto response = receive_frame(enrollment_socket);
    assert(response.has_value());
    assert(response->envelope.type ==
           nstu::protocol::CommandType::enrollment_accept);
    enrollment_socket.close();
    auto key = nstu::security::derive_enrolled_key(enrollment_secret,
                                                   *request);
    assert(key.has_value());

    nstu::net::TcpSocket control_socket;
    assert(control_socket.connect("127.0.0.1", server.local_port(), &error));
    assert(control_socket.set_io_timeouts(5000, 5000, &error));
    auto session = nstu::control::client_handshake(
        control_socket, client_id, 1, *key, now,
        std::chrono::seconds(120), &error);
    assert(session.has_value());
    control_socket.close();
    server.stop();

    nstu::security::KeyStore restored;
    assert(nstu::security::load_keyring(restored, keyring_path.wstring(),
                                        entropy, &error));
    assert(restored.resolve(client_id, 1) ==
           std::vector<std::byte>(key->begin(), key->end()));
    assert(DeleteFileW(keyring_path.c_str()));
    nstu::security::secure_zero(enrollment_secret);
    nstu::security::secure_zero(*key);
    return 0;
}
