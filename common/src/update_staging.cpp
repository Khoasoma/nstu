#include "nstu/update_staging.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace nstu::update {

std::optional<UpdateState> parse_update_state(std::string_view str) noexcept {
    if (str == "idle") return UpdateState::idle;
    if (str == "checking") return UpdateState::checking;
    if (str == "deferred") return UpdateState::deferred;
    if (str == "downloading") return UpdateState::downloading;
    if (str == "verifying") return UpdateState::verifying;
    if (str == "staged") return UpdateState::staged;
    if (str == "apply_pending_reboot") return UpdateState::apply_pending_reboot;
    if (str == "validating") return UpdateState::validating;
    if (str == "committed") return UpdateState::committed;
    if (str == "rolling_back") return UpdateState::rolling_back;
    if (str == "failed") return UpdateState::failed;
    return std::nullopt;
}

namespace {

constexpr std::size_t kMaximumStagingRecordBytes = 64u * 1024u;
constexpr std::array<std::string_view, 12> kStagingRecordFields{
    "schema_version", "active_version", "target_version", "target_sha256_hex",
    "expected_bytes", "staged_bytes", "current_state", "last_error",
    "state_timestamp_utc", "requires_reboot", "backup_path", "staged_package_path"};

std::uint64_t current_time_seconds() noexcept {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

struct CappedRecordSize {
    std::size_t bytes = 0;
    bool append(std::string_view text) noexcept {
        if (text.size() > kMaximumStagingRecordBytes - bytes) return false;
        bytes += text.size();
        return true;
    }
};

struct RecordTextSink {
    std::string& text;
    bool append(std::string_view part) {
        if (part.size() > kMaximumStagingRecordBytes - text.size()) return false;
        text.append(part);
        return true;
    }
};

template<class Sink>
bool emit_record_string(Sink& sink, std::string_view text) {
    if (text.size() > kMaximumStagingRecordBytes || !sink.append("\"")) return false;
    constexpr char digits[] = "0123456789abcdef";
    for (const char c : text) {
        switch (c) {
        case '"': if (!sink.append("\\\"")) return false; break;
        case '\\': if (!sink.append("\\\\")) return false; break;
        case '\b': if (!sink.append("\\b")) return false; break;
        case '\f': if (!sink.append("\\f")) return false; break;
        case '\n': if (!sink.append("\\n")) return false; break;
        case '\r': if (!sink.append("\\r")) return false; break;
        case '\t': if (!sink.append("\\t")) return false; break;
        default:
            if (const auto value = static_cast<unsigned char>(c); value < 0x20u) {
                const char escaped[]{'\\', 'u', '0', '0', digits[value >> 4u], digits[value & 0x0fu]};
                if (!sink.append(std::string_view(escaped, sizeof(escaped)))) return false;
            } else if (!sink.append(std::string_view(&c, 1))) return false;
            break;
        }
    }
    return sink.append("\"");
}

template<class Sink>
bool emit_record_number(Sink& sink, std::uint64_t value) {
    char digits[20];
    const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
    return converted.ec == std::errc{} &&
           sink.append(std::string_view(digits, static_cast<std::size_t>(converted.ptr - digits)));
}

template<class Sink>
bool emit_record_version(Sink& sink, const SemanticVersion& version) {
    return sink.append("\"") && emit_record_number(sink, version.major) &&
           sink.append(".") && emit_record_number(sink, version.minor) &&
           sink.append(".") && emit_record_number(sink, version.patch) && sink.append("\"");
}

template<class Sink, class ValueWriter>
bool emit_record_field(Sink& sink, std::size_t index, ValueWriter write_value,
                       bool last = false) {
    return sink.append("  \"") && sink.append(kStagingRecordFields[index]) &&
           sink.append("\": ") && write_value() && sink.append(last ? "\n" : ",\n");
}

template<class Sink>
bool emit_record(Sink& sink, const StagingRecord& record) {
    return sink.append("{\n") &&
        emit_record_field(sink, 0, [&] { return emit_record_string(sink, record.schema_version); }) &&
        emit_record_field(sink, 1, [&] { return emit_record_version(sink, record.active_version); }) &&
        emit_record_field(sink, 2, [&] { return emit_record_version(sink, record.target_version); }) &&
        emit_record_field(sink, 3, [&] { return emit_record_string(sink, record.target_sha256_hex); }) &&
        emit_record_field(sink, 4, [&] { return emit_record_number(sink, record.expected_bytes); }) &&
        emit_record_field(sink, 5, [&] { return emit_record_number(sink, record.staged_bytes); }) &&
        emit_record_field(sink, 6, [&] { return emit_record_string(sink, to_string(record.current_state)); }) &&
        emit_record_field(sink, 7, [&] { return emit_record_string(sink, record.last_error); }) &&
        emit_record_field(sink, 8, [&] { return emit_record_number(sink, record.state_timestamp_utc); }) &&
        emit_record_field(sink, 9, [&] { return sink.append(record.requires_reboot ? "true" : "false"); }) &&
        emit_record_field(sink, 10, [&] { return emit_record_string(sink, record.backup_path); }) &&
        emit_record_field(sink, 11, [&] { return emit_record_string(sink, record.staged_package_path); }, true) &&
        sink.append("}\n");
}

struct MiniJsonReader {
    std::string_view input;
    std::size_t pos{0};

    void skip_whitespace() noexcept {
        while (pos < input.size()) {
            const char c = input[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool match_char(char expected) noexcept {
        skip_whitespace();
        if (pos < input.size() && input[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool read_hex_quad(std::uint32_t& value) noexcept {
        value = 0;
        for (unsigned int index = 0; index < 4; ++index) {
            if (pos >= input.size()) return false;
            const char c = input[pos++];
            unsigned int digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned int>(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned int>(c - 'A') + 10u;
            else return false;
            value = value * 16u + digit;
        }
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t codepoint) {
        if (codepoint <= 0x7fu) out.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            out.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            out.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            out.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool parse_string(std::string& out) {
        skip_whitespace();
        if (pos >= input.size() || input[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < input.size()) {
            const char c = input[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20u) return false;
            if (c == '\\') {
                if (pos >= input.size()) return false;
                switch (input[pos++]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!read_hex_quad(codepoint)) return false;
                    if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                        if (input.substr(pos, 2) != "\\u") return false;
                        pos += 2;
                        std::uint32_t low = 0;
                        if (!read_hex_quad(low) || low < 0xdc00u || low > 0xdfffu) return false;
                        codepoint = 0x10000u + (codepoint - 0xd800u) * 0x400u + low - 0xdc00u;
                    } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) return false;
                    append_utf8(out, codepoint);
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_uint64(std::uint64_t& out) noexcept {
        skip_whitespace();
        if (pos >= input.size() || !std::isdigit(static_cast<unsigned char>(input[pos]))) return false;
        if (input[pos] == '0' && pos + 1 < input.size() &&
            std::isdigit(static_cast<unsigned char>(input[pos + 1]))) return false;
        std::uint64_t val = 0;
        while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
            const unsigned int digit = static_cast<unsigned int>(input[pos++] - '0');
            if (val > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return false;
            }
            val = val * 10 + digit;
        }
        out = val;
        return true;
    }

    bool parse_bool(bool& out) noexcept {
        skip_whitespace();
        if (pos + 4 <= input.size() && input.substr(pos, 4) == "true") {
            pos += 4;
            out = true;
            return true;
        }
        if (pos + 5 <= input.size() && input.substr(pos, 5) == "false") {
            pos += 5;
            out = false;
            return true;
        }
        return false;
    }

};

bool bind_record_path(std::string& supplied, const std::string& configured,
                      bool directory) {
    if (supplied.empty()) return true;
    if (configured.empty() || supplied.find('\0') != std::string::npos ||
        configured.find('\0') != std::string::npos) return false;
    const auto separator = [](char c) { return c == '/' || c == '\\'; };
    if (supplied.size() >= 2 && separator(supplied[0]) && separator(supplied[1])) return false;
    auto candidate = std::filesystem::path(supplied);
    if (candidate.has_root_name() && !candidate.has_root_directory()) return false;
    for (const auto& component : candidate) {
        if (component == "." || component == "..") return false;
    }
    candidate = candidate.lexically_normal().make_preferred();
    auto expected = std::filesystem::path(configured).lexically_normal().make_preferred();
    if (directory) {
        if (candidate.filename().empty()) candidate = candidate.parent_path();
        if (expected.filename().empty()) expected = expected.parent_path();
    }
    if (candidate.native().size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        expected.native().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    if (CompareStringOrdinal(candidate.c_str(), static_cast<int>(candidate.native().size()),
                             expected.c_str(), static_cast<int>(expected.native().size()), TRUE) != CSTR_EQUAL) return false;
    supplied = configured;
    return true;
}

} // namespace

std::string export_staging_record_json(const StagingRecord& record) {
    constexpr char oversized[] = "Staging record exceeds the 64 KiB serialization limit";
    CappedRecordSize size;
    if (!emit_record(size, record)) throw std::length_error(oversized);
    std::string text;
    text.reserve(size.bytes);
    RecordTextSink sink{text};
    if (!emit_record(sink, record)) throw std::length_error(oversized);
    return text;
}

std::optional<StagingRecord> import_staging_record_json(std::string_view json_str) {
    if (json_str.empty() || json_str.size() > kMaximumStagingRecordBytes) return std::nullopt;

    MiniJsonReader reader{json_str, 0};
    if (!reader.match_char('{')) return std::nullopt;

    StagingRecord rec;
    std::uint16_t seen = 0;

    while (true) {
        reader.skip_whitespace();
        if (reader.match_char('}')) break;

        std::string key;
        if (!reader.parse_string(key) || !reader.match_char(':')) return std::nullopt;
        const auto field = std::find(kStagingRecordFields.begin(), kStagingRecordFields.end(), key);
        if (field == kStagingRecordFields.end()) return std::nullopt;
        const auto index = static_cast<unsigned int>(field - kStagingRecordFields.begin());
        const auto bit = static_cast<std::uint16_t>(1u << index);
        if ((seen & bit) != 0) return std::nullopt;
        seen = static_cast<std::uint16_t>(seen | bit);

        if (key == "schema_version") {
            if (!reader.parse_string(rec.schema_version)) return std::nullopt;
        } else if (key == "active_version") {
            std::string v;
            if (!reader.parse_string(v)) return std::nullopt;
            const auto parsed = SemanticVersion::parse(v);
            if (!parsed) return std::nullopt;
            rec.active_version = *parsed;
        } else if (key == "target_version") {
            std::string v;
            if (!reader.parse_string(v)) return std::nullopt;
            const auto parsed = SemanticVersion::parse(v);
            if (!parsed) return std::nullopt;
            rec.target_version = *parsed;
        } else if (key == "target_sha256_hex") {
            if (!reader.parse_string(rec.target_sha256_hex)) return std::nullopt;
        } else if (key == "expected_bytes") {
            if (!reader.parse_uint64(rec.expected_bytes)) return std::nullopt;
        } else if (key == "staged_bytes") {
            if (!reader.parse_uint64(rec.staged_bytes)) return std::nullopt;
        } else if (key == "current_state") {
            std::string st;
            if (!reader.parse_string(st)) return std::nullopt;
            const auto parsed = parse_update_state(st);
            if (!parsed) return std::nullopt;
            rec.current_state = *parsed;
        } else if (key == "last_error") {
            if (!reader.parse_string(rec.last_error)) return std::nullopt;
        } else if (key == "state_timestamp_utc") {
            if (!reader.parse_uint64(rec.state_timestamp_utc)) return std::nullopt;
        } else if (key == "requires_reboot") {
            if (!reader.parse_bool(rec.requires_reboot)) return std::nullopt;
        } else if (key == "backup_path") {
            if (!reader.parse_string(rec.backup_path)) return std::nullopt;
        } else if (key == "staged_package_path") {
            if (!reader.parse_string(rec.staged_package_path)) return std::nullopt;
        }

        reader.skip_whitespace();
        if (reader.match_char(',')) {
            reader.skip_whitespace();
            if (reader.pos < reader.input.size() && reader.input[reader.pos] == '}') {
                return std::nullopt; // Trailing comma
            }
        } else if (reader.match_char('}')) break;
        else return std::nullopt;
    }

    reader.skip_whitespace();
    if (reader.pos != json_str.size()) return std::nullopt;
    constexpr std::uint16_t required = (1u << kStagingRecordFields.size()) - 1u;
    if (seen != required || rec.schema_version != "1.0" || rec.staged_bytes > rec.expected_bytes) return std::nullopt;

    return rec;
}

UpdateStagingController::UpdateStagingController(UpdateStagingConfig config)
    : config_(std::move(config)) {}

UpdateStagingController::~UpdateStagingController() {
    cleanup_hash_context();
}

UpdateStagingController::UpdateStagingController(UpdateStagingController&& other) noexcept
    : config_(std::move(other.config_)),
      record_(std::move(other.record_)),
      hash_handle_(other.hash_handle_),
      alg_handle_(other.alg_handle_),
      hash_object_(std::move(other.hash_object_)),
      is_streaming_(other.is_streaming_) {
    other.hash_handle_ = nullptr;
    other.alg_handle_ = nullptr;
    other.is_streaming_ = false;
}

UpdateStagingController& UpdateStagingController::operator=(UpdateStagingController&& other) noexcept {
    if (this != &other) {
        cleanup_hash_context();
        config_ = std::move(other.config_);
        record_ = std::move(other.record_);
        hash_handle_ = other.hash_handle_;
        alg_handle_ = other.alg_handle_;
        hash_object_ = std::move(other.hash_object_);
        is_streaming_ = other.is_streaming_;
        other.hash_handle_ = nullptr;
        other.alg_handle_ = nullptr;
        other.is_streaming_ = false;
    }
    return *this;
}

void UpdateStagingController::cleanup_hash_context() noexcept {
    if (hash_handle_ != nullptr) {
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_handle_));
        hash_handle_ = nullptr;
    }
    if (alg_handle_ != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_handle_), 0);
        alg_handle_ = nullptr;
    }
    hash_object_.clear();
    is_streaming_ = false;
}

std::string UpdateStagingController::get_part_file_path() const {
    std::filesystem::path p(config_.staging_directory);
    return (p / "update_package.part").string();
}

std::string UpdateStagingController::get_final_package_path() const {
    std::filesystem::path p(config_.staging_directory);
    return (p / "update_package.pkg").string();
}

std::string UpdateStagingController::get_record_file_path() const {
    std::filesystem::path p(config_.staging_directory);
    return (p / "staging_record.json").string();
}

bool UpdateStagingController::save_record_to_disk() {
    try {
        const std::string json = export_staging_record_json(record_);
        if (!config_.staging_directory.empty()) {
            std::filesystem::create_directories(config_.staging_directory);
            const std::string record_file = get_record_file_path();
            std::ofstream ofs(record_file, std::ios::trunc | std::ios::binary);
            if (ofs.is_open()) {
                ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
                return ofs.good();
            }
        }
    } catch (...) {}
    return false;
}

UpdateStagingResult UpdateStagingController::init() {
    UpdateStagingResult res;
    try {
        if (!config_.staging_directory.empty()) {
            std::filesystem::create_directories(config_.staging_directory);
        }

        const std::string record_file = get_record_file_path();
        // Query the directory entry itself: exists/status can follow a dangling
        // symbolic link and mistake an existing recovery entry for absence.
        const std::filesystem::path record_path(record_file);
        const DWORD attributes = GetFileAttributesW(record_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return {UpdateStagingError::io_error, "Unable to inspect recovery record"};
            }
        } else {
            if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_DEVICE)) != 0) {
                return {UpdateStagingError::io_error, "Recovery record is not a regular file"};
            }
            std::ifstream ifs(record_file, std::ios::binary);
            if (!ifs.is_open()) return {UpdateStagingError::io_error, "Unable to open recovery record"};
            std::string content(kMaximumStagingRecordBytes + 1u, '\0');
            ifs.read(content.data(), static_cast<std::streamsize>(content.size()));
            const auto bytes = ifs.gcount();
            if (bytes < 0 || ifs.bad() || (ifs.fail() && !ifs.eof())) {
                return {UpdateStagingError::io_error, "Unable to read recovery record"};
            }
            if (bytes > static_cast<std::streamsize>(kMaximumStagingRecordBytes)) {
                return {UpdateStagingError::invalid_state, "Recovery record exceeds size limit"};
            }
            content.resize(static_cast<std::size_t>(bytes));
            auto loaded = import_staging_record_json(content);
            if (!loaded) return {UpdateStagingError::invalid_state, "Recovery record is malformed or incompatible"};
            if (!bind_record_path(loaded->staged_package_path, get_final_package_path(), false) ||
                !bind_record_path(loaded->backup_path, config_.backup_directory, true)) {
                return {UpdateStagingError::invalid_state, "Recovery record paths do not match configuration"};
            }
            record_ = std::move(*loaded);
            // Existing interrupted-download recovery runs only after preflight.
            if (record_.current_state == UpdateState::downloading) {
                record_.current_state = UpdateState::failed;
                record_.last_error = "Download interrupted by system restart";
                std::filesystem::remove(get_part_file_path());
                save_record_to_disk();
            }
            res.error = UpdateStagingError::ok;
            return res;
        }
    } catch (const std::exception&) {
        res.error = UpdateStagingError::io_error;
        res.message = "Unable to initialize update staging";
        return res;
    }

    record_ = StagingRecord{};
    record_.current_state = UpdateState::idle;
    res.error = UpdateStagingError::ok;
    return res;
}

void UpdateStagingController::set_deferred(std::string_view reason) {
    record_.current_state = UpdateState::deferred;
    record_.last_error = std::string(reason);
    record_.state_timestamp_utc = current_time_seconds();
    save_record_to_disk();
}

UpdateStagingResult UpdateStagingController::begin_staging(const UpdateManifest& manifest,
                                                          const SemanticVersion& current_version,
                                                          bool is_exam_active) {
    UpdateStagingResult res;

    if (is_exam_active) {
        res.error = UpdateStagingError::exam_active_conflict;
        res.message = "Cannot begin update staging while an assessment is active";
        return res;
    }

    cleanup_hash_context();

    try {
        if (!config_.staging_directory.empty()) {
            std::filesystem::create_directories(config_.staging_directory);
        }

        // Remove stale partial files
        const std::string part_file = get_part_file_path();
        if (std::filesystem::exists(part_file)) {
            std::filesystem::remove(part_file);
        }
        const std::string final_pkg = get_final_package_path();
        if (std::filesystem::exists(final_pkg)) {
            std::filesystem::remove(final_pkg);
        }

        // Initialize BCrypt SHA-256 incremental hash
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            res.error = UpdateStagingError::io_error;
            res.message = "Failed to open BCrypt SHA-256 algorithm provider";
            return res;
        }
        alg_handle_ = alg;

        DWORD obj_len = 0;
        DWORD copied = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len),
                              sizeof(obj_len), &copied, 0) != 0) {
            cleanup_hash_context();
            res.error = UpdateStagingError::io_error;
            res.message = "Failed to get BCrypt object length";
            return res;
        }

        hash_object_.resize(obj_len);
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(alg, &hash, hash_object_.data(), obj_len, nullptr, 0, 0) != 0) {
            cleanup_hash_context();
            res.error = UpdateStagingError::io_error;
            res.message = "Failed to create BCrypt hash handle";
            return res;
        }
        hash_handle_ = hash;

        // Reset state record
        record_.active_version = current_version;
        record_.target_version = manifest.version;
        record_.target_sha256_hex = manifest.sha256_hex;
        record_.expected_bytes = manifest.byte_length;
        record_.staged_bytes = 0;
        record_.requires_reboot = manifest.requires_reboot;
        record_.staged_package_path = final_pkg;
        record_.current_state = UpdateState::downloading;
        record_.last_error.clear();
        record_.state_timestamp_utc = current_time_seconds();

        is_streaming_ = true;
        save_record_to_disk();

        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        cleanup_hash_context();
        res.error = UpdateStagingError::io_error;
        res.message = e.what();
        return res;
    }
}

UpdateStagingResult UpdateStagingController::append_chunk(std::span<const std::byte> chunk) {
    UpdateStagingResult res;

    if (record_.current_state != UpdateState::downloading || !is_streaming_ || hash_handle_ == nullptr) {
        res.error = UpdateStagingError::invalid_state;
        res.message = "Controller is not actively streaming an update package";
        return res;
    }

    if (chunk.size() > record_.expected_bytes - record_.staged_bytes) {
        abort_staging("Byte overflow: chunk exceeds expected package length");
        res.error = UpdateStagingError::byte_overflow;
        res.message = "Appended chunk exceeds declared package byte length";
        return res;
    }

    try {
        const std::string part_file = get_part_file_path();
        std::ofstream ofs(part_file, std::ios::binary | std::ios::app);
        if (!ofs.is_open()) {
            abort_staging("Failed to open staging .part file for writing");
            res.error = UpdateStagingError::io_error;
            res.message = "Cannot write to staging .part file";
            return res;
        }

        ofs.write(reinterpret_cast<const char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        if (!ofs.good()) {
            abort_staging("I/O write failure on staging .part file");
            res.error = UpdateStagingError::io_error;
            res.message = "I/O failure while writing chunk";
            return res;
        }

        auto* hash = static_cast<BCRYPT_HASH_HANDLE>(hash_handle_);
        if (BCryptHashData(hash,
                           const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(chunk.data())),
                           static_cast<ULONG>(chunk.size()), 0) != 0) {
            abort_staging("BCryptHashData failed on chunk");
            res.error = UpdateStagingError::io_error;
            res.message = "Cryptographic hash computation failed";
            return res;
        }

        record_.staged_bytes += chunk.size();
        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        abort_staging(e.what());
        res.error = UpdateStagingError::io_error;
        res.message = e.what();
        return res;
    }
}

UpdateStagingResult UpdateStagingController::finalize_staging() {
    UpdateStagingResult res;

    if (record_.current_state != UpdateState::downloading || !is_streaming_ || hash_handle_ == nullptr) {
        res.error = UpdateStagingError::invalid_state;
        res.message = "Controller is not in downloading state";
        return res;
    }

    if (record_.staged_bytes != record_.expected_bytes) {
        abort_staging("Truncated payload: staged bytes do not match expected manifest byte length");
        res.error = UpdateStagingError::truncated_payload;
        res.message = "Staged byte length is less than declared manifest length";
        return res;
    }

    auto* hash = static_cast<BCRYPT_HASH_HANDLE>(hash_handle_);
    std::array<std::byte, 32> digest{};
    if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                         static_cast<ULONG>(digest.size()), 0) != 0) {
        abort_staging("BCryptFinishHash failed");
        res.error = UpdateStagingError::io_error;
        res.message = "Failed to finalize SHA-256 digest";
        return res;
    }

    cleanup_hash_context();

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto b : digest) {
        ss << std::setw(2) << static_cast<unsigned int>(std::to_integer<unsigned char>(b));
    }
    const std::string computed_hex = ss.str();

    auto to_lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    if (to_lower(computed_hex) != to_lower(record_.target_sha256_hex)) {
        abort_staging("SHA-256 digest mismatch between downloaded payload and signed manifest");
        res.error = UpdateStagingError::digest_mismatch;
        res.message = "Payload digest does not match manifest specification";
        return res;
    }

    try {
        const std::string part_file = get_part_file_path();
        const std::string final_pkg = get_final_package_path();

        if (std::filesystem::exists(final_pkg)) {
            std::filesystem::remove(final_pkg);
        }
        std::filesystem::rename(part_file, final_pkg);

        record_.staged_package_path = final_pkg;
        record_.current_state = record_.requires_reboot ? UpdateState::apply_pending_reboot
                                                        : UpdateState::staged;
        record_.last_error.clear();
        record_.state_timestamp_utc = current_time_seconds();
        save_record_to_disk();

        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        abort_staging(e.what());
        res.error = UpdateStagingError::io_error;
        res.message = e.what();
        return res;
    }
}

UpdateStagingResult UpdateStagingController::abort_staging(std::string_view reason) {
    cleanup_hash_context();

    try {
        const std::string part_file = get_part_file_path();
        if (std::filesystem::exists(part_file)) {
            std::filesystem::remove(part_file);
        }
    } catch (...) {}

    record_.current_state = UpdateState::failed;
    record_.last_error = std::string(reason);
    record_.state_timestamp_utc = current_time_seconds();
    save_record_to_disk();

    UpdateStagingResult res;
    res.error = UpdateStagingError::ok;
    return res;
}

UpdateStagingResult UpdateStagingController::apply_staged_update(std::string_view primary_binary_name) {
    UpdateStagingResult res;

    if (record_.current_state != UpdateState::staged &&
        record_.current_state != UpdateState::apply_pending_reboot) {
        res.error = UpdateStagingError::invalid_state;
        res.message = "Cannot apply update: package is not in staged state";
        return res;
    }

    try {
        if (!std::filesystem::exists(record_.staged_package_path)) {
            res.error = UpdateStagingError::io_error;
            res.message = "Staged package does not exist on disk";
            return res;
        }

        // Backup existing active installation
        if (!config_.backup_directory.empty() && std::filesystem::exists(config_.backup_directory)) {
            std::filesystem::remove_all(config_.backup_directory);
        }

        if (std::filesystem::exists(config_.active_directory)) {
            std::filesystem::create_directories(std::filesystem::path(config_.backup_directory).parent_path());
            std::filesystem::rename(config_.active_directory, config_.backup_directory);
            record_.backup_path = config_.backup_directory;
        }

        // Promote staged package to active directory
        std::filesystem::create_directories(config_.active_directory);
        const std::filesystem::path target_binary = std::filesystem::path(config_.active_directory) / primary_binary_name;
        std::filesystem::copy_file(record_.staged_package_path, target_binary,
                                   std::filesystem::copy_options::overwrite_existing);

        record_.current_state = UpdateState::validating;
        record_.state_timestamp_utc = current_time_seconds();
        save_record_to_disk();

        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        if (!record_.backup_path.empty() && std::filesystem::exists(record_.backup_path) &&
            !std::filesystem::exists(config_.active_directory)) {
            try {
                std::filesystem::rename(record_.backup_path, config_.active_directory);
                record_.backup_path.clear();
            } catch (...) {}
        }
        res.error = UpdateStagingError::backup_failed;
        res.message = e.what();
        return res;
    }
}

UpdateStagingResult UpdateStagingController::commit_update() {
    UpdateStagingResult res;

    if (record_.current_state != UpdateState::validating) {
        res.error = UpdateStagingError::invalid_state;
        res.message = "Cannot commit update: controller is not in validating state";
        return res;
    }

    try {
        if (!record_.backup_path.empty() && std::filesystem::exists(record_.backup_path)) {
            std::filesystem::remove_all(record_.backup_path);
        }
        record_.backup_path.clear();

        if (!record_.staged_package_path.empty() && std::filesystem::exists(record_.staged_package_path)) {
            std::filesystem::remove(record_.staged_package_path);
        }

        record_.active_version = record_.target_version;
        record_.current_state = UpdateState::committed;
        record_.state_timestamp_utc = current_time_seconds();
        save_record_to_disk();

        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        res.error = UpdateStagingError::io_error;
        res.message = e.what();
        return res;
    }
}

UpdateStagingResult UpdateStagingController::rollback_update() {
    UpdateStagingResult res;

    if (record_.current_state != UpdateState::validating) {
        res.error = UpdateStagingError::invalid_state;
        res.message = "Cannot rollback update: controller is not in validating state";
        return res;
    }

    try {
        if (record_.backup_path.empty() || !std::filesystem::exists(record_.backup_path)) {
            record_.current_state = UpdateState::failed;
            record_.last_error = "Rollback failed: backup directory does not exist";
            save_record_to_disk();
            res.error = UpdateStagingError::rollback_failed;
            res.message = "Backup directory not found";
            return res;
        }

        if (std::filesystem::exists(config_.active_directory)) {
            std::filesystem::remove_all(config_.active_directory);
        }

        std::filesystem::rename(record_.backup_path, config_.active_directory);
        record_.backup_path.clear();
        record_.current_state = UpdateState::rolling_back;
        record_.last_error = "Update rolled back following validation failure";
        record_.state_timestamp_utc = current_time_seconds();
        save_record_to_disk();

        res.error = UpdateStagingError::ok;
        return res;
    } catch (const std::exception& e) {
        record_.current_state = UpdateState::failed;
        record_.last_error = e.what();
        save_record_to_disk();
        res.error = UpdateStagingError::rollback_failed;
        res.message = e.what();
        return res;
    }
}

} // namespace nstu::update

