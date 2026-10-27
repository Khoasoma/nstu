#pragma once

#include "nstu/exam_telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nstu::exam {

inline constexpr std::uint64_t kTelemetryRetentionMilliseconds =
    static_cast<std::uint64_t>(kTelemetryRetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;

struct TelemetryOutboxAggregateLoss {
    SessionId session_id{};
    std::uint64_t rejected_batches = 0;
    std::uint64_t conflict_batches = 0;
    std::uint64_t expired_batches = 0;
    std::uint64_t first_recorded_unix_ms = 0;
    std::uint64_t last_updated_unix_ms = 0;
};

enum class TelemetryAckApplyResult : std::uint8_t {
    retired,            // accepted or duplicate with matching hash; removed from outbox
    deleted_terminal,   // rejected or conflict; deleted from outbox, incremented aggregate loss
    retry_scheduled,    // unavailable; backoff scheduled (2..60s)
    ignored_mismatch,   // no matching batch in outbox (e.g. unknown session/producer/sequence or hash mismatch)
    storage_failed,     // storage persistence failed; state rolled back
};

class TelemetryOutbox {
public:
    TelemetryOutbox();
    ~TelemetryOutbox();
    TelemetryOutbox(const TelemetryOutbox&) = delete;
    TelemetryOutbox& operator=(const TelemetryOutbox&) = delete;
    TelemetryOutbox(TelemetryOutbox&&) noexcept;
    TelemetryOutbox& operator=(TelemetryOutbox&&) noexcept;

    // Opens or creates a protected outbox. A proven decrypted integrity/format failure
    // quarantines the file; read/decrypt/storage failures leave it untouched and closed.
    // Old quarantined files expire; oversized or revoked files are never loaded.
    [[nodiscard]] bool open(const std::filesystem::path& path,
                            std::string* error = nullptr);
    // Explicit trusted local time for deterministic recovery/retention checks.
    // Uses the same validated load and automatic prune as the real-time overload.
    [[nodiscard]] bool open(const std::filesystem::path& path,
                            std::uint64_t now_unix_ms, std::string* error);
    // Releases payload memory. Bounded observed-expiry identities survive a
    // failed cleanup so reopening the same instance cannot renew expired data.
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // Enqueues a validated batch. Rejects if outbox would exceed 4096 batches
    // or 8 MiB, or if duplicate sequence exists for the same producer.
    [[nodiscard]] bool enqueue(const TelemetryBatch& batch,
                               std::string* error = nullptr);

    // Applies a server ACK:
    // - accepted: retire matching batch
    // - duplicate: retire only if exact durable identity & hash match
    // - rejected / conflict: delete payload immediately, increment aggregate loss
    // - unavailable: retry with exponential backoff 2..60s
    [[nodiscard]] TelemetryAckApplyResult acknowledge(
        const TelemetryAck& ack,
        std::uint64_t now_unix_ms = 0,
        std::string* error = nullptr);

    // Prunes expired payload/loss first, then marks an eligible batch in-flight.
    // A persistence failure returns no batch and leaves durable state retryable.
    // A protected sibling expiry barrier preserves observations across restart
    // and clock rollback if payload replacement fails. If every storage write
    // fails, durable observation is impossible and offers remain refused.
    [[nodiscard]] std::optional<TelemetryBatch> next_batch_to_send(
        std::uint64_t now_unix_ms = 0,
        std::uint64_t inflight_timeout_ms = 5000);

    // Clears in-flight markers on all batches (e.g. on disconnect or restart).
    void clear_inflight() noexcept;

    // Prunes batches at 30 days after the local arrival or an earlier valid
    // producer enqueue stamp. Increments per-context expired loss and returns count.
    std::size_t prune_expired(std::uint64_t now_unix_ms,
                              std::string* error = nullptr);

    // Purges all unsent batches immediately (e.g. on explicit stop revocation).
    // Preserves aggregate loss counters.
    bool purge(std::string* error = nullptr);

    // Clears all batches and resets aggregate loss counters.
    bool clear(std::string* error = nullptr);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::vector<TelemetryBatch> pending_batches() const;
    [[nodiscard]] TelemetryOutboxAggregateLoss aggregate_loss() const noexcept;
    [[nodiscard]] TelemetryOutboxAggregateLoss aggregate_loss(const SessionId& session_id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::exam
