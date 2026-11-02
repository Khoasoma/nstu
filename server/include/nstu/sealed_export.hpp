#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_grader.hpp"
#include "nstu/exam_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::server::sealed_export {

enum class SealedExportVerifyResult {
    valid,
    invalid_magic,
    unsupported_version,
    corrupted_header,
    manifest_digest_mismatch,
    signature_mismatch,
    section_digest_mismatch,
    payload_truncated
};

[[nodiscard]] const char* sealed_export_verify_result_to_string(
    SealedExportVerifyResult result) noexcept;

struct SealedSectionEntry {
    std::string filename;
    security::Sha256Digest digest{};
    std::size_t size_bytes = 0;
};

struct SealedExportManifest {
    std::string package_id;
    std::string candidate_id;
    std::uint64_t created_timestamp_ms = 0;
    std::uint32_t total_points = 0;
    std::uint32_t max_points = 0;
    double score_percentage = 0.0;
    std::vector<SealedSectionEntry> sections;
};

struct SealedExportData {
    SealedExportManifest manifest;
    std::string grade_summary_json;
    std::string student_answers_json;
    std::string audit_trail_json;
};

[[nodiscard]] std::vector<std::byte> create_sealed_export(
    const grading::CandidateExamGrade& grade,
    const std::vector<exam::AnswerState>& answers,
    const std::vector<std::string>& audit_events,
    std::span<const std::byte> signing_key,
    std::uint64_t timestamp_ms = 0);

[[nodiscard]] SealedExportVerifyResult verify_sealed_export(
    std::span<const std::byte> container_bytes,
    std::span<const std::byte> verify_key,
    SealedExportData* out_data = nullptr);

[[nodiscard]] bool export_to_file(
    const std::filesystem::path& file_path,
    const grading::CandidateExamGrade& grade,
    const std::vector<exam::AnswerState>& answers,
    const std::vector<std::string>& audit_events,
    std::span<const std::byte> signing_key,
    std::string* error = nullptr);

[[nodiscard]] std::optional<SealedExportData> import_from_file(
    const std::filesystem::path& file_path,
    std::span<const std::byte> verify_key,
    std::string* error = nullptr);

}  // namespace nstu::server::sealed_export
