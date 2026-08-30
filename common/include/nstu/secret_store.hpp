#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::security {

[[nodiscard]] std::vector<std::byte> protect_machine_secret(
    std::span<const std::byte> plaintext,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::byte> unprotect_machine_secret(
    std::span<const std::byte> protected_blob,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

[[nodiscard]] bool save_machine_secret(
    std::wstring_view path, std::span<const std::byte> plaintext,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::byte> load_machine_secret(
    std::wstring_view path,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

} // namespace nstu::security
