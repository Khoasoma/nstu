#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::security {

// User-scoped encryption never opts into CRYPTPROTECT_LOCAL_MACHINE. Callers
// still enforce owner-only storage paths and context-bound entropy.
[[nodiscard]] std::vector<std::byte> protect_user_secret(
    std::span<const std::byte> plaintext,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

// DPAPI decryption follows the blob's recorded scope; this function does not
// attest that arbitrary input was encrypted with user scope.
[[nodiscard]] std::vector<std::byte> unprotect_user_secret(
    std::span<const std::byte> protected_blob,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

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
