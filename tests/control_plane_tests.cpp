#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/multicast.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 7);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x50 + index);
    }
    return key;
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    const auto id = client_id();
    auto key = client_key();
    constexpr std::uint32_t key_id = 4;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 64;
    assert(control_plane.start(std::move(config), &error));

    nstu::net::TcpSocket socket;
    assert(socket.connect("127.0.0.1", control_plane.local_port(), &error));
    assert(socket.set_io_timeouts(5000, 5000, &error));
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session = nstu::control::client_handshake(
        socket, id, key_id, key, now, std::chrono::seconds(120), &error);
    assert(session.has_value());
    nstu::control::AuthenticatedControlChannel channel(
        std::move(socket), std::move(*session));
    nstu::control::ClientStatusReport status;
    status.hostname = "LAB-PC-01";
    status.session_id = 3;
    status.latency_ms = 8;
    status.packet_loss_per_mille = 5;
    status.packet_loss_sample_size = 500;
    const auto status_payload = nstu::control::encode_status_report(status);
    assert(channel.send(nstu::protocol::CommandType::hello, 1,
                        status_payload, &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return snapshot.size() == 1 && snapshot[0].hostname == "LAB-PC-01";
    }));
    const auto clients = registry.snapshot();
    assert(clients.size() == 1);
    assert(clients[0].hostname == "LAB-PC-01");
    const auto registry_id = clients[0].id;

    assert(control_plane.set_locked(registry_id, true, &error));
    const auto lock = channel.receive(&error);
    assert(lock.has_value());
    assert(lock->envelope.type == nstu::protocol::CommandType::lock);

    status.locked = true;
    const auto locked_payload = nstu::control::encode_status_report(status);
    assert(channel.send(nstu::protocol::CommandType::status_report, 2,
                        locked_payload, &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot[0].status == nstu::server::ClientStatus::locked;
    }));

    channel.close();
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot[0].status == nstu::server::ClientStatus::offline;
    }));
    control_plane.stop();
    nstu::security::secure_zero(key);
    return 0;
}
