#pragma once

#include "nstu/exam_telemetry.hpp"
#include "nstu/exam_telemetry_lifecycle.hpp"
#include "nstu/exam_telemetry_outbox.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::client {

inline constexpr std::size_t kMaximumLifetimeProducerGenerations = 16;
inline constexpr std::size_t kMaximumBatchSamplesLimit = 32;

// Current explicit-stop facts contain no identity, payload, or historical diagnostics.
enum class TelemetryStopApplicability : std::uint8_t { closed, unbound, applicable };
enum class TelemetryStopMemoryRevocation : std::uint8_t { not_applicable, revoked_now, already_revoked };
enum class TelemetryStopDurableWitness : std::uint8_t {
    not_attempted, protected_state_confirmed, refusal_marker_confirmed, unconfirmed
};
enum class TelemetryStopCleanup : std::uint8_t { not_needed, complete, pending };
enum class TelemetryStopError : std::uint8_t {
    none, protected_state_failed, revocation_witness_unconfirmed, cleanup_failed
};

struct TelemetryStopOutcome {
    TelemetryStopApplicability applicability = TelemetryStopApplicability::closed;
    TelemetryStopMemoryRevocation memory_revocation = TelemetryStopMemoryRevocation::not_applicable;
    TelemetryStopDurableWitness durable_witness = TelemetryStopDurableWitness::not_attempted;
    TelemetryStopCleanup cleanup = TelemetryStopCleanup::not_needed;
    TelemetryStopError error = TelemetryStopError::none;
};

// Disconnect observations include closed without changing the persisted lifecycle enum.
enum class TelemetryDisconnectState : std::uint8_t { closed, disabled, enabled, suspended, revoked };
enum class TelemetryDisconnectPending : std::uint8_t {
    not_present, retained, completed, revoked_expired, revoked_age_uncertain
};
enum class TelemetryDisconnectPublication : std::uint8_t {
    not_attempted, current_protected_state_confirmed, earlier_protected_snapshot_confirmed,
    refusal_marker_confirmed, unconfirmed
};
enum class TelemetryDisconnectCleanup : std::uint8_t { not_needed, complete, pending };
enum class TelemetryDisconnectError : std::uint8_t {
    none, protected_state_failed, revocation_witness_unconfirmed, cleanup_failed,
    pending_conflict, pending_enqueue_unfinished
};

struct TelemetryDisconnectOutcome {
    TelemetryDisconnectState before = TelemetryDisconnectState::closed;
    TelemetryDisconnectState after = TelemetryDisconnectState::closed;
    TelemetryDisconnectPending pending = TelemetryDisconnectPending::not_present;
    TelemetryDisconnectPublication publication = TelemetryDisconnectPublication::not_attempted;
    TelemetryDisconnectCleanup cleanup = TelemetryDisconnectCleanup::not_needed;
    TelemetryDisconnectError current_failure = TelemetryDisconnectError::none;
};

// Callable service coordinator that encapsulates telemetry batching,
// lifecycle, producer identity generation, monotonic sequencing, and
// outbox management without relying on global service variables.
class ExamTelemetryServiceCoordinator {
public:
    using Clock = std::function<std::uint64_t()>;
    ExamTelemetryServiceCoordinator();
    // Trusted local clock dependency; the default constructor uses real time.
    explicit ExamTelemetryServiceCoordinator(Clock clock);
    ~ExamTelemetryServiceCoordinator();
    ExamTelemetryServiceCoordinator(const ExamTelemetryServiceCoordinator&) = delete;
    ExamTelemetryServiceCoordinator& operator=(const ExamTelemetryServiceCoordinator&) = delete;
    ExamTelemetryServiceCoordinator(ExamTelemetryServiceCoordinator&&) = delete;
    ExamTelemetryServiceCoordinator& operator=(ExamTelemetryServiceCoordinator&&) = delete;

    // Opens the outbox and restores protected state, finishing any age-bounded intent.
    // v2 state without pending data migrates; legacy pending data has no trusted
    // intent-age anchor and requires explicit storage recovery.
    // Corrupt/legacy state or a missing state beside an existing outbox is refused
    // and preserved for explicit operator storage recovery.
    [[nodiscard]] bool open(const std::filesystem::path& outbox_path,
                            std::string* error = nullptr);
    // Pending storage failure is logged and leaves the coordinator open for retry.
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // Server enable arrived via authenticated command channel:
    // Binds context, transitions lifecycle to enabled. Reuses producer/sequence if
    // same active context, or initializes a new producer for a different exam after stop.
    [[nodiscard]] bool on_server_enable(const exam::TelemetryEnable& enable,
                                        std::string* error = nullptr);

    // Transport disconnected: suspends collection and clears outbox in-flight markers.
    // Entry/final authorization and entry-intent disposition are observed under one lock.
    // Publication confirms only this call's coordinator writer, not payload storage or
    // permanent durability. Later dirty serialized changes demote a successful state
    // write to an earlier snapshot; completed intent may still need completion publication.
    // Fresh marker success supersedes earlier state evidence; existing markers do not
    // confirm publication. Cleanup requires actual reopen/purge/marker removal completion.
    // Failure priority: witness uncertainty, state write, cleanup, conflict, enqueue unfinished.
    [[nodiscard]] TelemetryDisconnectOutcome on_disconnect_with_outcome() noexcept;
    // Compatibility entry point explicitly discards the current operation outcome.
    void on_disconnect() noexcept;

    // Service shutting down: suspends collection, retains active outbox.
    void on_service_shutdown() noexcept;

    // Explicit stop command received: revokes context and attempts durable cleanup.
    // Facts are observed in this call under the coordinator lock; cleanup may need retry.
    // Confirmed witnesses require this invocation's successful writer; an existing
    // refusal marker alone is unconfirmed. A later loss-counter write can fail,
    // leaving protected_state_confirmed together with protected_state_failed.
    // Cleanup is complete only after payload cleanup and marker removal clear
    // the pending cleanup state. Error priority is witness uncertainty, state write, cleanup.
    [[nodiscard]] TelemetryStopOutcome on_explicit_stop_with_outcome() noexcept;
    // Compatibility entry point explicitly discards the current operation outcome.
    void on_explicit_stop() noexcept;

    // Ingests raw samples from agent IPC:
    // Fails closed if not enabled. Chunks samples into batches of at most 32 samples,
    // stamps producer ID, monotonic sequence, timestamp, canonical hash, and enqueues to outbox.
    // If outbox is full, counts every dropped sample/chunk with saturated counters.
    // A pending chunk is retained on a storage failure; unauthorized input is counted
    // separately. Counters expire 30 days after the first record in their window.
    [[nodiscard]] bool ingest_samples(std::span<const exam::TelemetrySample> samples,
                                      std::uint64_t now_unix_ms = 0,
                                      std::string* error = nullptr);

    // Applies a server ACK to the outbox.
    [[nodiscard]] exam::TelemetryAckApplyResult on_server_ack(
        const exam::TelemetryAck& ack,
        std::uint64_t now_unix_ms = 0,
        std::string* error = nullptr);

    // Returns the next batch to send if lifecycle allows uploading (enabled state).
    [[nodiscard]] std::optional<exam::TelemetryBatch> next_batch_to_send(
        std::uint64_t now_unix_ms = 0,
        std::uint64_t inflight_timeout_ms = 5000);

    // State inspection returns snapshots; string copies may allocate.
    [[nodiscard]] TelemetryLifecycleState lifecycle_state() const noexcept;
    [[nodiscard]] TelemetryContextIdentity context() const;
    [[nodiscard]] exam::ProducerId active_producer_id() const noexcept;
    [[nodiscard]] std::uint64_t next_sequence() const noexcept;
    [[nodiscard]] std::uint32_t generation_count() const noexcept;
    [[nodiscard]] std::size_t outbox_size() const noexcept;
    [[nodiscard]] std::uint64_t dropped_sample_count() const noexcept;
    [[nodiscard]] std::uint64_t dropped_batch_count() const noexcept;
    [[nodiscard]] std::uint64_t not_authorized_sample_count() const noexcept;
    // Most recent bounded operational failure, also sent to the default log sink.
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] exam::TelemetryOutboxAggregateLoss aggregate_loss() const noexcept;

    // Retention prune:
    std::size_t prune_expired(std::uint64_t now_unix_ms);
    // Maintenance never grants upload authorization. Reports storage failure so
    // the service can call it periodically while disconnected or otherwise idle.
    [[nodiscard]] bool maintain(std::uint64_t now_unix_ms = 0,
                                std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::client
