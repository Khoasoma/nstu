#pragma once

#include <string>

namespace nstu::client {

[[nodiscard]] bool launch_agent_in_active_session(
    const std::wstring& agent_path, std::string* error = nullptr);

[[nodiscard]] bool harden_service_dacl(const std::wstring& service_name,
                                       std::string* error = nullptr);

} // namespace nstu::client
