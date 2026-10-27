#include "nstu/exam_telemetry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace nstu::exam {
namespace {

inline constexpr std::uint32_t kSampleMagic = 0x3153544eu; // "NTS1"
inline constexpr std::uint32_t kBatchMagic = 0x3154424eu;  // "NBT1"
inline constexpr std::uint32_t kAckMagic = 0x3141544eu;    // "NTA1"
inline constexpr std::uint32_t kEnableMagic = 0x3145454eu; // "NEE1"
inline constexpr std::uint16_t kWireVersion = 1;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    const auto offset = output.size();
    output.resize(offset + sizeof(T));
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output[offset + index] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || sizeof(T) > input.size() - offset) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

template <std::size_t N>
void append_array(std::vector<std::byte>& output,
                  const std::array<std::byte, N>& array) {
    output.insert(output.end(), array.begin(), array.end());
}

template <std::size_t N>
bool read_array(std::span<const std::byte> input, std::size_t& offset,
                std::array<std::byte, N>& array) {
    if (offset > input.size() || N > input.size() - offset) {
        return false;
    }
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), N,
                array.begin());
    offset += N;
    return true;
}

bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::byte b) { return b == std::byte{0}; });
}

bool is_valid_utf8(std::string_view str) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(str.data());
    const std::size_t len = str.size();
    std::size_t i = 0;
    while (i < len) {
        if (bytes[i] == 0) {
            return false;
        }
        if (bytes[i] <= 0x7f) {
            i += 1;
        } else if ((bytes[i] & 0xe0) == 0xc0) {
            if (i + 1 >= len || (bytes[i + 1] & 0xc0) != 0x80) return false;
            if (bytes[i] < 0xc2) return false;
            i += 2;
        } else if ((bytes[i] & 0xf0) == 0xe0) {
            if (i + 2 >= len || (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80) {
                return false;
            }
            if (bytes[i] == 0xe0 && bytes[i + 1] < 0xa0) return false;
            if (bytes[i] == 0xed && bytes[i + 1] >= 0xa0) return false;
            i += 3;
        } else if ((bytes[i] & 0xf8) == 0xf0) {
            if (i + 3 >= len || (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80 ||
                (bytes[i + 3] & 0xc0) != 0x80) {
                return false;
            }
            if (bytes[i] == 0xf0 && bytes[i + 1] < 0x90) return false;
            if (bytes[i] == 0xf4 && bytes[i + 1] > 0x8f) return false;
            if (bytes[i] > 0xf4) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool valid_text(std::string_view value, std::size_t maximum,
                bool allow_empty = false) noexcept {
    if (value.empty()) {
        return allow_empty;
    }
    if (value.size() > maximum) {
        return false;
    }
    return is_valid_utf8(value);
}

bool valid_sample_kind(TelemetrySampleKind kind) noexcept {
    const auto val = static_cast<std::uint8_t>(kind);
    return val >= static_cast<std::uint8_t>(TelemetrySampleKind::collector_started) &&
           val <= static_cast<std::uint8_t>(TelemetrySampleKind::clock_discontinuity);
}

bool valid_source(TelemetrySource source) noexcept {
    const auto val = static_cast<std::uint8_t>(source);
    return val == static_cast<std::uint8_t>(TelemetrySource::browser) ||
           val == static_cast<std::uint8_t>(TelemetrySource::native);
}

bool valid_ack_status(TelemetryAckStatus status) noexcept {
    const auto val = static_cast<std::uint8_t>(status);
    return val >= static_cast<std::uint8_t>(TelemetryAckStatus::accepted) &&
           val <= static_cast<std::uint8_t>(TelemetryAckStatus::unavailable);
}

void append_string(std::vector<std::byte>& output, std::string_view str) {
    append_le(output, static_cast<std::uint16_t>(str.size()));
    for (char ch : str) {
        output.push_back(static_cast<std::byte>(ch));
    }
}

bool read_string(std::span<const std::byte> input, std::size_t& offset,
                 std::string& str, std::size_t max_bytes,
                 bool allow_empty = false) {
    std::uint16_t len = 0;
    if (!read_le(input, offset, len)) {
        return false;
    }
    if (len > max_bytes || (!allow_empty && len == 0)) {
        return false;
    }
    if (offset > input.size() || len > input.size() - offset) {
        return false;
    }
    str.assign(reinterpret_cast<const char*>(input.data() + offset), len);
    offset += len;
    return valid_text(str, max_bytes, allow_empty);
}

} // namespace

bool validate_telemetry_sample(const TelemetrySample& sample) noexcept {
    if (!valid_sample_kind(sample.kind) || !valid_source(sample.source) ||
        !valid_telemetry_reason(sample.reason)) {
        return false;
    }
    if (sample.collector_id == 0 ||
        sample.client_time_unix_milliseconds == 0) {
        return false;
    }
    if (sample.question_id.empty()) {
        if (sample.question_revision != 0) {
            return false;
        }
    } else {
        if (sample.question_revision == 0) {
            return false;
        }
        if (!valid_text(sample.question_id, kMaximumQuestionIdBytes,
                        /*allow_empty=*/false)) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> encode_telemetry_sample(
    const TelemetrySample& sample) {
    if (!validate_telemetry_sample(sample)) {
        return {};
    }

    std::vector<std::byte> wire;
    wire.reserve(64 + sample.question_id.size());

    append_le(wire, kSampleMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint8_t>(sample.kind));
    append_le(wire, static_cast<std::uint8_t>(sample.source));
    append_le(wire, static_cast<std::uint16_t>(0)); // reserved
    append_le(wire, sample.collector_id);
    append_le(wire, sample.monotonic_elapsed_milliseconds);
    append_le(wire, sample.duration_milliseconds);
    append_le(wire, sample.client_time_unix_milliseconds);
    append_le(wire, sample.numeric_value);
    append_le(wire, sample.count);
    append_le(wire, static_cast<std::uint32_t>(sample.reason));
    append_le(wire, sample.question_revision);
    append_string(wire, sample.question_id);

    if (wire.size() > kMaximumTelemetrySampleBytes) {
        return {};
    }
    return wire;
}

std::optional<TelemetrySample> decode_telemetry_sample(
    std::span<const std::byte> payload, std::string* error) {
    if (payload.size() < kTelemetrySampleFixedBytes ||
        payload.size() > kMaximumTelemetrySampleBytes) {
        set_error(error, "invalid telemetry sample payload size");
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t raw_kind = 0;
    std::uint8_t raw_source = 0;
    std::uint16_t reserved = 0;

    if (!read_le(payload, offset, magic) || magic != kSampleMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, raw_kind) ||
        !read_le(payload, offset, raw_source) ||
        !read_le(payload, offset, reserved) || reserved != 0) {
        set_error(error, "invalid sample header, version, or reserved bits");
        return std::nullopt;
    }

    TelemetrySample sample;
    sample.kind = static_cast<TelemetrySampleKind>(raw_kind);
    sample.source = static_cast<TelemetrySource>(raw_source);

    std::uint32_t raw_reason = 0;
    if (!read_le(payload, offset, sample.collector_id) ||
        !read_le(payload, offset, sample.monotonic_elapsed_milliseconds) ||
        !read_le(payload, offset, sample.duration_milliseconds) ||
        !read_le(payload, offset, sample.client_time_unix_milliseconds) ||
        !read_le(payload, offset, sample.numeric_value) ||
        !read_le(payload, offset, sample.count) ||
        !read_le(payload, offset, raw_reason) ||
        !valid_telemetry_reason(static_cast<TelemetryReason>(raw_reason)) ||
        !read_le(payload, offset, sample.question_revision) ||
        !read_string(payload, offset, sample.question_id,
                     kMaximumQuestionIdBytes, /*allow_empty=*/true)) {
        set_error(error, "malformed sample fields or question_id");
        return std::nullopt;
    }
    sample.reason = static_cast<TelemetryReason>(raw_reason);

    if (offset != payload.size()) {
        set_error(error, "trailing bytes in sample payload");
        return std::nullopt;
    }

    if (!validate_telemetry_sample(sample)) {
        set_error(error, "telemetry sample validation failed");
        return std::nullopt;
    }

    return sample;
}

bool validate_telemetry_batch(const TelemetryBatch& batch) noexcept {
    if (batch.sequence == 0 ||
        batch.first_enqueued_at_unix_milliseconds == 0) {
        return false;
    }
    if (all_zero(batch.package_digest) || all_zero(batch.client_id) ||
        all_zero(batch.session_id) || all_zero(batch.producer_id)) {
        return false;
    }
    if (!valid_text(batch.package_id, kMaximumPackageIdBytes) ||
        !valid_text(batch.candidate_id, kMaximumCandidateIdBytes)) {
        return false;
    }
    if (batch.samples.size() < kMinimumBatchSamples ||
        batch.samples.size() > kMaximumBatchSamples) {
        return false;
    }
    for (const auto& sample : batch.samples) {
        if (!validate_telemetry_sample(sample)) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> encode_telemetry_batch(
    const TelemetryBatch& batch) {
    if (!validate_telemetry_batch(batch)) {
        return {};
    }

    std::vector<std::byte> wire;
    wire.reserve(256);

    append_le(wire, kBatchMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint16_t>(batch.samples.size()));
    append_le(wire, batch.sequence);
    append_le(wire, batch.first_enqueued_at_unix_milliseconds);
    append_array(wire, batch.package_digest);
    append_array(wire, batch.client_id);
    append_array(wire, batch.session_id);
    append_array(wire, batch.producer_id);
    append_string(wire, batch.package_id);
    append_string(wire, batch.candidate_id);

    for (const auto& sample : batch.samples) {
        const auto sample_wire = encode_telemetry_sample(sample);
        if (sample_wire.empty() ||
            sample_wire.size() > kMaximumTelemetrySampleBytes) {
            return {};
        }
        append_le(wire, static_cast<std::uint16_t>(sample_wire.size()));
        wire.insert(wire.end(), sample_wire.begin(), sample_wire.end());
    }

    if (wire.size() > kMaximumTelemetryBatchBytes) {
        return {};
    }
    return wire;
}

std::optional<TelemetryBatch> decode_telemetry_batch(
    std::span<const std::byte> payload, std::string* error) {
    if (payload.size() < kTelemetryBatchFixedBytes + 2 + sizeof(std::uint16_t) +
                             kTelemetrySampleFixedBytes ||
        payload.size() > kMaximumTelemetryBatchBytes) {
        set_error(error, "invalid batch payload size");
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t sample_count = 0;

    if (!read_le(payload, offset, magic) || magic != kBatchMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, sample_count) ||
        sample_count < kMinimumBatchSamples ||
        sample_count > kMaximumBatchSamples) {
        set_error(error, "invalid batch header, version, or sample count");
        return std::nullopt;
    }

    TelemetryBatch batch;
    if (!read_le(payload, offset, batch.sequence) ||
        !read_le(payload, offset, batch.first_enqueued_at_unix_milliseconds) ||
        !read_array(payload, offset, batch.package_digest) ||
        !read_array(payload, offset, batch.client_id) ||
        !read_array(payload, offset, batch.session_id) ||
        !read_array(payload, offset, batch.producer_id) ||
        !read_string(payload, offset, batch.package_id,
                     kMaximumPackageIdBytes) ||
        !read_string(payload, offset, batch.candidate_id,
                     kMaximumCandidateIdBytes)) {
        set_error(error, "malformed batch header identity fields");
        return std::nullopt;
    }

    batch.samples.reserve(sample_count);
    for (std::uint16_t i = 0; i < sample_count; ++i) {
        std::uint16_t sample_len = 0;
        if (!read_le(payload, offset, sample_len) ||
            sample_len > kMaximumTelemetrySampleBytes ||
            offset > payload.size() || sample_len > payload.size() - offset) {
            set_error(error, "malformed sample size in batch");
            return std::nullopt;
        }

        const auto sample_span = payload.subspan(offset, sample_len);
        offset += sample_len;

        std::string sample_error;
        auto sample = decode_telemetry_sample(sample_span, &sample_error);
        if (!sample.has_value()) {
            set_error(error, sample_error.c_str());
            return std::nullopt;
        }
        batch.samples.push_back(std::move(*sample));
    }

    if (offset != payload.size()) {
        set_error(error, "trailing bytes in batch payload");
        return std::nullopt;
    }

    if (!validate_telemetry_batch(batch)) {
        set_error(error, "batch validation failed");
        return std::nullopt;
    }

    return batch;
}

std::optional<security::Sha256Digest> compute_telemetry_batch_hash(
    const TelemetryBatch& batch) {
    const auto wire = encode_telemetry_batch(batch);
    if (wire.empty()) {
        return std::nullopt;
    }
    return security::sha256(wire);
}

bool validate_telemetry_ack(const TelemetryAck& ack) noexcept {
    if (!valid_ack_status(ack.status)) {
        return false;
    }
    if (ack.sequence == 0 ||
        ack.server_time_unix_milliseconds == 0) {
        return false;
    }
    if (all_zero(ack.session_id) || all_zero(ack.producer_id)) {
        return false;
    }
    return true;
}

std::vector<std::byte> encode_telemetry_ack(const TelemetryAck& ack) {
    if (!validate_telemetry_ack(ack)) {
        return {};
    }

    std::vector<std::byte> wire;
    wire.reserve(kTelemetryAckFixedBytes);

    append_le(wire, kAckMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint8_t>(ack.status));
    append_le(wire, static_cast<std::uint8_t>(0)); // reserved
    append_le(wire, ack.sequence);
    append_le(wire, ack.server_time_unix_milliseconds);
    append_array(wire, ack.session_id);
    append_array(wire, ack.producer_id);
    append_array(wire, ack.batch_hash);

    return wire;
}

std::optional<TelemetryAck> decode_telemetry_ack(
    std::span<const std::byte> payload, std::string* error) {
    if (payload.size() != kTelemetryAckFixedBytes) {
        set_error(error, "invalid telemetry ack payload size");
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t raw_status = 0;
    std::uint8_t reserved = 0;

    if (!read_le(payload, offset, magic) || magic != kAckMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, raw_status) ||
        !read_le(payload, offset, reserved) || reserved != 0) {
        set_error(error, "invalid ack header, version, or reserved bits");
        return std::nullopt;
    }

    TelemetryAck ack;
    ack.status = static_cast<TelemetryAckStatus>(raw_status);

    if (!read_le(payload, offset, ack.sequence) ||
        !read_le(payload, offset, ack.server_time_unix_milliseconds) ||
        !read_array(payload, offset, ack.session_id) ||
        !read_array(payload, offset, ack.producer_id) ||
        !read_array(payload, offset, ack.batch_hash)) {
        set_error(error, "malformed ack fields");
        return std::nullopt;
    }

    if (offset != payload.size()) {
        set_error(error, "trailing bytes in ack payload");
        return std::nullopt;
    }

    if (!validate_telemetry_ack(ack)) {
        set_error(error, "ack validation failed");
        return std::nullopt;
    }

    return ack;
}

bool validate_telemetry_enable(const TelemetryEnable& enable) noexcept {
    if (all_zero(enable.package_digest) || all_zero(enable.client_id) ||
        all_zero(enable.session_id)) {
        return false;
    }
    if (!valid_text(enable.package_id, kMaximumPackageIdBytes) ||
        !valid_text(enable.candidate_id, kMaximumCandidateIdBytes)) {
        return false;
    }
    return true;
}

std::vector<std::byte> encode_telemetry_enable(
    const TelemetryEnable& enable) {
    if (!validate_telemetry_enable(enable)) {
        return {};
    }

    std::vector<std::byte> wire;
    wire.reserve(kTelemetryEnableFixedBytes + enable.package_id.size() +
                 enable.candidate_id.size());

    append_le(wire, kEnableMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint16_t>(0)); // reserved
    append_le(wire, enable.server_time_unix_milliseconds);
    append_array(wire, enable.package_digest);
    append_array(wire, enable.client_id);
    append_array(wire, enable.session_id);
    append_string(wire, enable.package_id);
    append_string(wire, enable.candidate_id);

    return wire;
}

std::optional<TelemetryEnable> decode_telemetry_enable(
    std::span<const std::byte> payload, std::string* error) {
    // 84 fixed bytes + at least 1 byte each for package_id and candidate_id = 86 bytes
    if (payload.size() < kTelemetryEnableFixedBytes + 2 ||
        payload.size() > kMaximumTelemetryEnableBytes) {
        set_error(error, "invalid enable payload size");
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;

    if (!read_le(payload, offset, magic) || magic != kEnableMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, reserved) || reserved != 0) {
        set_error(error, "invalid enable header, version, or reserved bits");
        return std::nullopt;
    }

    TelemetryEnable enable;
    if (!read_le(payload, offset, enable.server_time_unix_milliseconds) ||
        !read_array(payload, offset, enable.package_digest) ||
        !read_array(payload, offset, enable.client_id) ||
        !read_array(payload, offset, enable.session_id) ||
        !read_string(payload, offset, enable.package_id,
                     kMaximumPackageIdBytes) ||
        !read_string(payload, offset, enable.candidate_id,
                     kMaximumCandidateIdBytes)) {
        set_error(error, "malformed enable identity fields");
        return std::nullopt;
    }

    if (offset != payload.size()) {
        set_error(error, "trailing bytes in enable payload");
        return std::nullopt;
    }

    if (!validate_telemetry_enable(enable)) {
        set_error(error, "enable validation failed");
        return std::nullopt;
    }

    return enable;
}

} // namespace nstu::exam
