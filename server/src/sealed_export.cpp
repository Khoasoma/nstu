#include "nstu/sealed_export.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace nstu::server::sealed_export {

namespace {

inline constexpr std::uint32_t kSealedMagic = 0x5345414CU; // "SEAL"
inline constexpr std::uint16_t kSealedVersion = 1u;
inline constexpr std::size_t kHeaderBytes = 88u;
inline constexpr std::string_view kDomainTag = "NSTU-SEALED-EXPORT-V1";

template <typename T>
void append_le(std::vector<std::byte>& out, T val) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>((val >> (i * 8)) & 0xFF));
    }
}

template <typename T>
T read_le(std::span<const std::byte> bytes, std::size_t offset) {
    T val = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        val |= (static_cast<T>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8));
    }
    return val;
}

std::string digest_to_hex(const security::Sha256Digest& digest) {
    std::ostringstream ss;
    for (std::byte b : digest) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}


std::string extract_json_string(std::string_view json, std::string_view key) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return {};
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return {};
    auto quote1 = json.find('\"', colon + 1);
    if (quote1 == std::string_view::npos) return {};
    auto quote2 = json.find('\"', quote1 + 1);
    if (quote2 == std::string_view::npos) return {};
    return std::string(json.substr(quote1 + 1, quote2 - quote1 - 1));
}

std::int64_t extract_json_int(std::string_view json, std::string_view key, std::int64_t default_val = 0) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return default_val;
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return default_val;
    std::size_t start = colon + 1;
    while (start < json.size() && (std::isspace(static_cast<unsigned char>(json[start])) || json[start] == '\"')) {
        ++start;
    }
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    if (start == end) return default_val;
    char* endptr = nullptr;
    return std::strtoll(json.data() + start, &endptr, 10);
}

double extract_json_double(std::string_view json, std::string_view key, double default_val = 0.0) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return default_val;
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return default_val;
    std::size_t start = colon + 1;
    while (start < json.size() && (std::isspace(static_cast<unsigned char>(json[start])) || json[start] == '\"')) {
        ++start;
    }
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E' || json[end] == '+')) {
        ++end;
    }
    if (start == end) return default_val;
    char* endptr = nullptr;
    return std::strtod(json.data() + start, &endptr);
}

std::string serialize_answers_json(const std::vector<exam::AnswerState>& answers) {
    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < answers.size(); ++i) {
        const auto& ans = answers[i];
        std::string text;
        for (std::byte b : ans.answer) {
            text.push_back(static_cast<char>(b));
        }
        ss << "  {\n";
        ss << "    \"question_id\": \"" << ans.question_id << "\",\n";
        ss << "    \"sequence\": " << ans.sequence << ",\n";
        ss << "    \"kind\": " << static_cast<int>(ans.kind) << ",\n";
        ss << "    \"answer\": \"" << text << "\"\n";
        ss << "  }" << (i + 1 < answers.size() ? "," : "") << "\n";
    }
    ss << "]\n";
    return ss.str();
}

std::string serialize_audit_json(const std::vector<std::string>& audit_events) {
    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < audit_events.size(); ++i) {
        ss << "  \"" << audit_events[i] << "\"" << (i + 1 < audit_events.size() ? "," : "") << "\n";
    }
    ss << "]\n";
    return ss.str();
}

}  // namespace

const char* sealed_export_verify_result_to_string(
    SealedExportVerifyResult result) noexcept {
    switch (result) {
        case SealedExportVerifyResult::valid:
            return "valid";
        case SealedExportVerifyResult::invalid_magic:
            return "invalid_magic";
        case SealedExportVerifyResult::unsupported_version:
            return "unsupported_version";
        case SealedExportVerifyResult::corrupted_header:
            return "corrupted_header";
        case SealedExportVerifyResult::manifest_digest_mismatch:
            return "manifest_digest_mismatch";
        case SealedExportVerifyResult::signature_mismatch:
            return "signature_mismatch";
        case SealedExportVerifyResult::section_digest_mismatch:
            return "section_digest_mismatch";
        case SealedExportVerifyResult::payload_truncated:
            return "payload_truncated";
    }
    return "unknown";
}

std::vector<std::byte> create_sealed_export(
    const grading::CandidateExamGrade& grade,
    const std::vector<exam::AnswerState>& answers,
    const std::vector<std::string>& audit_events,
    std::span<const std::byte> signing_key,
    std::uint64_t timestamp_ms) {
    if (signing_key.empty()) return {};

    const std::string grade_summary_json = grading::serialize_grade_json(grade);
    const std::string student_answers_json = serialize_answers_json(answers);
    const std::string audit_trail_json = serialize_audit_json(audit_events);

    // Compute digests of all sections
    const auto grade_digest = security::sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(grade_summary_json.data()), grade_summary_json.size()));
    const auto answers_digest = security::sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(student_answers_json.data()), student_answers_json.size()));
    const auto audit_digest = security::sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(audit_trail_json.data()), audit_trail_json.size()));

    if (!grade_digest || !answers_digest || !audit_digest) {
        return {};
    }

    struct SectionPayload {
        std::string filename;
        security::Sha256Digest digest;
        std::string content;
    };

    std::vector<SectionPayload> sections = {
        {"grade_summary.json", *grade_digest, grade_summary_json},
        {"student_answers.json", *answers_digest, student_answers_json},
        {"audit_trail.json", *audit_digest, audit_trail_json}
    };

    // Build canonical manifest JSON
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"package_id\": \"" << grade.package_id << "\",\n";
    ss << "  \"candidate_id\": \"" << grade.candidate_id << "\",\n";
    ss << "  \"timestamp_ms\": " << timestamp_ms << ",\n";
    ss << "  \"total_points\": " << grade.total_points << ",\n";
    ss << "  \"max_points\": " << grade.max_points << ",\n";
    ss << "  \"score_percentage\": " << grade.score_percentage << ",\n";
    ss << "  \"sections\": [\n";
    for (std::size_t i = 0; i < sections.size(); ++i) {
        ss << "    {\n";
        ss << "      \"filename\": \"" << sections[i].filename << "\",\n";
        ss << "      \"digest\": \"" << digest_to_hex(sections[i].digest) << "\",\n";
        ss << "      \"size\": " << sections[i].content.size() << "\n";
        ss << "    }" << (i + 1 < sections.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    const std::string manifest_json = ss.str();
    const auto manifest_digest = security::sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(manifest_json.data()), manifest_json.size()));
    if (!manifest_digest) return {};

    // Compute seal signature: HMAC-SHA256(signing_key, kDomainTag + manifest_digest)
    std::vector<std::byte> sign_transcript;
    sign_transcript.reserve(kDomainTag.size() + manifest_digest->size());
    for (char c : kDomainTag) sign_transcript.push_back(static_cast<std::byte>(c));
    sign_transcript.insert(sign_transcript.end(), manifest_digest->begin(), manifest_digest->end());

    const auto seal_sig = security::hmac_sha256(signing_key, sign_transcript);
    if (!seal_sig) return {};

    // Assemble final container bytes
    std::vector<std::byte> container;
    container.reserve(kHeaderBytes + manifest_json.size() + 2048);

    // Header
    append_le<std::uint32_t>(container, kSealedMagic);
    append_le<std::uint16_t>(container, kSealedVersion);
    append_le<std::uint16_t>(container, 0); // reserved
    append_le<std::uint64_t>(container, timestamp_ms);
    container.insert(container.end(), manifest_digest->begin(), manifest_digest->end());
    container.insert(container.end(), seal_sig->begin(), seal_sig->end());
    append_le<std::uint32_t>(container, static_cast<std::uint32_t>(manifest_json.size()));
    append_le<std::uint32_t>(container, static_cast<std::uint32_t>(sections.size()));

    // Manifest
    for (char c : manifest_json) {
        container.push_back(static_cast<std::byte>(c));
    }

    // Sections payload
    for (const auto& sec : sections) {
        append_le<std::uint16_t>(container, static_cast<std::uint16_t>(sec.filename.size()));
        for (char c : sec.filename) container.push_back(static_cast<std::byte>(c));
        append_le<std::uint32_t>(container, static_cast<std::uint32_t>(sec.content.size()));
        for (char c : sec.content) container.push_back(static_cast<std::byte>(c));
    }

    return container;
}

SealedExportVerifyResult verify_sealed_export(
    std::span<const std::byte> container_bytes,
    std::span<const std::byte> verify_key,
    SealedExportData* out_data) {
    if (container_bytes.size() < kHeaderBytes) {
        return SealedExportVerifyResult::corrupted_header;
    }

    std::size_t offset = 0;
    const auto magic = read_le<std::uint32_t>(container_bytes, offset);
    offset += 4;
    if (magic != kSealedMagic) {
        return SealedExportVerifyResult::invalid_magic;
    }

    const auto version = read_le<std::uint16_t>(container_bytes, offset);
    offset += 2;
    const auto reserved = read_le<std::uint16_t>(container_bytes, offset);
    offset += 2;
    if (version != kSealedVersion || reserved != 0) {
        return SealedExportVerifyResult::unsupported_version;
    }

    const auto timestamp_ms = read_le<std::uint64_t>(container_bytes, offset);
    offset += 8;

    security::Sha256Digest stored_manifest_digest{};
    std::memcpy(stored_manifest_digest.data(), container_bytes.data() + offset, stored_manifest_digest.size());
    offset += stored_manifest_digest.size();

    security::Sha256Digest stored_seal_sig{};
    std::memcpy(stored_seal_sig.data(), container_bytes.data() + offset, stored_seal_sig.size());
    offset += stored_seal_sig.size();

    const auto manifest_len = read_le<std::uint32_t>(container_bytes, offset);
    offset += 4;
    const auto num_sections = read_le<std::uint32_t>(container_bytes, offset);
    offset += 4;

    if (offset + manifest_len > container_bytes.size()) {
        return SealedExportVerifyResult::payload_truncated;
    }

    // Verify manifest digest
    const auto manifest_span = container_bytes.subspan(offset, manifest_len);
    const auto computed_manifest_digest = security::sha256(manifest_span);
    if (!computed_manifest_digest ||
        !security::constant_time_equal(stored_manifest_digest, *computed_manifest_digest)) {
        return SealedExportVerifyResult::manifest_digest_mismatch;
    }

    // Verify seal signature
    if (!verify_key.empty()) {
        std::vector<std::byte> sign_transcript;
        sign_transcript.reserve(kDomainTag.size() + stored_manifest_digest.size());
        for (char c : kDomainTag) sign_transcript.push_back(static_cast<std::byte>(c));
        sign_transcript.insert(sign_transcript.end(), stored_manifest_digest.begin(), stored_manifest_digest.end());

        const auto expected_sig = security::hmac_sha256(verify_key, sign_transcript);
        if (!expected_sig ||
            !security::constant_time_equal(stored_seal_sig, *expected_sig)) {
            return SealedExportVerifyResult::signature_mismatch;
        }
    }

    offset += manifest_len;

    // Parse and verify sections
    SealedExportData parsed_data;
    parsed_data.manifest.created_timestamp_ms = timestamp_ms;
    const std::string_view manifest_str(reinterpret_cast<const char*>(manifest_span.data()), manifest_span.size());
    parsed_data.manifest.package_id = extract_json_string(manifest_str, "package_id");
    parsed_data.manifest.candidate_id = extract_json_string(manifest_str, "candidate_id");
    parsed_data.manifest.total_points = static_cast<std::uint32_t>(extract_json_int(manifest_str, "total_points", 0));
    parsed_data.manifest.max_points = static_cast<std::uint32_t>(extract_json_int(manifest_str, "max_points", 0));
    parsed_data.manifest.score_percentage = extract_json_double(manifest_str, "score_percentage", 0.0);

    for (std::uint32_t s = 0; s < num_sections; ++s) {
        if (offset + 2 > container_bytes.size()) {
            return SealedExportVerifyResult::payload_truncated;
        }
        const auto filename_len = read_le<std::uint16_t>(container_bytes, offset);
        offset += 2;
        if (offset + filename_len + 4 > container_bytes.size()) {
            return SealedExportVerifyResult::payload_truncated;
        }
        std::string filename(reinterpret_cast<const char*>(container_bytes.data() + offset), filename_len);
        offset += filename_len;

        const auto content_len = read_le<std::uint32_t>(container_bytes, offset);
        offset += 4;
        if (offset + content_len > container_bytes.size()) {
            return SealedExportVerifyResult::payload_truncated;
        }

        const auto section_span = container_bytes.subspan(offset, content_len);
        const auto section_digest = security::sha256(section_span);
        if (!section_digest) {
            return SealedExportVerifyResult::section_digest_mismatch;
        }

        // Verify computed section digest matches the expected digest recorded in the manifest
        const std::string filename_search = "\"" + filename + "\"";
        const auto fn_pos = manifest_str.find(filename_search);
        if (fn_pos != std::string_view::npos) {
            const auto obj_start = manifest_str.rfind('{', fn_pos);
            const auto obj_end = manifest_str.find('}', fn_pos);
            if (obj_start != std::string_view::npos && obj_end != std::string_view::npos) {
                const std::string_view sec_json = manifest_str.substr(obj_start, obj_end - obj_start + 1);
                const std::string exp_hex = extract_json_string(sec_json, "digest");
                if (!exp_hex.empty() && digest_to_hex(*section_digest) != exp_hex) {
                    return SealedExportVerifyResult::section_digest_mismatch;
                }
            }
        }

        std::string content(reinterpret_cast<const char*>(container_bytes.data() + offset), content_len);
        offset += content_len;

        SealedSectionEntry entry;
        entry.filename = filename;
        entry.digest = *section_digest;
        entry.size_bytes = content_len;
        parsed_data.manifest.sections.push_back(std::move(entry));

        if (filename == "grade_summary.json") {
            parsed_data.grade_summary_json = std::move(content);
        } else if (filename == "student_answers.json") {
            parsed_data.student_answers_json = std::move(content);
        } else if (filename == "audit_trail.json") {
            parsed_data.audit_trail_json = std::move(content);
        }
    }

    if (offset != container_bytes.size()) {
        return SealedExportVerifyResult::corrupted_header;
    }

    if (out_data) {
        *out_data = std::move(parsed_data);
    }

    return SealedExportVerifyResult::valid;
}

bool export_to_file(
    const std::filesystem::path& file_path,
    const grading::CandidateExamGrade& grade,
    const std::vector<exam::AnswerState>& answers,
    const std::vector<std::string>& audit_events,
    std::span<const std::byte> signing_key,
    std::string* error) {
    const auto bytes = create_sealed_export(grade, answers, audit_events, signing_key,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    if (bytes.empty()) {
        if (error) *error = "failed to create sealed export bytes";
        return false;
    }

    std::ofstream out(file_path, std::ios::binary);
    if (!out.is_open()) {
        if (error) *error = "failed to open output file: " + file_path.string();
        return false;
    }

    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        if (error) *error = "failed to write sealed export to: " + file_path.string();
        return false;
    }

    return true;
}

std::optional<SealedExportData> import_from_file(
    const std::filesystem::path& file_path,
    std::span<const std::byte> verify_key,
    std::string* error) {
    std::ifstream in(file_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        if (error) *error = "failed to open file: " + file_path.string();
        return std::nullopt;
    }

    const auto file_size = in.tellg();
    if (file_size <= 0 || file_size > 100 * 1024 * 1024) {
        if (error) *error = "invalid sealed file size: " + file_path.string();
        return std::nullopt;
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(buffer.data()), file_size);
    if (!in.good()) {
        if (error) *error = "failed to read file: " + file_path.string();
        return std::nullopt;
    }

    SealedExportData data;
    const auto result = verify_sealed_export(buffer, verify_key, &data);
    if (result != SealedExportVerifyResult::valid) {
        if (error) *error = std::string("sealed export verification failed: ") +
                            sealed_export_verify_result_to_string(result);
        return std::nullopt;
    }

    return data;
}

}  // namespace nstu::server::sealed_export
