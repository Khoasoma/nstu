#include "nstu/url_validation.hpp"

#include <algorithm>
#include <cctype>

namespace nstu::url {

namespace {

[[nodiscard]] bool starts_with_ci(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

void set_error(std::string* error, std::string_view message) noexcept {
    if (error != nullptr) {
        *error = std::string(message);
    }
}

} // namespace

bool is_valid_launch_url(std::string_view url, std::string* error) noexcept {
    if (url.empty()) {
        set_error(error, "URL cannot be empty.");
        return false;
    }
    if (url.size() > kMaximumUrlBytes) {
        set_error(error, "URL exceeds maximum allowed length of 2048 bytes.");
        return false;
    }

    std::size_t scheme_len = 0;
    if (starts_with_ci(url, "https://")) {
        scheme_len = 8;
    } else if (starts_with_ci(url, "http://")) {
        scheme_len = 7;
    } else {
        set_error(error, "URL must use http:// or https:// protocol scheme.");
        return false;
    }

    // Extract the host portion (after scheme, up to '/', '?', '#', or end of string)
    std::string_view remainder = url.substr(scheme_len);
    if (remainder.empty()) {
        set_error(error, "URL host component cannot be empty.");
        return false;
    }

    const std::size_t host_end = remainder.find_first_of("/?#");
    const std::string_view host = (host_end == std::string_view::npos)
                                      ? remainder
                                      : remainder.substr(0, host_end);
    if (host.empty()) {
        set_error(error, "URL host component cannot be empty.");
        return false;
    }

    // Verify all characters in the entire URL for safety
    for (std::size_t i = 0; i < url.size(); ++i) {
        const char c = url[i];
        const auto uc = static_cast<unsigned char>(c);

        // Disallow control characters, newlines, carriage returns, tabs, and DEL
        if (uc < 32 || uc == 127) {
            set_error(error, "URL contains invalid control characters or newlines.");
            return false;
        }

        // Disallow literal whitespace (must be percent-encoded)
        if (c == ' ') {
            set_error(error, "URL contains unencoded whitespace; use %20 instead.");
            return false;
        }

        // Disallow quotes and backticks
        if (c == '"' || c == '\'' || c == '`') {
            set_error(error, "URL contains disallowed quote or backtick characters.");
            return false;
        }

        // Disallow angle brackets
        if (c == '<' || c == '>') {
            set_error(error, "URL contains disallowed angle bracket characters.");
            return false;
        }

        // Disallow shell metacharacters and redirects
        if (c == '|' || c == '^' || c == '\\') {
            set_error(error, "URL contains disallowed shell or separator characters.");
            return false;
        }
    }

    return true;
}

std::string sanitize_launch_url(std::string_view raw_url) {
    while (!raw_url.empty() &&
           (raw_url.front() == ' ' || raw_url.front() == '\t' ||
            raw_url.front() == '\r' || raw_url.front() == '\n')) {
        raw_url.remove_prefix(1);
    }
    while (!raw_url.empty() &&
           (raw_url.back() == ' ' || raw_url.back() == '\t' ||
            raw_url.back() == '\r' || raw_url.back() == '\n')) {
        raw_url.remove_suffix(1);
    }
    if (raw_url.empty()) {
        return {};
    }
    if (starts_with_ci(raw_url, "http://") || starts_with_ci(raw_url, "https://")) {
        return std::string(raw_url);
    }
    return "https://" + std::string(raw_url);
}

} // namespace nstu::url

