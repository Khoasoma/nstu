#pragma once

#include "nstu/control_plane.hpp"

namespace nstu::ui {

struct PairingRequestView {
    bool awaiting_student = true;
    bool can_approve = false;
};

// Presentation/action guard only. The server's unchanged approve_pairing
// independently verifies the current record, client tag and expiry.
[[nodiscard]] inline PairingRequestView pairing_request_view(
    const server::PairingRequest& request) noexcept {
    const bool awaiting = request.stage != server::PairingRequestStage::awaiting_teacher_approval;
    return {awaiting, !awaiting && request.pairing_id != 0 && request.seconds_remaining != 0};
}

} // namespace nstu::ui
