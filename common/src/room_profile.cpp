#include "nstu/room_profile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::fleet {

namespace {

constexpr std::size_t kMaximumProfileBytes = 64 * 1024; // 64 KiB payload limit
constexpr std::size_t kMaximumSeats = 256;
constexpr std::size_t kMaximumPresets = 64;
constexpr std::uint32_t kMaximumGridDimension = 16;
constexpr std::uint32_t kMaximumBeaconInterval = 60;

std::string get_current_utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    struct tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now_c);
#else
    gmtime_r(&now_c, &tm_utc);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string escape_json_string(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 8);
    for (char c : str) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Ignore other control characters
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    return out;
}

bool has_disallowed_control_chars(std::string_view str, bool allow_newline = false) noexcept {
    for (char c : str) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            if (allow_newline && (c == '\n' || c == '\r' || c == '\t')) {
                continue;
            }
            return true;
        }
    }
    return false;
}

} // namespace

bool contains_forbidden_profile_secrets(std::string_view json_text) noexcept {
    static constexpr std::string_view kForbiddenTokens[] = {
        "key", "secret", "token", "password", "private", "entropy",
        "student_answer", "ticket", "sas", "pairing_key", "credential",
        "bearer", "psk"
    };

    std::size_t i = 0;
    while (i < json_text.size()) {
        if (json_text[i] == '"') {
            const std::size_t start = i + 1;
            std::size_t end = start;
            bool escaped = false;
            while (end < json_text.size()) {
                if (!escaped && json_text[end] == '\\') {
                    escaped = true;
                } else if (!escaped && json_text[end] == '"') {
                    break;
                } else {
                    escaped = false;
                }
                ++end;
            }
            if (end >= json_text.size()) break;
            const std::string_view token = json_text.substr(start, end - start);
            
            // Check if followed by ':' (JSON object key)
            std::size_t next = end + 1;
            while (next < json_text.size() && (json_text[next] == ' ' || json_text[next] == '\t' || json_text[next] == '\r' || json_text[next] == '\n')) {
                ++next;
            }
            
            std::string lower;
            lower.reserve(token.size());
            for (char c : token) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }

            if (next < json_text.size() && json_text[next] == ':') {
                for (const auto& forbidden : kForbiddenTokens) {
                    if (lower.find(forbidden) != std::string::npos) {
                        return true;
                    }
                }
            } else {
                if (lower.find("bearer ") != std::string::npos ||
                    lower.find("password=") != std::string::npos ||
                    lower.find("token=") != std::string::npos ||
                    lower.find("secret=") != std::string::npos ||
                    lower.find("begin private key") != std::string::npos) {
                    return true;
                }
            }
            i = end + 1;
        } else {
            ++i;
        }
    }
    return false;
}

bool is_valid_website_preset(std::string_view url) noexcept {
    if (url.empty() || url.size() > 256) return false;
    
    // Check forbidden control chars or whitespace
    for (char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '\'' || c == '@') {
            return false; // Disallow spaces, control chars, quotes, and userinfo credentials
        }
    }

    std::string lower;
    lower.reserve(url.size());
    for (char c : url) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Must start with http:// or https://
    std::string_view host_part;
    if (lower.starts_with("http://")) {
        host_part = url.substr(7);
    } else if (lower.starts_with("https://")) {
        host_part = url.substr(8);
    } else {
        return false;
    }

    if (host_part.empty()) return false;
    if (host_part.front() == '/' || host_part.front() == ':') return false;

    // Disallow dangerous pseudo-schemes
    if (lower.find("javascript:") != std::string::npos ||
        lower.find("file:") != std::string::npos ||
        lower.find("data:") != std::string::npos ||
        lower.find("blob:") != std::string::npos) {
        return false;
    }

    return true;
}

bool is_valid_application_preset(std::string_view app_name) noexcept {
    if (app_name.empty() || app_name.size() > 64) return false;

    // Reject path separators, drives, or directory traversal
    if (app_name.find('/') != std::string::npos ||
        app_name.find('\\') != std::string::npos ||
        app_name.find(':') != std::string::npos ||
        app_name.find("..") != std::string::npos) {
        return false;
    }

    std::string lower;
    lower.reserve(app_name.size());
    for (char c : app_name) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || c == '"' || c == ';' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
            return false;
        }
        lower.push_back(static_cast<char>(std::tolower(uc)));
    }

    // Must end with .exe
    if (!lower.ends_with(".exe") || lower.size() <= 4) {
        return false;
    }

    return true;
}

bool is_valid_hostname(std::string_view hostname) noexcept {
    if (hostname.empty() || hostname.size() > 32) return false;
    if (hostname.front() == '.' || hostname.front() == '-' ||
        hostname.back() == '.' || hostname.back() == '-') {
        return false;
    }
    if (hostname.find("..") != std::string::npos) return false;

    for (char c : hostname) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string export_room_profile_json(const RoomProfile& profile) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema_version\": \"" << escape_json_string(profile.schema_version.empty() ? "1.0" : profile.schema_version) << "\",\n";
    ss << "  \"room_name\": \"" << escape_json_string(profile.room_name) << "\",\n";
    ss << "  \"room_description\": \"" << escape_json_string(profile.room_description) << "\",\n";
    ss << "  \"organization\": \"" << escape_json_string(profile.organization) << "\",\n";
    ss << "  \"grid_columns\": " << (profile.grid_columns == 0 ? 6 : std::min(profile.grid_columns, kMaximumGridDimension)) << ",\n";
    ss << "  \"grid_rows\": " << (profile.grid_rows == 0 ? 5 : std::min(profile.grid_rows, kMaximumGridDimension)) << ",\n";
    ss << "  \"beacon_interval_seconds\": " << (profile.beacon_interval_seconds == 0 ? 3 : std::min(profile.beacon_interval_seconds, kMaximumBeaconInterval)) << ",\n";
    ss << "  \"network_subnet_hint\": \"" << escape_json_string(profile.network_subnet_hint) << "\",\n";
    ss << "  \"default_lock_message_en\": \"" << escape_json_string(profile.default_lock_message_en) << "\",\n";
    ss << "  \"default_lock_message_vi\": \"" << escape_json_string(profile.default_lock_message_vi) << "\",\n";

    // allowed_websites
    ss << "  \"allowed_websites\": [";
    for (std::size_t i = 0; i < profile.allowed_websites.size() && i < kMaximumPresets; ++i) {
        if (i > 0) ss << ", ";
        ss << "\n    \"" << escape_json_string(profile.allowed_websites[i]) << "\"";
    }
    if (!profile.allowed_websites.empty()) ss << "\n  ";
    ss << "],\n";

    // allowed_applications
    ss << "  \"allowed_applications\": [";
    for (std::size_t i = 0; i < profile.allowed_applications.size() && i < kMaximumPresets; ++i) {
        if (i > 0) ss << ", ";
        ss << "\n    \"" << escape_json_string(profile.allowed_applications[i]) << "\"";
    }
    if (!profile.allowed_applications.empty()) ss << "\n  ";
    ss << "],\n";

    // seats
    ss << "  \"seats\": [";
    for (std::size_t i = 0; i < profile.seats.size() && i < kMaximumSeats; ++i) {
        if (i > 0) ss << ",";
        const auto& seat = profile.seats[i];
        ss << "\n    {\n";
        ss << "      \"hostname\": \"" << escape_json_string(seat.hostname) << "\",\n";
        ss << "      \"seat_label\": \"" << escape_json_string(seat.seat_label) << "\",\n";
        ss << "      \"row\": " << seat.row << ",\n";
        ss << "      \"col\": " << seat.col << "\n";
        ss << "    }";
    }
    if (!profile.seats.empty()) ss << "\n  ";
    ss << "],\n";

    const std::string created = profile.created_at_utc.empty() ? get_current_utc_timestamp() : profile.created_at_utc;
    ss << "  \"created_at_utc\": \"" << escape_json_string(created) << "\"\n";
    ss << "}\n";

    return ss.str();
}

namespace {

struct SimpleJsonParser {
    std::string_view input;
    std::size_t pos{0};

    void skip_whitespace() noexcept {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n')) {
            ++pos;
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

    bool parse_string(std::string& out) {
        skip_whitespace();
        if (pos >= input.size() || input[pos] != '"') return false;
        ++pos;
        out.clear();
        bool escaped = false;
        while (pos < input.size()) {
            char c = input[pos++];
            if (escaped) {
                switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 <= input.size()) {
                        // Skip unicode 4-hex codepoint for simplicity or replace with '?'
                        pos += 4;
                        out.push_back('?');
                    }
                    break;
                }
                default: out.push_back(c); break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return true;
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_uint(std::uint32_t& out) {
        skip_whitespace();
        if (pos >= input.size() || !std::isdigit(static_cast<unsigned char>(input[pos]))) return false;
        std::uint64_t val = 0;
        while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
            val = val * 10 + (input[pos++] - '0');
            if (val > 0xFFFFFFFFULL) return false;
        }
        out = static_cast<std::uint32_t>(val);
        return true;
    }

    bool skip_value() {
        skip_whitespace();
        if (pos >= input.size()) return false;
        char c = input[pos];
        if (c == '"') {
            std::string discard;
            return parse_string(discard);
        }
        if (c == '{') {
            ++pos;
            int depth = 1;
            while (pos < input.size() && depth > 0) {
                if (input[pos] == '"') {
                    std::string discard;
                    if (!parse_string(discard)) return false;
                    continue;
                }
                if (input[pos] == '{') ++depth;
                else if (input[pos] == '}') --depth;
                ++pos;
            }
            return depth == 0;
        }
        if (c == '[') {
            ++pos;
            int depth = 1;
            while (pos < input.size() && depth > 0) {
                if (input[pos] == '"') {
                    std::string discard;
                    if (!parse_string(discard)) return false;
                    continue;
                }
                if (input[pos] == '[') ++depth;
                else if (input[pos] == ']') --depth;
                ++pos;
            }
            return depth == 0;
        }
        // Scalar value (number, bool, null)
        while (pos < input.size() && input[pos] != ',' && input[pos] != '}' && input[pos] != ']' &&
               input[pos] != ' ' && input[pos] != '\t' && input[pos] != '\r' && input[pos] != '\n') {
            ++pos;
        }
        return true;
    }
};

} // namespace

RoomProfileResult import_room_profile_json(std::string_view json_str) {
    RoomProfileResult result;

    if (json_str.size() > kMaximumProfileBytes) {
        result.error = RoomProfileError::oversized_payload;
        result.error_message = "Payload exceeds maximum allowed size of 64 KiB";
        return result;
    }

    if (contains_forbidden_profile_secrets(json_str)) {
        result.error = RoomProfileError::forbidden_secret_detected;
        result.error_message = "Payload contains forbidden secret key or canary pattern";
        return result;
    }

    SimpleJsonParser parser{json_str, 0};
    if (!parser.match_char('{')) {
        result.error = RoomProfileError::invalid_json;
        result.error_message = "Expected JSON object at root";
        return result;
    }

    bool has_room_name = false;

    while (true) {
        parser.skip_whitespace();
        if (parser.match_char('}')) {
            break;
        }

        std::string key;
        if (!parser.parse_string(key)) {
            result.error = RoomProfileError::invalid_json;
            result.error_message = "Failed to parse JSON key";
            return result;
        }

        if (!parser.match_char(':')) {
            result.error = RoomProfileError::invalid_json;
            result.error_message = "Expected ':' after key '" + key + "'";
            return result;
        }

        if (key == "schema_version") {
            std::string ver;
            if (!parser.parse_string(ver)) {
                result.error = RoomProfileError::invalid_json;
                result.error_message = "Failed to parse schema_version string";
                return result;
            }
            if (ver != "1.0") {
                result.error = RoomProfileError::unsupported_schema;
                result.error_message = "Unsupported schema_version: " + ver;
                return result;
            }
            result.profile.schema_version = ver;
        } else if (key == "room_name") {
            std::string name;
            if (!parser.parse_string(name)) {
                result.error = RoomProfileError::invalid_json;
                result.error_message = "Failed to parse room_name string";
                return result;
            }
            if (name.empty()) {
                result.error = RoomProfileError::missing_required_field;
                result.error_message = "room_name cannot be empty";
                return result;
            }
            if (name.size() > 64 || has_disallowed_control_chars(name)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "room_name exceeds 64 chars or contains invalid characters";
                return result;
            }
            result.profile.room_name = name;
            has_room_name = true;
        } else if (key == "room_description") {
            std::string desc;
            if (!parser.parse_string(desc)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            if (desc.size() > 128 || has_disallowed_control_chars(desc)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "room_description exceeds 128 chars or contains invalid characters";
                return result;
            }
            result.profile.room_description = desc;
        } else if (key == "organization") {
            std::string org;
            if (!parser.parse_string(org)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            if (org.size() > 64 || has_disallowed_control_chars(org)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "organization exceeds 64 chars or contains invalid characters";
                return result;
            }
            result.profile.organization = org;
        } else if (key == "grid_columns") {
            std::uint32_t cols = 0;
            if (!parser.parse_uint(cols) || cols == 0 || cols > kMaximumGridDimension) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "grid_columns must be between 1 and 16";
                return result;
            }
            result.profile.grid_columns = cols;
        } else if (key == "grid_rows") {
            std::uint32_t rows = 0;
            if (!parser.parse_uint(rows) || rows == 0 || rows > kMaximumGridDimension) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "grid_rows must be between 1 and 16";
                return result;
            }
            result.profile.grid_rows = rows;
        } else if (key == "beacon_interval_seconds") {
            std::uint32_t interval = 0;
            if (!parser.parse_uint(interval) || interval == 0 || interval > kMaximumBeaconInterval) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "beacon_interval_seconds must be between 1 and 60";
                return result;
            }
            result.profile.beacon_interval_seconds = interval;
        } else if (key == "network_subnet_hint") {
            std::string hint;
            if (!parser.parse_string(hint)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            if (hint.size() > 64 || has_disallowed_control_chars(hint)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "network_subnet_hint exceeds 64 chars";
                return result;
            }
            result.profile.network_subnet_hint = hint;
        } else if (key == "default_lock_message_en") {
            std::string msg;
            if (!parser.parse_string(msg)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            if (msg.size() > 256 || has_disallowed_control_chars(msg, true)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "default_lock_message_en exceeds 256 chars or contains invalid characters";
                return result;
            }
            result.profile.default_lock_message_en = msg;
        } else if (key == "default_lock_message_vi") {
            std::string msg;
            if (!parser.parse_string(msg)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            if (msg.size() > 256 || has_disallowed_control_chars(msg, true)) {
                result.error = RoomProfileError::validation_failed;
                result.error_message = "default_lock_message_vi exceeds 256 chars or contains invalid characters";
                return result;
            }
            result.profile.default_lock_message_vi = msg;
        } else if (key == "created_at_utc") {
            std::string ts;
            if (!parser.parse_string(ts)) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            result.profile.created_at_utc = ts;
        } else if (key == "allowed_websites") {
            if (!parser.match_char('[')) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            while (true) {
                parser.skip_whitespace();
                if (parser.match_char(']')) break;
                std::string url;
                if (!parser.parse_string(url)) {
                    result.error = RoomProfileError::invalid_json;
                    return result;
                }
                if (!is_valid_website_preset(url)) {
                    result.error = RoomProfileError::validation_failed;
                    result.error_message = "Invalid website preset: " + url;
                    return result;
                }
                if (result.profile.allowed_websites.size() < kMaximumPresets) {
                    result.profile.allowed_websites.push_back(url);
                }
                parser.match_char(',');
            }
        } else if (key == "allowed_applications") {
            if (!parser.match_char('[')) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            while (true) {
                parser.skip_whitespace();
                if (parser.match_char(']')) break;
                std::string app;
                if (!parser.parse_string(app)) {
                    result.error = RoomProfileError::invalid_json;
                    return result;
                }
                if (!is_valid_application_preset(app)) {
                    result.error = RoomProfileError::validation_failed;
                    result.error_message = "Invalid application preset: " + app;
                    return result;
                }
                if (result.profile.allowed_applications.size() < kMaximumPresets) {
                    result.profile.allowed_applications.push_back(app);
                }
                parser.match_char(',');
            }
        } else if (key == "seats") {
            if (!parser.match_char('[')) {
                result.error = RoomProfileError::invalid_json;
                return result;
            }
            while (true) {
                parser.skip_whitespace();
                if (parser.match_char(']')) break;
                if (!parser.match_char('{')) {
                    result.error = RoomProfileError::invalid_json;
                    return result;
                }
                SeatAssignment seat;
                bool has_host = false;
                while (true) {
                    parser.skip_whitespace();
                    if (parser.match_char('}')) break;
                    std::string skey;
                    if (!parser.parse_string(skey) || !parser.match_char(':')) {
                        result.error = RoomProfileError::invalid_json;
                        return result;
                    }
                    if (skey == "hostname") {
                        if (!parser.parse_string(seat.hostname) || !is_valid_hostname(seat.hostname)) {
                            result.error = RoomProfileError::validation_failed;
                            result.error_message = "Invalid seat hostname: " + seat.hostname;
                            return result;
                        }
                        has_host = true;
                    } else if (skey == "seat_label") {
                        if (!parser.parse_string(seat.seat_label) || seat.seat_label.size() > 16 || has_disallowed_control_chars(seat.seat_label)) {
                            result.error = RoomProfileError::validation_failed;
                            result.error_message = "Invalid seat_label";
                            return result;
                        }
                    } else if (skey == "row") {
                        if (!parser.parse_uint(seat.row)) {
                            result.error = RoomProfileError::invalid_json;
                            return result;
                        }
                    } else if (skey == "col") {
                        if (!parser.parse_uint(seat.col)) {
                            result.error = RoomProfileError::invalid_json;
                            return result;
                        }
                    } else {
                        parser.skip_value();
                    }
                    parser.match_char(',');
                }
                if (!has_host) {
                    result.error = RoomProfileError::validation_failed;
                    result.error_message = "Seat missing hostname";
                    return result;
                }
                if (result.profile.seats.size() < kMaximumSeats) {
                    result.profile.seats.push_back(seat);
                }
                parser.match_char(',');
            }
        } else {
            // Forward-compatible: safely skip unknown benign fields
            parser.skip_value();
        }

        parser.match_char(',');
    }

    if (!has_room_name) {
        result.error = RoomProfileError::missing_required_field;
        result.error_message = "Missing required 'room_name' field";
        return result;
    }

    // Post-parse validation: verify seat coordinates against parsed grid bounds
    for (const auto& seat : result.profile.seats) {
        if (seat.row >= result.profile.grid_rows || seat.col >= result.profile.grid_columns) {
            result.error = RoomProfileError::validation_failed;
            result.error_message = "Seat coordinate (" + std::to_string(seat.row) + ", " +
                                  std::to_string(seat.col) + ") exceeds grid dimensions (" +
                                  std::to_string(result.profile.grid_rows) + ", " +
                                  std::to_string(result.profile.grid_columns) + ")";
            return result;
        }
    }

    result.error = RoomProfileError::ok;
    return result;
}

} // namespace nstu::fleet

