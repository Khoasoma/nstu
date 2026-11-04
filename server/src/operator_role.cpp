#include "nstu/operator_role.hpp"

namespace nstu::server {

bool is_action_permitted(OperatorRole role, OperatorAction action) noexcept {
    switch (role) {
    case OperatorRole::teacher:
        switch (action) {
        case OperatorAction::view_classroom:
        case OperatorAction::toggle_lock:
        case OperatorAction::broadcast:
        case OperatorAction::chat:
        case OperatorAction::manage_exam:
        case OperatorAction::manage_settings:
        case OperatorAction::wake_on_lan:
        case OperatorAction::distribute_file:
            return true;
        case OperatorAction::configure_uwf:
        case OperatorAction::reboot_client:
        case OperatorAction::shutdown_client:
        case OperatorAction::view_fleet_health:
        case OperatorAction::view_diagnostics_raw:
        case OperatorAction::export_telemetry_report:
        case OperatorAction::manage_software_updates:
            return false;
        }
        return false;

    case OperatorRole::technician:
        switch (action) {
        case OperatorAction::view_classroom:
        case OperatorAction::toggle_lock:
        case OperatorAction::broadcast:
        case OperatorAction::chat:
        case OperatorAction::manage_exam:
        case OperatorAction::configure_uwf:
        case OperatorAction::reboot_client:
        case OperatorAction::shutdown_client:
        case OperatorAction::view_fleet_health:
        case OperatorAction::manage_settings:
        case OperatorAction::wake_on_lan:
        case OperatorAction::distribute_file:
        case OperatorAction::manage_software_updates:
            return true;
        case OperatorAction::view_diagnostics_raw:
        case OperatorAction::export_telemetry_report:
            return false;
        }
        return false;

    case OperatorRole::developer:
        return true;
    }
    return false;
}

bool try_set_fleet_update_config(
    OperatorRole role, FleetUpdateConfig& current,
    const FleetUpdateConfig& proposed) noexcept {
    if (!is_action_permitted(role, OperatorAction::manage_software_updates) ||
        proposed.canary_rollout_percentage < 0 ||
        proposed.canary_rollout_percentage > 100) {
        return false;
    }
    switch (proposed.channel) {
    case update::ReleaseChannel::ltsc_stable:
    case update::ReleaseChannel::security:
    case update::ReleaseChannel::pilot:
        current = proposed;
        return true;
    }
    return false;
}

const char* to_string(OperatorRole role) noexcept {
    switch (role) {
    case OperatorRole::teacher:
        return "teacher";
    case OperatorRole::technician:
        return "technician";
    case OperatorRole::developer:
        return "developer";
    }
    return "unknown";
}

const char* role_label(OperatorRole role, bool vietnamese) noexcept {
    if (vietnamese) {
        switch (role) {
        case OperatorRole::teacher:
            return "Giáo viên";
        case OperatorRole::technician:
            return "Kỹ thuật viên";
        case OperatorRole::developer:
            return "Nhà phát triển (Nâng cao)";
        }
        return "Không rõ";
    }
    switch (role) {
    case OperatorRole::teacher:
        return "Teacher";
    case OperatorRole::technician:
        return "Technician";
    case OperatorRole::developer:
        return "Developer (Advanced)";
    }
    return "Unknown";
}

} // namespace nstu::server

