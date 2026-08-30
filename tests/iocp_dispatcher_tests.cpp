#include "nstu/iocp_dispatcher.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
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

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    nstu::net::IocpDispatcher dispatcher;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t connected = 0;
    std::size_t frames = 0;
    std::size_t disconnected = 0;
    std::size_t protocol_errors = 0;
    std::string error;

    nstu::net::IocpDispatcherConfig config;
    config.port = 0;
    config.maximum_connections = 128;
    config.accept_depth = 32;
    config.worker_threads = 4;
    nstu::net::IocpDispatcherCallbacks callbacks;
    callbacks.on_connected = [&](nstu::net::ConnectionId id,
                                 const std::string& source) {
        assert(!source.empty());
        dispatcher.record_handshake_success(id);
        {
            std::scoped_lock lock(mutex);
            ++connected;
        }
        changed.notify_all();
    };
    callbacks.on_frame = [&](nstu::net::ConnectionId id,
                             nstu::protocol::TcpFrame frame) {
        assert(frame.envelope.type == nstu::protocol::CommandType::heartbeat);
        const std::array<std::byte, 1> payload{std::byte{0x7f}};
        nstu::protocol::CommandEnvelope response{
            .version = nstu::protocol::kCommandVersion,
            .type = nstu::protocol::CommandType::hello_ack,
            .payload_bytes = static_cast<std::uint32_t>(payload.size()),
            .request_id = frame.envelope.request_id,
        };
        const auto wire = nstu::protocol::encode_tcp_frame(response, payload);
        assert(dispatcher.send(id, wire, nullptr));
        {
            std::scoped_lock lock(mutex);
            ++frames;
        }
        changed.notify_all();
    };
    callbacks.on_disconnected = [&](nstu::net::ConnectionId) {
        {
            std::scoped_lock lock(mutex);
            ++disconnected;
        }
        changed.notify_all();
    };
    callbacks.on_audit = [&](const nstu::net::AuditEvent& event) {
        if (event.kind == nstu::net::AuditEventKind::protocol_error) {
            std::scoped_lock lock(mutex);
            ++protocol_errors;
        }
    };
    assert(dispatcher.start(config, std::move(callbacks), &error));
    assert(dispatcher.local_port() != 0);

    constexpr std::size_t client_count = 64;
    std::vector<nstu::net::TcpSocket> clients;
    clients.reserve(client_count);
    const std::array<std::byte, 2> payload{std::byte{0x10}, std::byte{0x20}};
    for (std::size_t index = 0; index < client_count; ++index) {
        nstu::net::TcpSocket client;
        assert(client.connect("127.0.0.1", dispatcher.local_port(), &error));
        assert(client.set_io_timeouts(5000, 5000, &error));
        nstu::protocol::CommandEnvelope command{
            .version = nstu::protocol::kCommandVersion,
            .type = nstu::protocol::CommandType::heartbeat,
            .payload_bytes = static_cast<std::uint32_t>(payload.size()),
            .request_id = index + 1,
        };
        const auto wire = nstu::protocol::encode_tcp_frame(command, payload);
        assert(client.send_all(wire, &error) == static_cast<int>(wire.size()));
        clients.push_back(std::move(client));
    }

    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return connected == client_count && frames == client_count;
        }));
    }
    assert(dispatcher.connection_count() == client_count);
    for (auto& client : clients) {
        std::array<std::byte, 4> prefix{};
        assert(receive_exact(client, prefix));
        const std::uint32_t body_bytes =
            std::to_integer<std::uint32_t>(prefix[0]) |
            (std::to_integer<std::uint32_t>(prefix[1]) << 8u) |
            (std::to_integer<std::uint32_t>(prefix[2]) << 16u) |
            (std::to_integer<std::uint32_t>(prefix[3]) << 24u);
        assert(body_bytes == nstu::protocol::kCommandHeaderBytes + 1);
        std::vector<std::byte> body(body_bytes);
        assert(receive_exact(client, body));
        client.close();
    }
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return disconnected == client_count;
        }));
    }
    assert(protocol_errors == 0);
    dispatcher.stop();
    assert(!dispatcher.running());

    nstu::net::IocpDispatcher limited_dispatcher;
    std::mutex limited_mutex;
    std::condition_variable limited_changed;
    std::size_t limited_connected = 0;
    std::size_t capacity_rejected = 0;
    nstu::net::IocpDispatcherConfig limited_config;
    limited_config.port = 0;
    limited_config.maximum_connections = 4;
    limited_config.accept_depth = 16;
    limited_config.worker_threads = 4;
    nstu::net::IocpDispatcherCallbacks limited_callbacks;
    limited_callbacks.on_connected = [&](nstu::net::ConnectionId,
                                         const std::string&) {
        {
            std::scoped_lock lock(limited_mutex);
            ++limited_connected;
        }
        limited_changed.notify_all();
    };
    limited_callbacks.on_audit = [&](const nstu::net::AuditEvent& event) {
        if (event.kind == nstu::net::AuditEventKind::capacity_rejected) {
            {
                std::scoped_lock lock(limited_mutex);
                ++capacity_rejected;
            }
            limited_changed.notify_all();
        }
    };
    assert(limited_dispatcher.start(limited_config,
                                    std::move(limited_callbacks), &error));
    constexpr std::size_t attempted_connections = 16;
    std::vector<nstu::net::TcpSocket> limited_clients;
    limited_clients.reserve(attempted_connections);
    for (std::size_t index = 0; index < attempted_connections; ++index) {
        nstu::net::TcpSocket client;
        assert(client.connect("127.0.0.1", limited_dispatcher.local_port(),
                              &error));
        limited_clients.push_back(std::move(client));
    }
    {
        std::unique_lock lock(limited_mutex);
        assert(limited_changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return limited_connected + capacity_rejected ==
                   attempted_connections;
        }));
    }
    assert(limited_connected == limited_config.maximum_connections);
    assert(limited_dispatcher.connection_count() ==
           limited_config.maximum_connections);
    for (auto& client : limited_clients) {
        client.close();
    }
    limited_dispatcher.stop();
    return 0;
}
