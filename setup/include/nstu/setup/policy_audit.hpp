#pragma once

#include <string>

namespace nstu::setup {
struct PolicyState { bool task_manager_disabled = false; bool command_prompt_disabled = false; bool control_panel_disabled = false; bool drive_c_hidden = false; };
[[nodiscard]] PolicyState read_policy(std::string* error = nullptr);
[[nodiscard]] bool apply_lockdown(std::string* error = nullptr);
} // namespace nstu::setup
