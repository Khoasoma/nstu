#pragma once

#include "nstu/audit.hpp"
#include "nstu/exam_telemetry_router.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace nstu::client {

// Describes current supplied facts, independently of sink acceptance or delivery.
enum class TelemetryDisconnectReportDisposition {
    complete, no_applicable_work, observed_failure, outcome_unavailable
};

// Reports enum-only immutable disconnect snapshots; never invokes lifecycle work.
// Nonempty trusted synchronous sinks execute under this instance's mutex. They
// must not retain references, reenter this reporter or form lock cycles. Captured
// dependencies outlive the reporter and all calls; destruction during calls is
// forbidden. Ordering is mutex acquisition, not coordinator chronology: callers
// needing chronology submit in order. Audit success means returned memory-spool
// insertion, not durability/ACK/delivery. Throws leave the next observation retry
// eligible, without a worker or immediate retry; a sole observation may not recur.
class ExamTelemetryDisconnectReporter {
public:
    using AuditEmitter = std::function<std::uint64_t(const audit::Event&)>;
    using DebugSink = std::function<void(const std::string&)>;

    ExamTelemetryDisconnectReporter(AuditEmitter emitter, DebugSink debug);
    ExamTelemetryDisconnectReporter(const ExamTelemetryDisconnectReporter&) = delete;
    ExamTelemetryDisconnectReporter& operator=(const ExamTelemetryDisconnectReporter&) = delete;
    ExamTelemetryDisconnectReporter(ExamTelemetryDisconnectReporter&&) = delete;
    ExamTelemetryDisconnectReporter& operator=(ExamTelemetryDisconnectReporter&&) = delete;

    // Closed/unbound canonical observations preserve suppression; completed
    // current work resets it. Current failures never consult sticky last_error.
    [[nodiscard]] TelemetryDisconnectReportDisposition report(const TelemetryDisconnectDispatchResult& snapshot) noexcept;

private:
    struct EpochKey {
        TelemetryDisconnectDispatch dispatch = TelemetryDisconnectDispatch::not_configured;
        bool has_outcome = false;
        TelemetryDisconnectOutcome outcome;
        bool operator==(const EpochKey& other) const noexcept;
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
