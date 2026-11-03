#pragma once

#include "nstu/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::file_dist {

inline constexpr std::size_t kMaxFileSize = 64 * 1024 * 1024; // 64 MiB
inline constexpr std::size_t kMaxChunkSize = 64 * 1024;        // 64 KiB
inline constexpr std::size_t kMaxFilenameLength = 255;

enum class FileDistStatus : std::uint8_t {
    in_progress = 0,
    completed = 1,
    hash_mismatch = 2,
    write_error = 3,
    cancelled = 4,
    rejected_exam_active = 5,
    rejected_unsafe_path = 6,
    rejected_quota_exceeded = 7,
};

[[nodiscard]] const char* to_string(FileDistStatus status) noexcept;
[[nodiscard]] const char* status_label(FileDistStatus status, bool vietnamese) noexcept;

[[nodiscard]] bool is_safe_filename(std::string_view filename) noexcept;
[[nodiscard]] std::string sanitize_filename(std::string_view filename);

[[nodiscard]] std::optional<std::filesystem::path> compute_staging_path(
    const std::filesystem::path& base_dir,
    std::string_view filename);

struct FileDistStartMessage {
    std::uint64_t session_id = 0;
    std::uint64_t file_size = 0;
    nstu::security::Sha256Digest sha256{};
    std::string filename;
};

struct FileDistChunkMessage {
    std::uint64_t session_id = 0;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> data;
};

struct FileDistFinishMessage {
    std::uint64_t session_id = 0;
};

struct FileDistStatusMessage {
    std::uint64_t session_id = 0;
    std::uint64_t bytes_received = 0;
    FileDistStatus status = FileDistStatus::in_progress;
};

struct FileDistCancelMessage {
    std::uint64_t session_id = 0;
    std::uint32_t reason_code = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_file_dist_start(
    const FileDistStartMessage& msg);
[[nodiscard]] std::optional<FileDistStartMessage> decode_file_dist_start(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_file_dist_chunk(
    const FileDistChunkMessage& msg);
[[nodiscard]] std::optional<FileDistChunkMessage> decode_file_dist_chunk(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_file_dist_finish(
    const FileDistFinishMessage& msg);
[[nodiscard]] std::optional<FileDistFinishMessage> decode_file_dist_finish(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_file_dist_status(
    const FileDistStatusMessage& msg);
[[nodiscard]] std::optional<FileDistStatusMessage> decode_file_dist_status(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_file_dist_cancel(
    const FileDistCancelMessage& msg);
[[nodiscard]] std::optional<FileDistCancelMessage> decode_file_dist_cancel(
    std::span<const std::uint8_t> payload);

} // namespace nstu::file_dist

