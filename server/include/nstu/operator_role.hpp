#pragma once

#include "nstu/update_manifest.hpp"

#include <cstdint>
#include <string_view>

namespace nstu::server {

enum class OperatorRole : int {
    teacher = 0,
    technician = 1,
    developer = 2,
};

enum class OperatorAction : int {
    view_classroom = 0,
    toggle_lock = 1,
    broadcast = 2,
    chat = 3,
    manage_exam = 4,
    configure_uwf = 5,
    reboot_client = 6,
    view_fleet_health = 7,
    view_diagnostics_raw = 8,
    export_telemetry_report = 9,
    manage_settings = 10,
    shutdown_client = 11,
    wake_on_lan = 12,
    distribute_file = 13,
    manage_software_updates = 14,
};

struct FleetUpdateConfig {
    // Session-only planning preferences, never proof of maintenance readiness
    // or authorization to stage/apply a package on a client.
    update::ReleaseChannel channel{update::ReleaseChannel::ltsc_stable};
    bool maintenance_window_planned{false};
    int canary_rollout_percentage{0};
};

[[nodiscard]] bool try_set_fleet_update_config(
    OperatorRole role, FleetUpdateConfig& current,
    const FleetUpdateConfig& proposed) noexcept;

[[nodiscard]] bool is_action_permitted(
    OperatorRole role,
    OperatorAction action) noexcept;

[[nodiscard]] const char* to_string(OperatorRole role) noexcept;

[[nodiscard]] const char* role_label(OperatorRole role, bool vietnamese) noexcept;

} // namespace nstu::server

