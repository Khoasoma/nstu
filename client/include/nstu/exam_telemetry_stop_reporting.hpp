#pragma once

#include "nstu/audit.hpp"
#include "nstu/exam_telemetry_router.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace nstu::client {

// Describes the supplied current stop facts, never notice delivery or retention.
enum class TelemetryStopReportDisposition : std::uint8_t {
    complete, no_applicable_work, observed_failure, outcome_unavailable
};

// Owns trusted, nonempty synchronous callbacks. Empty callbacks are rejected with
// invalid_argument; an explicit no-op debug function is valid. The emitter inserts
// into its spool and returns the actual assigned sequence, propagating insertion
// exceptions rather than inventing success. Acceptance means bounded memory
// insertion only: ACK, eviction, process loss and durable delivery are separate.
//
// Callbacks run under this instance's mutex: they must not retain arguments,
// reenter this reporter or form a lock cycle. Captured dependencies must outlive
// the reporter and all calls. Snapshots stay immutable during each call; do not
// destroy the reporter while calls are active. Reporting order is mutex acquisition
// order, not source/coordinator chronology; callers needing chronology submit in
// that order. Failed insertion is eligible on a later observation, without an
// immediate retry, worker or service trigger. A sole observation may never recur.
class ExamTelemetryStopReporter {
public:
    using AuditEmitter = std::function<std::uint64_t(const audit::Event&)>;
    using DebugSink = std::function<void(const std::string&)>;

    ExamTelemetryStopReporter(AuditEmitter emitter, DebugSink debug);
    ExamTelemetryStopReporter(const ExamTelemetryStopReporter&) = delete;
    ExamTelemetryStopReporter& operator=(const ExamTelemetryStopReporter&) = delete;
    ExamTelemetryStopReporter(ExamTelemetryStopReporter&&) = delete;
    ExamTelemetryStopReporter& operator=(ExamTelemetryStopReporter&&) = delete;

    [[nodiscard]] TelemetryStopReportDisposition report(const TelemetryStopDispatchResult& snapshot) noexcept;

private:
    struct EpochKey {
        TelemetryStopDispatch dispatch = TelemetryStopDispatch::not_configured;
        bool has_outcome = false;
        TelemetryStopApplicability applicability = TelemetryStopApplicability::closed;
        TelemetryStopMemoryRevocation memory = TelemetryStopMemoryRevocation::not_applicable;
        TelemetryStopDurableWitness witness = TelemetryStopDurableWitness::not_attempted;
        TelemetryStopCleanup cleanup = TelemetryStopCleanup::not_needed;
        TelemetryStopError error = TelemetryStopError::none;
        bool operator==(const EpochKey&) const = default;
    };

    void reporting_exception_locked() noexcept;

    AuditEmitter emitter_;
    DebugSink debug_;
    std::mutex mutex_;
    std::optional<EpochKey> epoch_;
    bool debug_attempted_ = false;
    bool audit_accepted_ = false;
    bool emergency_attempted_ = false;
};

} // namespace nstu::client
