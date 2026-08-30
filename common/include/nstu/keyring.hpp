#pragma once

#include "nstu/key_store.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::security {

inline constexpr std::size_t kMaximumKeyringRecords = 100'000;

[[nodiscard]] std::vector<std::byte> encode_keyring(
    std::span<const KeyRecord> records, std::string* error = nullptr);

[[nodiscard]] std::optional<std::vector<KeyRecord>> decode_keyring(
    std::span<const std::byte> wire, std::string* error = nullptr);

[[nodiscard]] bool save_keyring(
    const KeyStore& store, std::wstring_view path,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

[[nodiscard]] bool load_keyring(
    KeyStore& store, std::wstring_view path,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

} // namespace nstu::security
