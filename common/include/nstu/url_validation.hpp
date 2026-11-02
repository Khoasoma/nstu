#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace nstu::url {

// Standard maximum length for HTTP/HTTPS URLs across web platforms.
inline constexpr std::size_t kMaximumUrlBytes = 2048;

// Validates whether the given URL is safe for client browser launch.
// Enforces strict safety rules:
// 1. Non-empty and at most kMaximumUrlBytes.
// 2. Protocol scheme strictly http:// or https:// (case-insensitive).
// 3. Rejection of dangerous schemes (file:, javascript:, data:, vbscript:, etc.).
// 4. Rejection of control characters (ASCII < 32 or 127), newlines (\r, \n), and null bytes.
// 5. Rejection of quotes (", '), backticks, and angle brackets (<, >).
// 6. Rejection of shell metacharacters and separators (^, |, ;, &, $, %, newline).
// 7. Requires a non-empty host/domain component following the scheme.
// Returns true if valid, or false with an explanatory message if error != nullptr.
[[nodiscard]] bool is_valid_launch_url(
    std::string_view url,
    std::string* error = nullptr) noexcept;

// Sanitizes a user-input string for URL launch:
// - Trims leading and trailing whitespace.
// - If no scheme is present (e.g. "classroom.google.com"), prefixes "https://".
[[nodiscard]] std::string sanitize_launch_url(
    std::string_view raw_url);

} // namespace nstu::url

