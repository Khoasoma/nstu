#include "nstu/file_distribution.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace nstu::file_dist {
namespace {

void write_u16_le(std::uint8_t* dst, std::uint16_t val) noexcept {
    dst[0] = static_cast<std::uint8_t>(val & 0xFF);
    dst[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
}

[[nodiscard]] std::uint16_t read_u16_le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>(src[0]) |
           (static_cast<std::uint16_t>(src[1]) << 8);
}

void write_u32_le(std::uint8_t* dst, std::uint32_t val) noexcept {
    dst[0] = static_cast<std::uint8_t>(val & 0xFF);
    dst[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    dst[2] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    dst[3] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
}

[[nodiscard]] std::uint32_t read_u32_le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

void write_u64_le(std::uint8_t* dst, std::uint64_t val) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((val >> (i * 8)) & 0xFF);
    }
}

[[nodiscard]] std::uint64_t read_u64_le(const std::uint8_t* src) noexcept {
    std::uint64_t val = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        val |= (static_cast<std::uint64_t>(src[i]) << (i * 8));
    }
    return val;
}

bool equals_case_insensitive(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool is_reserved_dos_device(std::string_view stem) noexcept {
    static constexpr std::array<std::string_view, 22> kReserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    for (const auto& dev : kReserved) {
        if (equals_case_insensitive(stem, dev)) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* to_string(FileDistStatus status) noexcept {
    switch (status) {
    case FileDistStatus::in_progress:
        return "in_progress";
    case FileDistStatus::completed:
        return "completed";
    case FileDistStatus::hash_mismatch:
        return "hash_mismatch";
    case FileDistStatus::write_error:
        return "write_error";
    case FileDistStatus::cancelled:
        return "cancelled";
    case FileDistStatus::rejected_exam_active:
        return "rejected_exam_active";
    case FileDistStatus::rejected_unsafe_path:
        return "rejected_unsafe_path";
    case FileDistStatus::rejected_quota_exceeded:
        return "rejected_quota_exceeded";
    }
    return "unknown";
}

const char* status_label(FileDistStatus status, bool vietnamese) noexcept {
    switch (status) {
    case FileDistStatus::in_progress:
        return vietnamese ? "Đang truyền" : "In Progress";
    case FileDistStatus::completed:
        return vietnamese ? "Hoàn thành" : "Completed";
    case FileDistStatus::hash_mismatch:
        return vietnamese ? "Lỗi toàn vẹn (sai mã băm)" : "Hash Mismatch";
    case FileDistStatus::write_error:
        return vietnamese ? "Lỗi ghi tệp" : "Write Error";
    case FileDistStatus::cancelled:
        return vietnamese ? "Đã hủy" : "Cancelled";
    case FileDistStatus::rejected_exam_active:
        return vietnamese ? "Bị từ chối: Đang diễn ra kỳ thi" : "Rejected: Exam Active";
    case FileDistStatus::rejected_unsafe_path:
        return vietnamese ? "Bị từ chối: Tên tệp không an toàn" : "Rejected: Unsafe Filename";
    case FileDistStatus::rejected_quota_exceeded:
        return vietnamese ? "Bị từ chối: Vượt quá dung lượng" : "Rejected: Quota Exceeded";
    }
    return vietnamese ? "Chưa rõ" : "Unknown";
}

bool is_safe_filename(std::string_view filename) noexcept {
    if (filename.empty() || filename.size() > kMaxFilenameLength) {
        return false;
    }

    // Prohibit leading/trailing spaces and dots
    if (filename.front() == ' ' || filename.front() == '.' ||
        filename.back() == ' ' || filename.back() == '.') {
        return false;
    }

    // Strict character validation: no path separators, no Windows forbidden chars, no control chars
    for (char c : filename) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            return false;
        }
        switch (c) {
        case '/':
        case '\\':
        case ':': // Also prevents NTFS Alternate Data Streams (e.g. evil.exe:$DATA)
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            return false;
        default:
            break;
        }
    }

    // Relative directory traversal tokens
    if (filename == "." || filename == "..") {
        return false;
    }

    // Extract stem before the first dot to identify DOS device names with arbitrary extensions
    // e.g. "con.txt" -> "con", "aux.tar.gz" -> "aux"
    const auto first_dot = filename.find('.');
    const auto stem = (first_dot == std::string_view::npos)
        ? filename
        : filename.substr(0, first_dot);

    if (is_reserved_dos_device(stem)) {
        return false;
    }

    return true;
}

std::string sanitize_filename(std::string_view filename) {
    // Strip any preceding directory path if provided
    const auto last_slash = filename.find_last_of("/\\");
    if (last_slash != std::string_view::npos) {
        filename = filename.substr(last_slash + 1);
    }

    if (filename.empty()) {
        return "received_file.bin";
    }

    std::string sanitized;
    sanitized.reserve(std::min(filename.size(), kMaxFilenameLength));

    for (char c : filename) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            sanitized.push_back('_');
            continue;
        }
        switch (c) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            sanitized.push_back('_');
            break;
        default:
            sanitized.push_back(c);
            break;
        }
    }

    // Trim leading/trailing spaces and dots
    const auto first_valid = sanitized.find_first_not_of(" .");
    if (first_valid == std::string::npos) {
        return "received_file.bin";
    }
    const auto last_valid = sanitized.find_last_not_of(" .");
    sanitized = sanitized.substr(first_valid, last_valid - first_valid + 1);

    if (sanitized.size() > kMaxFilenameLength) {
        sanitized.resize(kMaxFilenameLength);
        const auto trim_last = sanitized.find_last_not_of(" .");
        if (trim_last == std::string::npos) {
            return "received_file.bin";
        }
        sanitized = sanitized.substr(0, trim_last + 1);
    }

    // Check if stem is a reserved DOS device
    const auto first_dot = sanitized.find('.');
    const auto stem = (first_dot == std::string::npos)
        ? std::string_view(sanitized)
        : std::string_view(sanitized).substr(0, first_dot);

    if (is_reserved_dos_device(stem)) {
        sanitized = "safe_" + sanitized;
        if (sanitized.size() > kMaxFilenameLength) {
            sanitized.resize(kMaxFilenameLength);
        }
    }

    return is_safe_filename(sanitized) ? sanitized : "received_file.bin";
}

std::optional<std::filesystem::path> compute_staging_path(
    const std::filesystem::path& base_dir,
    std::string_view filename) {
    if (!is_safe_filename(filename)) {
        return std::nullopt;
    }

    const auto target_path = base_dir / filename;
    const auto rel = target_path.lexically_relative(base_dir);

    // If relative path starts with ".." or is empty, it escaped the base directory
    const auto rel_str = rel.string();
    if (rel_str.empty() || rel_str.rfind("..", 0) != std::string::npos) {
        return std::nullopt;
    }

    return target_path;
}

std::vector<std::uint8_t> encode_file_dist_start(
    const FileDistStartMessage& msg) {
    if (msg.filename.empty() || msg.filename.size() > kMaxFilenameLength ||
        !is_safe_filename(msg.filename) || msg.file_size > kMaxFileSize) {
        return {};
    }

    // Layout:
    // [0..7]:   session_id (8 bytes)
    // [8..15]:  file_size (8 bytes)
    // [16..47]: sha256 (32 bytes)
    // [48..49]: filename_len (2 bytes)
    // [50..]:   filename (variable bytes)
    const std::size_t total_size = 50 + msg.filename.size();
    std::vector<std::uint8_t> payload(total_size);

    write_u64_le(&payload[0], msg.session_id);
    write_u64_le(&payload[8], msg.file_size);
    std::memcpy(&payload[16], msg.sha256.data(), 32);
    write_u16_le(&payload[48], static_cast<std::uint16_t>(msg.filename.size()));
    std::memcpy(&payload[50], msg.filename.data(), msg.filename.size());

    return payload;
}

std::optional<FileDistStartMessage> decode_file_dist_start(
    std::span<const std::uint8_t> payload) {
    if (payload.size() < 51) { // 50 bytes header + at least 1 byte filename
        return std::nullopt;
    }

    const std::uint64_t session_id = read_u64_le(&payload[0]);
    const std::uint64_t file_size = read_u64_le(&payload[8]);
    if (file_size > kMaxFileSize) {
        return std::nullopt;
    }

    nstu::security::Sha256Digest digest{};
    std::memcpy(digest.data(), &payload[16], 32);

    const std::uint16_t filename_len = read_u16_le(&payload[48]);
    if (filename_len == 0 || filename_len > kMaxFilenameLength) {
        return std::nullopt;
    }
    if (payload.size() != static_cast<std::size_t>(50) + filename_len) {
        return std::nullopt;
    }

    std::string filename(
        reinterpret_cast<const char*>(&payload[50]),
        filename_len);

    if (!is_safe_filename(filename)) {
        return std::nullopt;
    }

    return FileDistStartMessage{
        .session_id = session_id,
        .file_size = file_size,
        .sha256 = digest,
        .filename = std::move(filename),
    };
}

std::vector<std::uint8_t> encode_file_dist_chunk(
    const FileDistChunkMessage& msg) {
    if (msg.data.empty() || msg.data.size() > kMaxChunkSize) {
        return {};
    }

    // Layout:
    // [0..7]:   session_id (8 bytes)
    // [8..15]:  offset (8 bytes)
    // [16..19]: data_len (4 bytes)
    // [20..]:   data (variable bytes)
    const std::size_t total_size = 20 + msg.data.size();
    std::vector<std::uint8_t> payload(total_size);

    write_u64_le(&payload[0], msg.session_id);
    write_u64_le(&payload[8], msg.offset);
    write_u32_le(&payload[16], static_cast<std::uint32_t>(msg.data.size()));
    std::memcpy(&payload[20], msg.data.data(), msg.data.size());

    return payload;
}

std::optional<FileDistChunkMessage> decode_file_dist_chunk(
    std::span<const std::uint8_t> payload) {
    if (payload.size() < 21) { // 20 bytes header + at least 1 byte data
        return std::nullopt;
    }

    const std::uint64_t session_id = read_u64_le(&payload[0]);
    const std::uint64_t offset = read_u64_le(&payload[8]);
    const std::uint32_t data_len = read_u32_le(&payload[16]);

    if (data_len == 0 || data_len > kMaxChunkSize) {
        return std::nullopt;
    }
    if (payload.size() != static_cast<std::size_t>(20) + data_len) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> data(
        payload.begin() + 20,
        payload.end());

    return FileDistChunkMessage{
        .session_id = session_id,
        .offset = offset,
        .data = std::move(data),
    };
}

std::vector<std::uint8_t> encode_file_dist_finish(
    const FileDistFinishMessage& msg) {
    // Layout:
    // [0..7]: session_id (8 bytes)
    std::vector<std::uint8_t> payload(8);
    write_u64_le(&payload[0], msg.session_id);
    return payload;
}

std::optional<FileDistFinishMessage> decode_file_dist_finish(
    std::span<const std::uint8_t> payload) {
    if (payload.size() != 8) {
        return std::nullopt;
    }
    return FileDistFinishMessage{
        .session_id = read_u64_le(&payload[0]),
    };
}

std::vector<std::uint8_t> encode_file_dist_status(
    const FileDistStatusMessage& msg) {
    // Layout:
    // [0..7]:   session_id (8 bytes)
    // [8..15]:  bytes_received (8 bytes)
    // [16]:     status (1 byte)
    // [17..23]: reserved (7 bytes zero padding for 24-byte alignment)
    std::vector<std::uint8_t> payload(24, 0);
    write_u64_le(&payload[0], msg.session_id);
    write_u64_le(&payload[8], msg.bytes_received);
    payload[16] = static_cast<std::uint8_t>(msg.status);
    return payload;
}

std::optional<FileDistStatusMessage> decode_file_dist_status(
    std::span<const std::uint8_t> payload) {
    if (payload.size() != 24) {
        return std::nullopt;
    }

    const std::uint8_t status_byte = payload[16];
    if (status_byte > static_cast<std::uint8_t>(FileDistStatus::rejected_quota_exceeded)) {
        return std::nullopt;
    }

    return FileDistStatusMessage{
        .session_id = read_u64_le(&payload[0]),
        .bytes_received = read_u64_le(&payload[8]),
        .status = static_cast<FileDistStatus>(status_byte),
    };
}

std::vector<std::uint8_t> encode_file_dist_cancel(
    const FileDistCancelMessage& msg) {
    // Layout:
    // [0..7]:  session_id (8 bytes)
    // [8..11]: reason_code (4 bytes)
    // [12..15]: reserved (4 bytes zero padding)
    std::vector<std::uint8_t> payload(16, 0);
    write_u64_le(&payload[0], msg.session_id);
    write_u32_le(&payload[8], msg.reason_code);
    return payload;
}

std::optional<FileDistCancelMessage> decode_file_dist_cancel(
    std::span<const std::uint8_t> payload) {
    if (payload.size() != 16) {
        return std::nullopt;
    }
    return FileDistCancelMessage{
        .session_id = read_u64_le(&payload[0]),
        .reason_code = read_u32_le(&payload[8]),
    };
}

} // namespace nstu::file_dist

