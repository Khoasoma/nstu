#include "nstu/operator_role.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

namespace {

void test_role_to_string_and_labels() {
    using nstu::server::OperatorRole;
    using nstu::server::to_string;
    using nstu::server::role_label;

    assert(std::strcmp(to_string(OperatorRole::teacher), "teacher") == 0);
    assert(std::strcmp(to_string(OperatorRole::technician), "technician") == 0);
    assert(std::strcmp(to_string(OperatorRole::developer), "developer") == 0);

    // English labels
    assert(std::strcmp(role_label(OperatorRole::teacher, false), "Teacher") == 0);
    assert(std::strcmp(role_label(OperatorRole::technician, false), "Technician") == 0);
    assert(std::strcmp(role_label(OperatorRole::developer, false), "Developer (Advanced)") == 0);

    // Vietnamese labels
    assert(std::strcmp(role_label(OperatorRole::teacher, true), "Giáo viên") == 0);
    assert(std::strcmp(role_label(OperatorRole::technician, true), "Kỹ thuật viên") == 0);
    assert(std::strcmp(role_label(OperatorRole::developer, true), "Nhà phát triển (Nâng cao)") == 0);
}

void test_teacher_permissions() {
    using nstu::server::OperatorRole;
    using nstu::server::OperatorAction;
    using nstu::server::is_action_permitted;

    constexpr auto role = OperatorRole::teacher;

    // Permitted common classroom actions
    assert(is_action_permitted(role, OperatorAction::view_classroom));
    assert(is_action_permitted(role, OperatorAction::toggle_lock));
    assert(is_action_permitted(role, OperatorAction::broadcast));
    assert(is_action_permitted(role, OperatorAction::chat));
    assert(is_action_permitted(role, OperatorAction::manage_exam));
    assert(is_action_permitted(role, OperatorAction::manage_settings));

    // Disruptive/maintenance actions must NOT be permitted for Teacher
    assert(!is_action_permitted(role, OperatorAction::configure_uwf));
    assert(!is_action_permitted(role, OperatorAction::reboot_client));
    assert(!is_action_permitted(role, OperatorAction::shutdown_client));
    assert(!is_action_permitted(role, OperatorAction::view_fleet_health));
    assert(!is_action_permitted(role, OperatorAction::view_diagnostics_raw));
    assert(!is_action_permitted(role, OperatorAction::export_telemetry_report));
}

void test_technician_permissions() {
    using nstu::server::OperatorRole;
    using nstu::server::OperatorAction;
    using nstu::server::is_action_permitted;

    constexpr auto role = OperatorRole::technician;

    // Technician has classroom permissions
    assert(is_action_permitted(role, OperatorAction::view_classroom));
    assert(is_action_permitted(role, OperatorAction::toggle_lock));
    assert(is_action_permitted(role, OperatorAction::broadcast));
    assert(is_action_permitted(role, OperatorAction::chat));
    assert(is_action_permitted(role, OperatorAction::manage_exam));
    assert(is_action_permitted(role, OperatorAction::manage_settings));

    // Technician has fleet health & UWF recovery permissions
    assert(is_action_permitted(role, OperatorAction::view_fleet_health));
    assert(is_action_permitted(role, OperatorAction::configure_uwf));
    assert(is_action_permitted(role, OperatorAction::reboot_client));
    assert(is_action_permitted(role, OperatorAction::shutdown_client));

    // Raw internal diagnostics & export remain developer-only
    assert(!is_action_permitted(role, OperatorAction::view_diagnostics_raw));
    assert(!is_action_permitted(role, OperatorAction::export_telemetry_report));
}

void test_developer_permissions() {
    using nstu::server::OperatorRole;
    using nstu::server::OperatorAction;
    using nstu::server::is_action_permitted;

    constexpr auto role = OperatorRole::developer;

    // Developer has full access across all operations
    assert(is_action_permitted(role, OperatorAction::view_classroom));
    assert(is_action_permitted(role, OperatorAction::toggle_lock));
    assert(is_action_permitted(role, OperatorAction::broadcast));
    assert(is_action_permitted(role, OperatorAction::chat));
    assert(is_action_permitted(role, OperatorAction::manage_exam));
    assert(is_action_permitted(role, OperatorAction::configure_uwf));
    assert(is_action_permitted(role, OperatorAction::reboot_client));
    assert(is_action_permitted(role, OperatorAction::shutdown_client));
    assert(is_action_permitted(role, OperatorAction::view_fleet_health));
    assert(is_action_permitted(role, OperatorAction::view_diagnostics_raw));
    assert(is_action_permitted(role, OperatorAction::export_telemetry_report));
    assert(is_action_permitted(role, OperatorAction::manage_settings));
}

} // namespace

int main() {
    test_role_to_string_and_labels();
    test_teacher_permissions();
    test_technician_permissions();
    test_developer_permissions();
    std::cout << "All operator role tests passed successfully.\n";
    return 0;
}

