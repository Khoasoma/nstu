#pragma once

#include "nstu/audit.hpp"
#include "nstu/exam_telemetry_router.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace nstu::client {

enum class TelemetryShutdownCloseReportDisposition : std::uint8_t {
    complete, no_applicable_work, observed_failure, outcome_unavailable
};
struct TelemetryShutdownCloseReportResult {
    TelemetryShutdownCloseReportDisposition shutdown = TelemetryShutdownCloseReportDisposition::outcome_unavailable;
    TelemetryShutdownCloseReportDisposition close = TelemetryShutdownCloseReportDisposition::outcome_unavailable;
};

// Observes two independent current snapshots; never invokes coordinator work.
// Trusted nonempty synchronous sinks execute under this reporter's mutex and
// must not retain references, reenter it or form lock cycles. Dependencies must
// outlive all calls; destruction during calls is forbidden. Mutex order does
// not establish source chronology or a shared coordinator context/transaction.
// Returned dispositions are independent from sink acceptance. An assigned
// Spool sequence means memory insertion only; capacity-one can evict shutdown
// during the close insertion. ACK/eviction does not reset suppression. A throw
// leaves the next observation eligible without a worker or sole-notice retry.
class ExamTelemetryShutdownCloseReporter {
public:
    using AuditEmitter = std::function<std::uint64_t(const audit::Event&)>;
    using DebugSink = std::function<void(const std::string&)>;
    ExamTelemetryShutdownCloseReporter(AuditEmitter emitter, DebugSink debug);
    ExamTelemetryShutdownCloseReporter(const ExamTelemetryShutdownCloseReporter&) = delete;
    ExamTelemetryShutdownCloseReporter& operator=(const ExamTelemetryShutdownCloseReporter&) = delete;
    ExamTelemetryShutdownCloseReporter(ExamTelemetryShutdownCloseReporter&&) = delete;
    ExamTelemetryShutdownCloseReporter& operator=(ExamTelemetryShutdownCloseReporter&&) = delete;

    [[nodiscard]] TelemetryShutdownCloseReportResult report(const TelemetryShutdownDispatchResult& snapshot) noexcept;

private:
    struct OperationKey {
        TelemetryDisconnectState before;
        TelemetryDisconnectState after;
        TelemetryDisconnectPending pending;
        TelemetryDisconnectPublication publication;
        TelemetryDisconnectCleanup cleanup;
        TelemetryDisconnectError current_failure;
        TelemetryShutdownDispatch dispatch = TelemetryShutdownDispatch::outcome_callback_invoked;
        bool operator==(const OperationKey&) const noexcept = default;
    };
    struct Attempts {
        bool debug_attempted = false;
        bool audit_accepted = false;
        bool emergency_attempted = false;
    };
    struct OperationEpoch {
        std::optional<OperationKey> key;
        Attempts attempts;
    };
    struct AvailabilityEpoch {
        std::optional<TelemetryShutdownDispatch> key;
        Attempts attempts;
    };
    void emergency_locked(Attempts& attempts, const char* text) noexcept;
    void attempt_locked(const audit::Event& event, const char* prefix,
                        const char* emergency, Attempts& attempts) noexcept;
    void operation_locked(OperationEpoch& epoch, const OperationKey& key,
                          TelemetryShutdownCloseReportDisposition disposition,
                          const char* action, const char* prefix, const char* emergency) noexcept;
    AuditEmitter emitter_;
    DebugSink debug_;
    std::mutex mutex_;
    OperationEpoch shutdown_;
    OperationEpoch close_;
    AvailabilityEpoch availability_;
};
} // namespace nstu::client
