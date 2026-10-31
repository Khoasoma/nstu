#include "nstu/exam_telemetry_disconnect_reporting.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace nstu::client {
namespace {
using Disposition = TelemetryDisconnectReportDisposition;
using State = TelemetryDisconnectState;
using Pending = TelemetryDisconnectPending;
using Publication = TelemetryDisconnectPublication;
using Cleanup = TelemetryDisconnectCleanup;
using Error = TelemetryDisconnectError;

Disposition classify(const TelemetryDisconnectDispatchResult& snapshot) noexcept {
    switch (snapshot.dispatch) {
    case TelemetryDisconnectDispatch::not_configured:
    case TelemetryDisconnectDispatch::legacy_callback_invoked:
        return Disposition::outcome_unavailable;
    case TelemetryDisconnectDispatch::outcome_callback_invoked: break;
    default: return Disposition::observed_failure;
    }
    if (!snapshot.outcome) return Disposition::outcome_unavailable;
    const auto& o = *snapshot.outcome;
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

const char* token(TelemetryDisconnectDispatch value) noexcept {
    switch (value) {
    case TelemetryDisconnectDispatch::not_configured: return "not_configured";
    case TelemetryDisconnectDispatch::legacy_callback_invoked: return "legacy_callback_invoked";
    case TelemetryDisconnectDispatch::outcome_callback_invoked: return "outcome_callback_invoked";
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

audit::Event notice(const TelemetryDisconnectDispatchResult& snapshot, Disposition disposition) {
    audit::Event event;
    event.category = audit::Category::exam;
    event.severity = audit::Severity::warning;
    event.component = "exam_telemetry";
    event.action = "disconnect";
    event.result = disposition == Disposition::outcome_unavailable ? "outcome_unavailable" : "failed";
    event.detail = std::string("dispatch=") + token(snapshot.dispatch);
    if (disposition == Disposition::outcome_unavailable || !snapshot.outcome) {
        event.detail += " outcome=unavailable";
    } else {
        const auto& o = *snapshot.outcome;
        // p= retains the longest publication token below the secret-like token
        // threshold, without changing the enum spelling or privacy sanitizer.
        event.detail += std::string(" before=") + token(o.before) + " after=" + token(o.after) +
            " pending=" + token(o.pending) + " p=" + token(o.publication) +
            " cleanup=" + token(o.cleanup) + " error=" + token(o.current_failure);
    }
    return audit::sanitize_event(event);
}
} // namespace

ExamTelemetryDisconnectReporter::ExamTelemetryDisconnectReporter(AuditEmitter emitter, DebugSink debug)
    : emitter_(std::move(emitter)), debug_(std::move(debug)) {
    if (!emitter_ || !debug_) throw std::invalid_argument("disconnect reporter requires nonempty sinks");
}
bool ExamTelemetryDisconnectReporter::EpochKey::operator==(const EpochKey& other) const noexcept {
    return dispatch == other.dispatch && has_outcome == other.has_outcome &&
        (!has_outcome || (outcome.before == other.outcome.before && outcome.after == other.outcome.after &&
         outcome.pending == other.outcome.pending && outcome.publication == other.outcome.publication &&
         outcome.cleanup == other.outcome.cleanup && outcome.current_failure == other.outcome.current_failure));
}
void ExamTelemetryDisconnectReporter::reporting_exception_locked() noexcept {
    if (emergency_attempted_) return;
    emergency_attempted_ = true;
    (void)std::fputs("NSTU exam telemetry disconnect: reporting exception\n", stderr);
}
TelemetryDisconnectReportDisposition ExamTelemetryDisconnectReporter::report(const TelemetryDisconnectDispatchResult& snapshot) noexcept {
    const auto disposition = classify(snapshot);
    if (disposition == Disposition::no_applicable_work) return disposition;
    try {
        const std::lock_guard lock(mutex_);
        if (disposition == Disposition::complete) {
            epoch_.reset();
            debug_attempted_ = audit_accepted_ = emergency_attempted_ = false;
            return disposition;
        }
        EpochKey key;
        key.dispatch = snapshot.dispatch;
        key.has_outcome = disposition != Disposition::outcome_unavailable && snapshot.outcome.has_value();
        if (key.has_outcome) key.outcome = *snapshot.outcome;
        if (!epoch_ || !(*epoch_ == key)) {
            epoch_ = key;
            debug_attempted_ = audit_accepted_ = emergency_attempted_ = false;
        }
        if (audit_accepted_) return disposition;
        try {
            const auto event = notice(snapshot, disposition);
            if (!debug_attempted_) {
                const auto line = "NSTU exam telemetry disconnect: " + event.result + " " + event.detail + "\n";
                debug_attempted_ = true;
                try { debug_(line); }
                catch (...) { reporting_exception_locked(); }
            }
            (void)emitter_(event);
            audit_accepted_ = true;
        } catch (...) { reporting_exception_locked(); }
    } catch (...) {
        // A failed lock cannot safely inspect or mutate the suppression epoch.
    }
    return disposition;
}
} // namespace nstu::client
