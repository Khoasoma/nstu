#include "nstu/exam_telemetry_stop_reporting.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace nstu::client {
namespace {
using Disposition = TelemetryStopReportDisposition;

Disposition classify(const TelemetryStopDispatchResult& snapshot) noexcept {
    switch (snapshot.dispatch) {
    case TelemetryStopDispatch::not_configured:
    case TelemetryStopDispatch::legacy_callback_invoked:
        return Disposition::outcome_unavailable;
    case TelemetryStopDispatch::outcome_callback_invoked:
        break;
    default:
        return Disposition::observed_failure;
    }
    if (!snapshot.outcome) return Disposition::outcome_unavailable;
    const auto& outcome = *snapshot.outcome;
    if (outcome.applicability == TelemetryStopApplicability::applicable &&
        (outcome.memory_revocation == TelemetryStopMemoryRevocation::revoked_now ||
         outcome.memory_revocation == TelemetryStopMemoryRevocation::already_revoked) &&
        outcome.durable_witness == TelemetryStopDurableWitness::protected_state_confirmed &&
        outcome.cleanup == TelemetryStopCleanup::complete && outcome.error == TelemetryStopError::none) {
        return Disposition::complete;
    }
    if ((outcome.applicability == TelemetryStopApplicability::closed ||
         outcome.applicability == TelemetryStopApplicability::unbound) &&
        outcome.memory_revocation == TelemetryStopMemoryRevocation::not_applicable &&
        outcome.durable_witness == TelemetryStopDurableWitness::not_attempted &&
        outcome.cleanup == TelemetryStopCleanup::not_needed && outcome.error == TelemetryStopError::none) {
        return Disposition::no_applicable_work;
    }
    return Disposition::observed_failure;
}

const char* token(TelemetryStopDispatch value) noexcept {
    switch (value) {
    case TelemetryStopDispatch::not_configured: return "not_configured";
    case TelemetryStopDispatch::legacy_callback_invoked: return "legacy_callback_invoked";
    case TelemetryStopDispatch::outcome_callback_invoked: return "outcome_callback_invoked";
    default: return "unknown";
    }
}
const char* token(TelemetryStopApplicability value) noexcept {
    switch (value) {
    case TelemetryStopApplicability::closed: return "closed";
    case TelemetryStopApplicability::unbound: return "unbound";
    case TelemetryStopApplicability::applicable: return "applicable";
    default: return "unknown";
    }
}
const char* token(TelemetryStopMemoryRevocation value) noexcept {
    switch (value) {
    case TelemetryStopMemoryRevocation::not_applicable: return "not_applicable";
    case TelemetryStopMemoryRevocation::revoked_now: return "revoked_now";
    case TelemetryStopMemoryRevocation::already_revoked: return "already_revoked";
    default: return "unknown";
    }
}
const char* token(TelemetryStopDurableWitness value) noexcept {
    switch (value) {
    case TelemetryStopDurableWitness::not_attempted: return "not_attempted";
    case TelemetryStopDurableWitness::protected_state_confirmed: return "protected_state_confirmed";
    case TelemetryStopDurableWitness::refusal_marker_confirmed: return "refusal_marker_confirmed";
    case TelemetryStopDurableWitness::unconfirmed: return "unconfirmed";
    default: return "unknown";
    }
}
const char* token(TelemetryStopCleanup value) noexcept {
    switch (value) {
    case TelemetryStopCleanup::not_needed: return "not_needed";
    case TelemetryStopCleanup::complete: return "complete";
    case TelemetryStopCleanup::pending: return "pending";
    default: return "unknown";
    }
}
const char* token(TelemetryStopError value) noexcept {
    switch (value) {
    case TelemetryStopError::none: return "none";
    case TelemetryStopError::protected_state_failed: return "protected_state_failed";
    case TelemetryStopError::revocation_witness_unconfirmed: return "revocation_witness_unconfirmed";
    case TelemetryStopError::cleanup_failed: return "cleanup_failed";
    default: return "unknown";
    }
}

audit::Event notice(const TelemetryStopDispatchResult& snapshot, Disposition disposition) {
    audit::Event event;
    event.category = audit::Category::exam;
    event.severity = audit::Severity::warning;
    event.component = "exam_telemetry";
    event.action = "explicit_stop";
    event.result = disposition == Disposition::outcome_unavailable ? "outcome_unavailable" : "failed";
    event.detail = std::string("dispatch=") + token(snapshot.dispatch);
    if (disposition == Disposition::outcome_unavailable) {
        event.detail += " outcome=unavailable";
    } else if (snapshot.outcome) {
        const auto& outcome = *snapshot.outcome;
        event.detail += std::string(" app=") + token(outcome.applicability) +
            " memory=" + token(outcome.memory_revocation) + " witness=" + token(outcome.durable_witness) +
            " cleanup=" + token(outcome.cleanup) + " error=" + token(outcome.error);
    } else {
        event.detail += " outcome=unavailable";
    }
    // Short witness= labels keep every known token below the sanitizer's
    // secret-like token threshold. Both sinks receive the sanitized event facts.
    return audit::sanitize_event(event);
}
} // namespace

ExamTelemetryStopReporter::ExamTelemetryStopReporter(AuditEmitter emitter, DebugSink debug)
    : emitter_(std::move(emitter)), debug_(std::move(debug)) {
    if (!emitter_ || !debug_) throw std::invalid_argument("stop reporter requires nonempty sinks");
}

void ExamTelemetryStopReporter::reporting_exception_locked() noexcept {
    if (emergency_attempted_) return;
    emergency_attempted_ = true;
    // Fixed, bounded, nonrecursive fallback; no callback, formatting or allocation.
    (void)std::fputs("NSTU exam telemetry stop: reporting exception\n", stderr);
}

TelemetryStopReportDisposition ExamTelemetryStopReporter::report(const TelemetryStopDispatchResult& snapshot) noexcept {
    // Pure current-fact classification precedes all allocation, locking and sinks.
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
        // Unavailable absent/legacy attachments never affect text or suppression.
        key.has_outcome = disposition != Disposition::outcome_unavailable && snapshot.outcome.has_value();
        if (key.has_outcome) {
            const auto& outcome = *snapshot.outcome;
            key.applicability = outcome.applicability;
            key.memory = outcome.memory_revocation;
            key.witness = outcome.durable_witness;
            key.cleanup = outcome.cleanup;
            key.error = outcome.error;
        }
        if (!epoch_ || *epoch_ != key) {
            epoch_ = key;
            debug_attempted_ = audit_accepted_ = emergency_attempted_ = false;
        }
        if (audit_accepted_) return disposition;
        try {
            const auto event = notice(snapshot, disposition);
            if (!debug_attempted_) {
                const auto line = "NSTU exam telemetry stop: " + event.result + " " + event.detail + "\n";
                debug_attempted_ = true;
                try { debug_(line); }
                catch (...) { reporting_exception_locked(); }
            }
            // A throwing debug sink does not skip the independent audit attempt.
            (void)emitter_(event);
            audit_accepted_ = true;
        } catch (...) {
            reporting_exception_locked();
        }
    } catch (...) {
        // Lock acquisition failure cannot safely inspect or mutate epoch state.
        // Omit the optional fallback on this path and preserve the classification.
    }
    return disposition;
}

} // namespace nstu::client
