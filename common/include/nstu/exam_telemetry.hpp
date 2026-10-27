#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_sync.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::exam {

using ProducerId = std::array<std::byte, 16>;

inline constexpr std::size_t kMaximumTelemetrySampleBytes = 4u * 1024u;
inline constexpr std::size_t kMaximumTelemetryBatchBytes = 32u * 1024u;
inline constexpr std::size_t kMaximumSampleBytes = kMaximumTelemetrySampleBytes;
inline constexpr std::size_t kMaximumBatchBytes = kMaximumTelemetryBatchBytes;
inline constexpr std::size_t kMinimumBatchSamples = 1;
inline constexpr std::size_t kMaximumBatchSamples = 32;

inline constexpr std::size_t kTelemetrySampleFixedBytes = 56;
inline constexpr std::size_t kTelemetryBatchFixedBytes = 108;
inline constexpr std::size_t kTelemetryAckFixedBytes = 88;
inline constexpr std::size_t kTelemetryEnableFixedBytes = 84;
inline constexpr std::size_t kMaximumTelemetryEnableBytes = 16u * 1024u;

inline constexpr std::size_t kMaximumTelemetryOutboxBatches = 4096;
inline constexpr std::size_t kMaximumTelemetryOutboxBytes = 8u * 1024u * 1024u;

inline constexpr std::uint32_t kTelemetryRetentionDays = 30;
inline constexpr std::uint32_t kTelemetryMaxActiveContextHours = 25;

enum class TelemetrySampleKind : std::uint8_t {
    collector_started = 1,
    question_first_view = 2,
    question_dwell = 3,
    question_first_answer = 4,
    answer_change_summary = 5,
    focus_changed = 6,
    visibility_changed = 7,
    blocked_shortcut_summary = 8,
    exam_submitted = 9,
    loss_summary = 10,
    clock_discontinuity = 11,
};

enum class TelemetrySource : std::uint8_t {
    browser = 1,
    native = 2,
};

enum class TelemetryAckStatus : std::uint8_t {
    accepted = 1,
    duplicate = 2,
    rejected = 3,
    conflict = 4,
    unavailable = 5,
};

enum class TelemetryReason : std::uint32_t {
    none = 0,
    collector_initial = 1,
    collector_reload = 2,
    collector_late_enable = 3,
    navigation_button = 4,
    navigation_arrow = 5,
    navigation_initial = 6,
    discontinuity_monotonic = 7,
    discontinuity_wall_clock = 8,
    discontinuity_suspend = 9,
    focus_blur = 10,
    focus_gain = 11,
    visibility_hidden = 12,
    visibility_visible = 13,
    shortcut_system = 14,
    shortcut_devtools = 15,
    shortcut_clipboard = 16,
    submit_manual = 17,
    submit_timeout = 18,
    loss_queue_full = 19,
    loss_rate_limit = 20,
};

[[nodiscard]] constexpr bool valid_telemetry_reason(
    TelemetryReason reason) noexcept {
    const auto val = static_cast<std::uint32_t>(reason);
    return val <= static_cast<std::uint32_t>(TelemetryReason::loss_rate_limit);
}

struct TelemetrySample {
    TelemetrySampleKind kind = TelemetrySampleKind::collector_started;
    TelemetrySource source = TelemetrySource::native;
    std::uint64_t collector_id = 0;
    std::string question_id;
    std::uint32_t question_revision = 0;
    std::uint64_t monotonic_elapsed_milliseconds = 0;
    std::uint32_t duration_milliseconds = 0;
    std::uint64_t client_time_unix_milliseconds = 0;
    std::uint32_t numeric_value = 0;
    std::uint32_t count = 0;
    TelemetryReason reason = TelemetryReason::none;
};

struct TelemetryBatch {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
    ProducerId producer_id{};
    std::uint64_t sequence = 0;
    std::uint64_t first_enqueued_at_unix_milliseconds = 0;
    std::vector<TelemetrySample> samples;
};

struct TelemetryAck {
    SessionId session_id{};
    ProducerId producer_id{};
    std::uint64_t sequence = 0;
    security::Sha256Digest batch_hash{};
    std::uint64_t server_time_unix_milliseconds = 0;
    TelemetryAckStatus status = TelemetryAckStatus::rejected;
};

struct TelemetryEnable {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
    std::uint64_t server_time_unix_milliseconds = 0;
};

[[nodiscard]] bool validate_telemetry_sample(
    const TelemetrySample& sample) noexcept;
[[nodiscard]] std::vector<std::byte> encode_telemetry_sample(
    const TelemetrySample& sample);
[[nodiscard]] std::optional<TelemetrySample> decode_telemetry_sample(
    std::span<const std::byte> payload, std::string* error = nullptr);

[[nodiscard]] bool validate_telemetry_batch(
    const TelemetryBatch& batch) noexcept;
[[nodiscard]] std::vector<std::byte> encode_telemetry_batch(
    const TelemetryBatch& batch);
[[nodiscard]] std::optional<TelemetryBatch> decode_telemetry_batch(
    std::span<const std::byte> payload, std::string* error = nullptr);

[[nodiscard]] std::optional<security::Sha256Digest>
compute_telemetry_batch_hash(const TelemetryBatch& batch);

[[nodiscard]] bool validate_telemetry_ack(const TelemetryAck& ack) noexcept;
[[nodiscard]] std::vector<std::byte> encode_telemetry_ack(
    const TelemetryAck& ack);
[[nodiscard]] std::optional<TelemetryAck> decode_telemetry_ack(
    std::span<const std::byte> payload, std::string* error = nullptr);

[[nodiscard]] bool validate_telemetry_enable(
    const TelemetryEnable& enable) noexcept;
[[nodiscard]] std::vector<std::byte> encode_telemetry_enable(
    const TelemetryEnable& enable);
[[nodiscard]] std::optional<TelemetryEnable> decode_telemetry_enable(
    std::span<const std::byte> payload, std::string* error = nullptr);

} // namespace nstu::exam
