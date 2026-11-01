#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/exam_telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace nstu::server {

inline constexpr std::size_t kDetailSegmentCount = 8;
inline constexpr std::size_t kDetailSegmentBytes = 8u * 1024u * 1024u; // 8 MiB
inline constexpr std::size_t kMaximumDetailBytes = kDetailSegmentCount * kDetailSegmentBytes; // 64 MiB

inline constexpr std::size_t kMaximumTrackedHashes = 262144; // <= 262,144 hashes
inline constexpr std::size_t kMaximumHashesPerProducer = 4096; // <= 4,096 hashes/producer
inline constexpr std::size_t kMaximumActiveContexts = 4096; // <= 4,096 contexts
inline constexpr std::size_t kMaximumIndexSizeBytes = 32u * 1024u * 1024u; // <= 32 MiB
inline constexpr std::size_t kMaximumLifetimeGenerationsPerContext = 16; // <= 16 generations

inline constexpr std::uint64_t kTelemetryRetentionMilliseconds =
    static_cast<std::uint64_t>(exam::kTelemetryRetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL; // 30 days
inline constexpr std::uint64_t kTelemetryMaxActiveContextMilliseconds =
    static_cast<std::uint64_t>(exam::kTelemetryMaxActiveContextHours) * 60ULL * 60ULL * 1000ULL; // 25 hours

class ExamTelemetryJournal {
public:
    ExamTelemetryJournal();
    ~ExamTelemetryJournal();
    ExamTelemetryJournal(const ExamTelemetryJournal&) = delete;
    ExamTelemetryJournal& operator=(const ExamTelemetryJournal&) = delete;
    ExamTelemetryJournal(ExamTelemetryJournal&&) noexcept;
    ExamTelemetryJournal& operator=(ExamTelemetryJournal&&) noexcept;

    [[nodiscard]] bool open(const std::filesystem::path& journal_dir,
                            std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& journal_dir,
                            std::uint64_t now_unix_ms,
                            std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] bool authorize_context(
        const exam::SessionId& session_id,
        std::uint64_t authorized_at_unix_ms,
        std::string* error = nullptr);

    [[nodiscard]] bool is_context_authorized(
        const exam::SessionId& session_id,
        std::uint64_t now_unix_ms = 0) const noexcept;

    [[nodiscard]] bool stop_authorization(
        const exam::SessionId& session_id,
        std::string* error = nullptr);

    [[nodiscard]] bool remove_context(
        const exam::SessionId& session_id,
        std::string* error = nullptr);

    [[nodiscard]] bool retire_producer(
        const exam::SessionId& session_id,
        const exam::ProducerId& producer_id,
        std::string* error = nullptr);

    [[nodiscard]] bool is_producer_admitted(
        const exam::SessionId& session_id,
        const exam::ProducerId& producer_id) const noexcept;

    [[nodiscard]] bool is_producer_retired(
        const exam::SessionId& session_id,
        const exam::ProducerId& producer_id) const noexcept;

    [[nodiscard]] std::uint32_t producer_generation(
        const exam::SessionId& session_id,
        const exam::ProducerId& producer_id) const noexcept;

    [[nodiscard]] std::uint64_t producer_replay_floor(
        const exam::SessionId& session_id,
        const exam::ProducerId& producer_id) const noexcept;

    [[nodiscard]] exam::TelemetryAck record_batch(
        const exam::TelemetryBatch& batch,
        std::uint64_t now_unix_ms,
        std::string* error = nullptr);

    [[nodiscard]] exam::TelemetryAck ingest(
        const exam::TelemetryBatch& batch,
        std::uint64_t now_unix_ms,
        std::string* error = nullptr) {
        return record_batch(batch, now_unix_ms, error);
    }

    std::size_t prune_expired(std::uint64_t now_unix_ms,
                              std::string* error = nullptr);

    void set_disk_failure_injection(bool enable) noexcept;
    void set_segment_size_limit_for_testing(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t active_context_count() const noexcept;
    [[nodiscard]] std::size_t tracked_hash_count() const noexcept;
    [[nodiscard]] std::size_t detail_bytes() const noexcept;
    [[nodiscard]] std::size_t index_bytes() const noexcept;
    [[nodiscard]] std::size_t active_segment_index() const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
