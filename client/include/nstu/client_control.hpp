#pragma once

#include "nstu/client_config.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"

#include <functional>
#include <stop_token>
#include <string>

namespace nstu::client {

using ClientStatusProvider = std::function<control::ClientStatusReport()>;
using ClientCommandHandler =
    std::function<void(const control::AuthenticatedCommand&)>;

[[nodiscard]] bool run_client_control_session(
    const ClientRuntimeConfig& config, std::stop_token stop_token,
    const ClientStatusProvider& status_provider,
    const ClientCommandHandler& command_handler,
    std::string* error = nullptr);

} // namespace nstu::client
