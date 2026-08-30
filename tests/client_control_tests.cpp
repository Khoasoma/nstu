#include "nstu/client_control.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/multicast.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 300; ++attempt) {
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
    nstu::client::ClientRuntimeConfig client_config;
    client_config.server_address = "127.0.0.1";
    client_config.client_id.fill(std::byte{0});
    for (std::size_t index = 0; index < client_config.client_id.size(); ++index) {
        client_config.client_id[index] = static_cast<std::byte>(index + 21);
    }
    client_config.key_id = 8;
    client_config.pre_shared_key.resize(
        nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < client_config.pre_shared_key.size();
         ++index) {
        client_config.pre_shared_key[index] =
            static_cast<std::byte>(0x60 + index);
    }

    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(client_config.client_id, client_config.key_id,
                            client_config.pre_shared_key, &error));
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig server_config;
    server_config.port = 0;
    assert(server.start(std::move(server_config), &error));
    client_config.server_port = server.local_port();

    std::stop_source stop_source;
    std::atomic_bool lock_received = false;
    std::atomic_bool session_succeeded = false;
    std::thread client([&] {
        std::string client_error;
        const bool result = nstu::client::run_client_control_session(
            client_config, stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "SERVICE-PC";
                status.session_id = 2;
                return status;
            },
            [&](const nstu::control::AuthenticatedCommand& command) {
                if (command.envelope.type ==
                    nstu::protocol::CommandType::lock) {
                    lock_received = true;
                    stop_source.request_stop();
                }
            },
            &client_error);
        session_succeeded = result;
    });
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return clients.size() == 1 && clients[0].hostname == "SERVICE-PC";
    }));
    const auto registry_id = registry.snapshot()[0].id;
    assert(server.set_locked(registry_id, true, &error));
    assert(wait_until([&] { return lock_received.load(); }));
    client.join();
    assert(session_succeeded.load());
    server.stop();
    nstu::client::clear_client_runtime_config(client_config);
    return 0;
}
