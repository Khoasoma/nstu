#include "../server/app/exam_fleet_presentation.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace nstu::server;
using nstu::ui::UwfArmRejection;
constexpr std::uint64_t target = (std::uint64_t{9} << 32u) + 7;

int main() {
    std::vector<ClientRecord> clients(2);
    clients[0].id = target; clients[1].id = (std::uint64_t{3} << 32u) + 7;
    const auto decide = [&](std::uint64_t id = target, bool acknowledged = true,
                            OperatorRole role = OperatorRole::technician) {
        return nstu::ui::evaluate_uwf_arm(id, clients, role, acknowledged);
    };
    assert(decide().allowed());
    assert(decide(target, true, OperatorRole::teacher).rejection == UwfArmRejection::role);
    assert(decide(target, true, OperatorRole::developer).allowed());
    assert(decide(target, false).rejection == UwfArmRejection::checkpoint);
    assert(decide(0).rejection == UwfArmRejection::no_target);
    assert(decide(77).rejection == UwfArmRejection::missing_target);
    assert(decide().target == &clients[0] && decide().target_id == target);
    assert(decide(clients[1].id).target == &clients[1]);
    for (const auto status : {ClientStatus::connecting, ClientStatus::online,
                             ClientStatus::degraded, ClientStatus::locked}) {
        clients[0].status = status; assert(decide().allowed());
    }
    clients[0].status = ClientStatus::offline;
    assert(decide().rejection == UwfArmRejection::offline);
    assert(decide().target == &clients[0]);
    clients[0].status = ClientStatus::online;
    auto& uwf = clients[0].uwf;
    uwf.phase = nstu::control::UwfFleetPhase::unsupported;
    assert(decide().allowed()); // Unreported support remains uncertain.
    uwf.reported = true;
    assert(decide().rejection == UwfArmRejection::unsupported);
    uwf.phase = nstu::control::UwfFleetPhase::verified_protected;
    assert(decide().allowed()); // Phase alone has no current proof.
    uwf.probe.probe_succeeded = true;
    uwf.probe.filter_current_enabled = true;
    uwf.probe.system_volume_current_protected = true;
    uwf.verified_boot_id[0] = std::byte{1};
    uwf.observed_boot_id = uwf.verified_boot_id;
    assert(uwf.proves_current_protection());
    assert(decide().rejection == UwfArmRejection::already_protected);
    const auto proven = uwf;
    uwf.phase = nstu::control::UwfFleetPhase::idle; assert(decide().allowed()); uwf = proven;
    uwf.probe.probe_succeeded = false; assert(decide().allowed()); uwf = proven;
    uwf.probe.filter_current_enabled = false; assert(decide().allowed()); uwf = proven;
    uwf.probe.system_volume_current_protected = false; assert(decide().allowed()); uwf = proven;
    uwf.verified_boot_id = {}; uwf.observed_boot_id = {}; assert(decide().allowed()); uwf = proven;
    uwf.observed_boot_id[0] = std::byte{2}; assert(decide().allowed()); uwf = proven;
    assert(!uwf.probe.filter_next_enabled && !uwf.probe.data_exclusion_present &&
           !uwf.probe.registry_exclusion_present && uwf.operation_id == 0 && !clients[0].agent_connected);
    assert(decide().rejection == UwfArmRejection::already_protected);
    clients.erase(clients.begin());
    assert(decide().target == nullptr && decide().target_id == target);
    assert(decide().rejection == UwfArmRejection::missing_target);
    std::cout << "Current roster UWF role/checkpoint/identity/status/proof transitions: PASS\n";
}
