#include "nstu/exam_telemetry_shutdown_close_reporting.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace nstu::client {
namespace {
using Disposition = TelemetryShutdownCloseReportDisposition;
using State = TelemetryDisconnectState;
using Pending = TelemetryDisconnectPending;
using Publication = TelemetryDisconnectPublication;
using Cleanup = TelemetryDisconnectCleanup;
using Error = TelemetryDisconnectError;

Disposition classify_shutdown(const TelemetryShutdownOutcome& o) noexcept {
    if (o.current_failure != Error::none) return Disposition::observed_failure;
    if (o.pending == Pending::not_present && o.cleanup == Cleanup::not_needed &&
        ((o.before == State::closed && o.after == State::closed && o.publication == Publication::not_attempted) ||
         (o.before == State::disabled && o.after == State::disabled && o.publication == Publication::current_protected_state_confirmed))) {
        return Disposition::no_applicable_work;
    }
    if (o.publication != Publication::current_protected_state_confirmed) return Disposition::observed_failure;
    const bool active_before = o.before == State::enabled || o.before == State::suspended;
    if ((active_before && o.after == State::suspended &&
         (o.pending == Pending::not_present || o.pending == Pending::completed) && o.cleanup == Cleanup::not_needed) ||
        (o.before == State::revoked && o.after == State::revoked && o.pending == Pending::not_present &&
         (o.cleanup == Cleanup::not_needed || o.cleanup == Cleanup::complete)) ||
        (active_before && o.after == State::revoked &&
         (o.pending == Pending::revoked_expired || o.pending == Pending::revoked_age_uncertain) && o.cleanup == Cleanup::complete)) {
        return Disposition::complete;
    }
    return Disposition::observed_failure;
}
Disposition classify_close(const TelemetryCloseOutcome& o) noexcept {
    if (o.current_failure != Error::none) return Disposition::observed_failure;
    if (o.before == State::closed && o.after == State::closed && o.pending == Pending::not_present &&
        o.publication == Publication::not_attempted && o.cleanup == Cleanup::not_needed) {
        return Disposition::no_applicable_work;
    }
    if (o.after != State::closed) return Disposition::observed_failure;
    const bool known_open_before = o.before == State::disabled || o.before == State::enabled ||
        o.before == State::suspended || o.before == State::revoked;
    const bool current_or_no_writer = o.publication == Publication::not_attempted ||
        o.publication == Publication::current_protected_state_confirmed;
    if ((known_open_before && o.pending == Pending::not_present && current_or_no_writer && o.cleanup == Cleanup::not_needed) ||
        ((o.before == State::enabled || o.before == State::suspended) && o.pending == Pending::completed &&
         o.publication == Publication::current_protected_state_confirmed && o.cleanup == Cleanup::not_needed) ||
        (o.before == State::revoked && o.pending == Pending::not_present && current_or_no_writer && o.cleanup == Cleanup::complete)) {
        return Disposition::complete;
    }
    return Disposition::observed_failure;
}
TelemetryShutdownCloseReportResult classify(const TelemetryShutdownDispatchResult& snapshot) noexcept {
    switch (snapshot.dispatch) {
    case TelemetryShutdownDispatch::not_configured:
    case TelemetryShutdownDispatch::legacy_callback_invoked: return {};
    case TelemetryShutdownDispatch::outcome_callback_invoked: break;
    default: return {Disposition::observed_failure, Disposition::observed_failure};
    }
    if (!snapshot.outcome) return {};
    return {classify_shutdown(snapshot.outcome->shutdown), classify_close(snapshot.outcome->close)};
}

const char* token(TelemetryShutdownDispatch value) noexcept {
    switch (value) {
    case TelemetryShutdownDispatch::not_configured: return "not_configured";
    case TelemetryShutdownDispatch::legacy_callback_invoked: return "legacy_callback_invoked";
    case TelemetryShutdownDispatch::outcome_callback_invoked: return "outcome_callback_invoked";
    default: return "unknown";
    }
}
const char* token(State value) noexcept {
    switch (value) {
    case State::closed: return "closed";
    case State::disabled: return "disabled";
    case State::enabled: return "enabled";
    case State::suspended: return "suspended";
    case State::revoked: return "revoked";
    default: return "unknown";
    }
}
const char* token(Pending value) noexcept {
    switch (value) {
    case Pending::not_present: return "not_present";
    case Pending::retained: return "retained";
    case Pending::completed: return "completed";
    case Pending::revoked_expired: return "revoked_expired";
    case Pending::revoked_age_uncertain: return "revoked_age_uncertain";
    default: return "unknown";
    }
}
const char* token(Publication value) noexcept {
    switch (value) {
    case Publication::not_attempted: return "not_attempted";
    case Publication::current_protected_state_confirmed: return "current_protected_state_confirmed";
    case Publication::earlier_protected_snapshot_confirmed: return "earlier_protected_snapshot_confirmed";
    case Publication::refusal_marker_confirmed: return "refusal_marker_confirmed";
    case Publication::unconfirmed: return "unconfirmed";
    default: return "unknown";
    }
}
const char* token(Cleanup value) noexcept {
    switch (value) {
    case Cleanup::not_needed: return "not_needed";
    case Cleanup::complete: return "complete";
    case Cleanup::pending: return "pending";
    default: return "unknown";
    }
}
const char* token(Error value) noexcept {
    switch (value) {
    case Error::none: return "none";
    case Error::protected_state_failed: return "protected_state_failed";
    case Error::revocation_witness_unconfirmed: return "revocation_witness_unconfirmed";
    case Error::cleanup_failed: return "cleanup_failed";
    case Error::pending_conflict: return "pending_conflict";
    case Error::pending_enqueue_unfinished: return "pending_enqueue_unfinished";
    default: return "unknown";
    }
}
audit::Event base_notice(const char* action, const char* result) {
    audit::Event event;
    event.category = audit::Category::exam;
    event.severity = audit::Severity::warning;
    event.component = "exam_telemetry";
    event.action = action;
    event.result = result;
    return event;
}
} // namespace

ExamTelemetryShutdownCloseReporter::ExamTelemetryShutdownCloseReporter(AuditEmitter emitter, DebugSink debug)
    : emitter_(std::move(emitter)), debug_(std::move(debug)) {
    if (!emitter_ || !debug_) throw std::invalid_argument("shutdown/close reporter requires nonempty sinks");
}
void ExamTelemetryShutdownCloseReporter::emergency_locked(Attempts& attempts, const char* text) noexcept {
    if (attempts.emergency_attempted) return;
    attempts.emergency_attempted = true;
    (void)std::fputs(text, stderr);
}
void ExamTelemetryShutdownCloseReporter::attempt_locked(const audit::Event& event, const char* prefix,
                                                       const char* emergency, Attempts& attempts) noexcept {
    // Debug construction/callback failure cannot skip this prepared Event's audit.
    if (!attempts.debug_attempted) {
        try {
            const auto line = std::string(prefix) + event.result + " " + event.detail + "\n";
            attempts.debug_attempted = true;
            debug_(line);
        } catch (...) { emergency_locked(attempts, emergency); }
    }
    try {
        (void)emitter_(event);
        attempts.audit_accepted = true;
    } catch (...) { emergency_locked(attempts, emergency); }
}
void ExamTelemetryShutdownCloseReporter::operation_locked(OperationEpoch& epoch, const OperationKey& key,
    Disposition disposition, const char* action, const char* prefix, const char* emergency) noexcept {
    if (disposition == Disposition::no_applicable_work) return;
    if (disposition == Disposition::complete) {
        epoch = {};
        return;
    }
    if (!epoch.key || !(*epoch.key == key)) {
        epoch.key = key;
        epoch.attempts = {};
    }
    if (epoch.attempts.audit_accepted) return;
    try {
        auto event = base_notice(action, "failed");
        // p= preserves the full longest enum below the sanitizer's 40-byte
        // secret-like assignment threshold. The two notices are separately bounded.
        event.detail = std::string("dispatch=") + token(key.dispatch) +
            " before=" + token(key.before) + " after=" + token(key.after) +
            " pending=" + token(key.pending) + " p=" + token(key.publication) +
            " cleanup=" + token(key.cleanup) + " error=" + token(key.current_failure);
        const auto sanitized = audit::sanitize_event(event);
        attempt_locked(sanitized, prefix, emergency, epoch.attempts);
    } catch (...) { emergency_locked(epoch.attempts, emergency); }
}
TelemetryShutdownCloseReportResult ExamTelemetryShutdownCloseReporter::report(const TelemetryShutdownDispatchResult& snapshot) noexcept {
    const auto result = classify(snapshot);
    try {
        const std::lock_guard lock(mutex_);
        if (snapshot.dispatch == TelemetryShutdownDispatch::outcome_callback_invoked && snapshot.outcome) {
            // Availability recovery never clears either operation's failure epoch.
            availability_ = {};
            const auto& s = snapshot.outcome->shutdown;
            const auto& c = snapshot.outcome->close;
            operation_locked(shutdown_, {s.before, s.after, s.pending, s.publication, s.cleanup, s.current_failure}, result.shutdown,
                "shutdown", "NSTU exam telemetry shutdown: ", "NSTU exam telemetry shutdown: reporting exception\n");
            operation_locked(close_, {c.before, c.after, c.pending, c.publication, c.cleanup, c.current_failure}, result.close,
                "close", "NSTU exam telemetry close: ", "NSTU exam telemetry close: reporting exception\n");
        } else {
            // Unavailable/unsupported dispatch ignores attachments and preserves operations.
            if (!availability_.key || *availability_.key != snapshot.dispatch) {
                availability_.key = snapshot.dispatch;
                availability_.attempts = {};
            }
            if (!availability_.attempts.audit_accepted) {
                constexpr auto emergency = "NSTU exam telemetry shutdown dispatch: reporting exception\n";
                try {
                    auto event = base_notice("shutdown_dispatch", result.shutdown == Disposition::outcome_unavailable ? "outcome_unavailable" : "failed");
                    event.detail = std::string("dispatch=") + token(snapshot.dispatch) + " outcome=unavailable";
                    const auto sanitized = audit::sanitize_event(event);
                    attempt_locked(sanitized, "NSTU exam telemetry shutdown dispatch: ", emergency, availability_.attempts);
                } catch (...) { emergency_locked(availability_.attempts, emergency); }
            }
        }
    } catch (...) {
        // A failed lock cannot safely inspect epochs or invoke their fallback.
    }
    return result;
}
} // namespace nstu::client
