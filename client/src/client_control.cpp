#include "nstu/client_control.hpp"

#include "nstu/multicast.hpp"

#include <chrono>
#include <thread>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::uint64_t unix_seconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

bool send_status(control::AuthenticatedControlChannel& channel,
                 const ClientStatusProvider& provider,
                 protocol::CommandType type, std::uint64_t request_id,
                 std::string* error) {
    const auto payload = control::encode_status_report(provider());
    return !payload.empty() && channel.send(type, request_id, payload, error);
}

} // namespace

bool run_client_control_session(
    const ClientRuntimeConfig& config, std::stop_token stop_token,
    const ClientStatusProvider& status_provider,
    const ClientCommandHandler& command_handler,
    const ClientOutboundProvider& outbound_provider, std::string* error) {
    if (!status_provider || !command_handler) {
        set_error(error, "client control callbacks are missing");
        return false;
    }
    net::WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "Winsock initialization failed");
        return false;
    }
    net::TcpSocket socket;
    if (!socket.connect(config.server_address, config.server_port, error) ||
        !socket.set_io_timeouts(5000, 5000, error)) {
        return false;
    }
    auto session = control::client_handshake(
        socket, config.client_id, config.key_id, config.pre_shared_key,
        unix_seconds_now(), std::chrono::seconds(120), error);
    if (!session) {
        return false;
    }
    control::AuthenticatedControlChannel channel(std::move(socket),
                                                 std::move(*session));
    std::uint64_t request_id = 1;
    if (!send_status(channel, status_provider, protocol::CommandType::hello,
                     request_id++, error)) {
        return false;
    }
    auto next_heartbeat = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    auto next_status = std::chrono::steady_clock::now() +
                       std::chrono::seconds(2);
    while (!stop_token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
            if (!channel.send(protocol::CommandType::heartbeat, request_id++,
                              {}, error)) {
                return false;
            }
            next_heartbeat = now + std::chrono::seconds(5);
        }
        if (now >= next_status) {
            if (!send_status(channel, status_provider,
                             protocol::CommandType::status_report,
                             request_id++, error)) {
                return false;
            }
            next_status = now + std::chrono::seconds(2);
        }
        if (outbound_provider) {
            for (int sent = 0; sent < 4; ++sent) {
                auto outbound = outbound_provider();
                if (!outbound) {
                    break;
                }
                if (!channel.send(outbound->type, request_id++,
                                  outbound->payload, error)) {
                    return false;
                }
            }
        }
        std::string wait_error;
        if (!channel.wait_readable(100, &wait_error)) {
            if (!wait_error.empty()) {
                if (error != nullptr) {
                    *error = wait_error;
                }
                return false;
            }
            continue;
        }
        const auto command = channel.receive(error);
        if (!command) {
            return false;
        }
        command_handler(*command);
        if (command->envelope.type == protocol::CommandType::status_request) {
            if (!send_status(channel, status_provider,
                             protocol::CommandType::status_report,
                             command->envelope.request_id, error)) {
                return false;
            }
        }
    }
    channel.close();
    return true;
}

} // namespace nstu::client
