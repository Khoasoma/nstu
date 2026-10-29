#include "nstu/exam_telemetry_lifecycle.hpp"

#include <utility>

namespace nstu::client {

ExamTelemetryLifecycle::ExamTelemetryLifecycle(
    TelemetryContextIdentity context,
    TelemetryLifecycleState initial_state) noexcept
    : context_(std::move(context)) {
    switch (initial_state) {
        case TelemetryLifecycleState::disabled:
        case TelemetryLifecycleState::enabled:
        case TelemetryLifecycleState::suspended:
        case TelemetryLifecycleState::revoked:
            break;
        default:
            state_ = TelemetryLifecycleState::revoked;
            return;
    }

    if (!context_.bound()) {
        if (initial_state == TelemetryLifecycleState::enabled ||
            initial_state == TelemetryLifecycleState::suspended) {
            state_ = TelemetryLifecycleState::disabled;
            return;
        }
    }

    state_ = initial_state;
}

bool ExamTelemetryLifecycle::archived(const TelemetryContextIdentity& id) const noexcept {
    for (std::size_t i = 0; i < revoked_count_; ++i) {
        if (revoked_[i] == id) {
            return true;
        }
    }
    return false;
}

void ExamTelemetryLifecycle::archive_current() noexcept {
    if (state_ == TelemetryLifecycleState::revoked &&
        context_.bound() &&
        !archived(context_)) {
        if (revoked_count_ < kMaximumRevokedTelemetryIdentities) {
            revoked_[revoked_count_++] = std::move(context_);
        }
    }
}

bool ExamTelemetryLifecycle::is_identity_revoked(
    const TelemetryContextIdentity& id) const noexcept {
    if (archived(id)) {
        return true;
    }
    if (state_ == TelemetryLifecycleState::revoked && context_ == id) {
        return true;
    }
    return false;
}

bool ExamTelemetryLifecycle::set_context(TelemetryContextIdentity context) noexcept {
    if (context == context_) {
        return true;
    }

    const bool needs_archive = (state_ == TelemetryLifecycleState::revoked &&
                                context_.bound() &&
                                !archived(context_));

    if (revoked_count_ == kMaximumRevokedTelemetryIdentities) {
        if (needs_archive || !archived(context)) {
            return false;
        }
    }

    if (needs_archive) {
        archive_current();
    }

    context_ = std::move(context);
    state_ = archived(context_) ? TelemetryLifecycleState::revoked
                                : TelemetryLifecycleState::disabled;
    return true;
}

LifecycleTransitionResult ExamTelemetryLifecycle::on_enable(
    const exam::TelemetryEnable& enable) noexcept {
    LifecycleTransitionResult result;
    result.previous_state = state_;
    result.current_state = state_;

    if (!context_.bound()) {
        result.accepted = false;
        result.state_changed = false;
        return result;
    }

    if (!context_.matches(enable)) {
        result.accepted = false;
        result.state_changed = false;
        return result;
    }

    if (state_ == TelemetryLifecycleState::revoked) {
        // Explicit stop permanently revokes context and forbids replay/re-enable.
        result.accepted = false;
        result.state_changed = false;
        return result;
    }

    result.accepted = true;
    if (state_ != TelemetryLifecycleState::enabled) {
        state_ = TelemetryLifecycleState::enabled;
        result.state_changed = true;
    } else {
        result.state_changed = false;
    }
    result.current_state = state_;
    return result;
}

LifecycleTransitionResult ExamTelemetryLifecycle::on_disconnect() noexcept {
    LifecycleTransitionResult result;
    result.accepted = true;
    result.previous_state = state_;

    if (state_ == TelemetryLifecycleState::enabled) {
        state_ = TelemetryLifecycleState::suspended;
        result.state_changed = true;
        result.clear_inflight = true;
    } else {
        result.state_changed = false;
    }
    result.current_state = state_;
    return result;
}

LifecycleTransitionResult ExamTelemetryLifecycle::on_service_shutdown() noexcept {
    return on_disconnect();
}

LifecycleTransitionResult ExamTelemetryLifecycle::on_explicit_stop() noexcept {
    LifecycleTransitionResult result;
    result.accepted = true;
    result.previous_state = state_;

    if (state_ != TelemetryLifecycleState::revoked) {
        state_ = TelemetryLifecycleState::revoked;
        result.state_changed = true;
        result.purge_outbox = true;
    } else {
        result.state_changed = false;
    }
    result.current_state = state_;
    return result;
}

LifecycleTransitionResult ExamTelemetryLifecycle::on_service_restart(
    TelemetryLifecycleState persisted_state) noexcept {
    LifecycleTransitionResult result;
    result.previous_state = state_;

    switch (persisted_state) {
        case TelemetryLifecycleState::revoked:
        case TelemetryLifecycleState::enabled:
        case TelemetryLifecycleState::suspended:
        case TelemetryLifecycleState::disabled:
            break;
        default: {
            state_ = TelemetryLifecycleState::revoked;
            result.accepted = false;
            result.state_changed = (result.previous_state != state_);
            result.current_state = state_;
            result.clear_inflight = true;
            result.purge_outbox = false;
            return result;
        }
    }

    if (state_ == TelemetryLifecycleState::revoked) {
        result.accepted = true;
        result.state_changed = false;
        result.current_state = state_;
        result.clear_inflight = false;
        result.purge_outbox = false;
        return result;
    }

    result.accepted = true;
    switch (persisted_state) {
        case TelemetryLifecycleState::revoked:
            state_ = TelemetryLifecycleState::revoked;
            break;
        case TelemetryLifecycleState::enabled:
        case TelemetryLifecycleState::suspended:
            state_ = TelemetryLifecycleState::suspended;
            result.clear_inflight = true;
            break;
        case TelemetryLifecycleState::disabled:
            state_ = TelemetryLifecycleState::disabled;
            break;
        default:
            break;
    }
    result.state_changed = (result.previous_state != state_);
    result.current_state = state_;
    return result;
}

LifecycleTransitionResult ExamTelemetryLifecycle::dispatch_event(
    TelemetryLifecycleEvent event,
    const std::optional<exam::TelemetryEnable>& enable,
    TelemetryLifecycleState restart_persisted_state) noexcept {
    switch (event) {
        case TelemetryLifecycleEvent::enable:
            if (enable.has_value()) {
                return on_enable(*enable);
            }
            return {
                .accepted = false,
                .state_changed = false,
                .previous_state = state_,
                .current_state = state_,
                .clear_inflight = false,
                .purge_outbox = false,
            };
        case TelemetryLifecycleEvent::disconnect:
            return on_disconnect();
        case TelemetryLifecycleEvent::explicit_stop:
            return on_explicit_stop();
        case TelemetryLifecycleEvent::service_restart:
            return on_service_restart(restart_persisted_state);
    }
    return {
        .accepted = false,
        .state_changed = false,
        .previous_state = state_,
        .current_state = state_,
        .clear_inflight = false,
        .purge_outbox = false,
    };
}

} // namespace nstu::client
