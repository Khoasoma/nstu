#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/exam_telemetry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nstu::client {

enum class TelemetryLifecycleState : std::uint8_t {
    disabled = 0,
    enabled = 1,
    suspended = 2,
    revoked = 3,
};

[[nodiscard]] constexpr std::string_view to_string(TelemetryLifecycleState state) noexcept {
    switch (state) {
        case TelemetryLifecycleState::disabled: return "disabled";
        case TelemetryLifecycleState::enabled: return "enabled";
        case TelemetryLifecycleState::suspended: return "suspended";
        case TelemetryLifecycleState::revoked: return "revoked";
    }
    return "unknown";
}

enum class TelemetryLifecycleEvent : std::uint8_t {
    enable = 0,
    disconnect = 1,
    explicit_stop = 2,
    service_restart = 3,
};

[[nodiscard]] constexpr std::string_view to_string(TelemetryLifecycleEvent event) noexcept {
    switch (event) {
        case TelemetryLifecycleEvent::enable: return "enable";
        case TelemetryLifecycleEvent::disconnect: return "disconnect";
        case TelemetryLifecycleEvent::explicit_stop: return "explicit_stop";
        case TelemetryLifecycleEvent::service_restart: return "service_restart";
    }
    return "unknown";
}

struct TelemetryContextIdentity {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    exam::SessionId session_id{};
    std::string candidate_id;

    [[nodiscard]] bool operator==(const TelemetryContextIdentity& other) const noexcept = default;

    [[nodiscard]] bool matches(const exam::TelemetryEnable& enable) const noexcept {
        return package_id == enable.package_id &&
               package_digest == enable.package_digest &&
               client_id == enable.client_id &&
               session_id == enable.session_id &&
               candidate_id == enable.candidate_id;
    }

    [[nodiscard]] bool matches(const exam::TelemetryBatch& batch) const noexcept {
        return package_id == batch.package_id &&
               package_digest == batch.package_digest &&
               client_id == batch.client_id &&
               session_id == batch.session_id &&
               candidate_id == batch.candidate_id;
    }

    [[nodiscard]] bool empty() const noexcept {
        return package_id.empty() && candidate_id.empty();
    }

    [[nodiscard]] bool bound() const noexcept {
        const auto has_nonzero = [](const auto& bytes) noexcept {
            return std::any_of(bytes.begin(), bytes.end(), [](std::byte b) noexcept {
                return b != std::byte{0};
            });
        };
        return !package_id.empty() &&
               !candidate_id.empty() &&
               has_nonzero(package_digest) &&
               has_nonzero(client_id) &&
               has_nonzero(session_id);
    }
};

struct LifecycleTransitionResult {
    bool accepted = false;
    bool state_changed = false;
    TelemetryLifecycleState previous_state = TelemetryLifecycleState::disabled;
    TelemetryLifecycleState current_state = TelemetryLifecycleState::disabled;
    bool clear_inflight = false;
    bool purge_outbox = false;
};

inline constexpr std::size_t kMaximumRevokedTelemetryIdentities = 16;

class ExamTelemetryLifecycle {
public:
    ExamTelemetryLifecycle() noexcept = default;

    explicit ExamTelemetryLifecycle(
        TelemetryContextIdentity context,
        TelemetryLifecycleState initial_state = TelemetryLifecycleState::disabled) noexcept;

    bool set_context(TelemetryContextIdentity context) noexcept;
    [[nodiscard]] const TelemetryContextIdentity& context() const noexcept { return context_; }

    [[nodiscard]] TelemetryLifecycleState state() const noexcept { return state_; }

    [[nodiscard]] bool is_disabled() const noexcept { return state_ == TelemetryLifecycleState::disabled; }
    [[nodiscard]] bool is_enabled() const noexcept { return state_ == TelemetryLifecycleState::enabled; }
    [[nodiscard]] bool is_suspended() const noexcept { return state_ == TelemetryLifecycleState::suspended; }
    [[nodiscard]] bool is_revoked() const noexcept { return state_ == TelemetryLifecycleState::revoked; }

    [[nodiscard]] bool can_collect() const noexcept { return state_ == TelemetryLifecycleState::enabled; }
    [[nodiscard]] bool can_upload() const noexcept { return state_ == TelemetryLifecycleState::enabled; }
    [[nodiscard]] bool should_retain_outbox() const noexcept { return state_ != TelemetryLifecycleState::revoked; }
    [[nodiscard]] bool allows_replay() const noexcept { return state_ != TelemetryLifecycleState::revoked; }

    [[nodiscard]] std::span<const TelemetryContextIdentity> revoked_identities() const noexcept {
        return std::span<const TelemetryContextIdentity>(revoked_.data(), revoked_count_);
    }

    [[nodiscard]] bool is_identity_revoked(const TelemetryContextIdentity& id) const noexcept;

    [[nodiscard]] LifecycleTransitionResult on_enable(const exam::TelemetryEnable& enable) noexcept;
    [[nodiscard]] LifecycleTransitionResult on_disconnect() noexcept;
    [[nodiscard]] LifecycleTransitionResult on_service_shutdown() noexcept;
    [[nodiscard]] LifecycleTransitionResult on_explicit_stop() noexcept;
    [[nodiscard]] LifecycleTransitionResult on_service_restart(
        TelemetryLifecycleState persisted_state) noexcept;

    [[nodiscard]] LifecycleTransitionResult dispatch_event(
        TelemetryLifecycleEvent event,
        const std::optional<exam::TelemetryEnable>& enable = std::nullopt,
        TelemetryLifecycleState restart_persisted_state = TelemetryLifecycleState::disabled) noexcept;

private:
    [[nodiscard]] bool archived(const TelemetryContextIdentity& id) const noexcept;
    void archive_current() noexcept;

    TelemetryContextIdentity context_{};
    TelemetryLifecycleState state_{TelemetryLifecycleState::disabled};
    std::array<TelemetryContextIdentity, kMaximumRevokedTelemetryIdentities> revoked_{};
    std::size_t revoked_count_ = 0;
};

} // namespace nstu::client
