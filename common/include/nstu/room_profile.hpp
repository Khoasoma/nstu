#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::fleet {

struct SeatAssignment {
    std::string hostname;    // Client machine hostname (max 32 chars, alphanumeric, '.', '-')
    std::string seat_label;  // Operator-assigned seat label, e.g. "A-01", "B-03" (max 16 chars)
    std::uint32_t row{0};    // 0-indexed row on grid
    std::uint32_t col{0};    // 0-indexed column on grid
};

struct RoomProfile {
    std::string schema_version{"1.0"};
    std::string room_name;                        // e.g. "Lab 301" (max 64 chars, required)
    std::string room_description;                 // e.g. "Computer Science Lab 3" (max 128 chars)
    std::string organization;                     // e.g. "High School No. 1" (max 64 chars)
    std::uint32_t grid_columns{6};                // Bounds: [1, 16]
    std::uint32_t grid_rows{5};                   // Bounds: [1, 16]
    std::vector<SeatAssignment> seats;            // Max 256 seats
    std::vector<std::string> allowed_websites;    // Max 64 URLs (http/https only, max 256 chars each)
    std::vector<std::string> allowed_applications;// Max 64 apps (filenames only, max 64 chars each)
    std::string default_lock_message_en;          // Max 256 chars
    std::string default_lock_message_vi;          // Max 256 chars
    std::string network_subnet_hint;              // Optional display hint e.g. "192.168.1.0/24" (max 64 chars)
    std::uint32_t beacon_interval_seconds{3};     // Bounds: [1, 60]
    std::string created_at_utc;                   // ISO 8601 UTC timestamp
};

enum class RoomProfileError : int {
    ok = 0,
    invalid_json = 1,
    oversized_payload = 2,
    unsupported_schema = 3,
    forbidden_secret_detected = 4,
    validation_failed = 5,
    missing_required_field = 6
};

struct RoomProfileResult {
    RoomProfile profile;
    RoomProfileError error{RoomProfileError::ok};
    std::string error_message;

    [[nodiscard]] bool ok() const noexcept {
        return error == RoomProfileError::ok;
    }
};

// Check whether candidate JSON or profile text contains forbidden secret keys or canaries.
[[nodiscard]] bool contains_forbidden_profile_secrets(std::string_view json_text) noexcept;

// Export a RoomProfile to canonical formatted JSON string.
[[nodiscard]] std::string export_room_profile_json(const RoomProfile& profile);

// Import and validate a RoomProfile from JSON string. Fail-closed on secrets, invalid schemas, or bounds violations.
[[nodiscard]] RoomProfileResult import_room_profile_json(std::string_view json_str);

// Validate whether a website preset is safe (scheme http/https, no userinfo, bounded length).
[[nodiscard]] bool is_valid_website_preset(std::string_view url) noexcept;

// Validate whether an application preset is safe (filename only, no path traversal, bounded length).
[[nodiscard]] bool is_valid_application_preset(std::string_view app_name) noexcept;

// Validate whether a hostname is safe (RFC 1123 compliant, bounded length).
[[nodiscard]] bool is_valid_hostname(std::string_view hostname) noexcept;

} // namespace nstu::fleet

