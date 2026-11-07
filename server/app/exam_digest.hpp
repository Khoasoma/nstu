#pragma once

#include "nstu/auth.hpp"

#include <string_view>

namespace nstu::ui {

// Accept exactly 64 ASCII hex characters and a digest that is not all zero.
// Failure leaves the caller's output unchanged.
[[nodiscard]] inline bool parse_sha256_hex(
    std::string_view text, security::Sha256Digest& digest) noexcept {
    if (text.size() != digest.size() * 2) {
        return false;
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    security::Sha256Digest candidate{};
    bool non_zero = false;
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        candidate[index] = static_cast<std::byte>((high << 4) | low);
        non_zero = non_zero || candidate[index] != std::byte{0};
    }
    if (!non_zero) {
        return false;
    }
    digest = candidate;
    return true;
}

} // namespace nstu::ui
