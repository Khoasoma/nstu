#pragma once
#include "nstu/client_registry.hpp"
#include "nstu/operator_role.hpp"
#include <algorithm>
#include <span>

namespace nstu::ui {
enum class UwfArmRejection : std::uint8_t {
    none, role, no_target, missing_target, offline, unsupported, already_protected, checkpoint
};
struct UwfArmDecision {
    std::uint64_t target_id;
    // Borrowed from this draw's roster; never retain across frames.
    const server::ClientRecord* target;
    UwfArmRejection rejection;
    [[nodiscard]] bool allowed() const noexcept { return rejection == UwfArmRejection::none; }
};
[[nodiscard]] inline UwfArmDecision evaluate_uwf_arm(
    std::uint64_t stored_id, std::span<const server::ClientRecord> clients,
    server::OperatorRole role, bool checkpoint_acknowledged) noexcept {
    const auto found = std::find_if(clients.begin(), clients.end(),
        [stored_id](const auto& client) { return client.id == stored_id; });
    const auto* target = found == clients.end() ? nullptr : &*found;
    auto reason = UwfArmRejection::none;
    if (!server::is_action_permitted(role, server::OperatorAction::configure_uwf)) reason = UwfArmRejection::role;
    else if (stored_id == 0) reason = UwfArmRejection::no_target;
    else if (!target) reason = UwfArmRejection::missing_target;
    else if (target->status == server::ClientStatus::offline) reason = UwfArmRejection::offline;
    else if (target->uwf.reported && target->uwf.phase == control::UwfFleetPhase::unsupported) reason = UwfArmRejection::unsupported;
    else if (target->uwf.proves_current_protection()) reason = UwfArmRejection::already_protected;
    else if (!checkpoint_acknowledged) reason = UwfArmRejection::checkpoint;
    return {stored_id, target, reason};
}
} // namespace nstu::ui
