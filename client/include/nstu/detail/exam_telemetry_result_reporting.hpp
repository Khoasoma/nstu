#pragma once

#include "nstu/audit.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nstu::client::detail {

// Private service result vocabulary; preserves the original six positions.
enum class TelemetryResultOperation : std::size_t {
    startup, enable, samples, ack, maintenance, shutdown, count
};

// Fixed private callable shared only by the service result wrapper and its test.
// Trusted nonempty synchronous sinks run under this mutex and must not retain
// references, reenter or form lock cycles. Dependencies outlive all calls;
// destruction during calls is forbidden. Only bounded canonical detail keys
// are retained. An emitter return confirms memory insertion, not durability or
// retained delivery. A throw leaves the next identical observation eligible;
// no worker, immediate retry or arbitrary Spool rollback is promised.
class ExamTelemetryResultReporter {
public:
    using AuditEmitter = std::function<std::uint64_t(const audit::Event&)>;
    using DebugSink = std::function<void(const std::string&)>;

    ExamTelemetryResultReporter(AuditEmitter emitter, DebugSink debug)
        : emitter_(std::move(emitter)), debug_(std::move(debug)) {
        if (!emitter_ || !debug_) throw std::invalid_argument("result reporter requires nonempty sinks");
    }
    ExamTelemetryResultReporter(const ExamTelemetryResultReporter&) = delete;
    ExamTelemetryResultReporter& operator=(const ExamTelemetryResultReporter&) = delete;
    ExamTelemetryResultReporter(ExamTelemetryResultReporter&&) = delete;
    ExamTelemetryResultReporter& operator=(ExamTelemetryResultReporter&&) = delete;

    void report(TelemetryResultOperation operation, bool succeeded, std::string_view error) noexcept {
        const auto index = static_cast<std::size_t>(operation);
        if (index >= actions_.size()) return;
        try {
            if (succeeded) {
                const std::lock_guard lock(mutex_);
                auto& slot = slots_[index];
                slot.detail.clear();
                slot.has_key = slot.accepted = false;
                return;
            }
            audit::Event event;
            event.category = audit::Category::exam;
            event.severity = audit::Severity::warning;
            event.component = "exam_telemetry";
            event.action = actions_[index];
            event.result = "failed";
            event.detail = audit::sanitize_audit_field(error.empty() ? "operation failed" : error,
                                                       audit::kMaximumDetailBytes);
            event = audit::sanitize_event(event);
            // Prepare the full canonical key before changing any existing slot.
            auto key = event.detail;
            const std::lock_guard lock(mutex_);
            auto& slot = slots_[index];
            if (!slot.has_key || slot.detail != key) {
                slot.detail.swap(key);
                slot.has_key = true;
                slot.accepted = false;
            }
            if (slot.accepted) return;
            // Keep audit-before-debug ordering. Failed audit skips debug and
            // cannot certify insertion; later identical observations may retry.
            (void)emitter_(event);
            slot.accepted = true;
            const auto line = "NSTU exam telemetry " + event.action + " failed: " + event.detail + "\n";
            debug_(line);
        } catch (...) {
            // This independent flag never inspects acceptance slots without
            // their mutex and never recurses through the provided callbacks.
            if (!reporting_failed_.test_and_set(std::memory_order_relaxed)) {
                (void)std::fputs("NSTU telemetry failure reporting unavailable.\n", stderr);
            }
        }
    }

private:
    struct Slot {
        std::string detail;
        bool has_key = false;
        bool accepted = false;
    };
    inline static constexpr std::array<std::string_view, 6> actions_{
        "startup", "enable", "samples", "ack", "maintenance", "shutdown"};
    AuditEmitter emitter_;
    DebugSink debug_;
    std::mutex mutex_;
    std::array<Slot, actions_.size()> slots_;
    std::atomic_flag reporting_failed_ = ATOMIC_FLAG_INIT;
};
} // namespace nstu::client::detail
