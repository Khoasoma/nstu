#pragma once

#include "nstu/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nstu::security {

struct KeyRecord {
    ClientId client_id{};
    std::uint32_t key_id = 0;
    std::vector<std::byte> key;
    bool revoked = false;
};

class KeyStore {
public:
    KeyStore() = default;
    ~KeyStore();
    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;

    [[nodiscard]] bool enroll(const ClientId& client_id, std::uint32_t key_id,
                              std::span<const std::byte> key,
                              std::string* error = nullptr);
    [[nodiscard]] std::optional<std::uint32_t> rotate(
        const ClientId& client_id, std::span<const std::byte> replacement_key,
        std::string* error = nullptr);
    [[nodiscard]] bool revoke(const ClientId& client_id, std::uint32_t key_id,
                              std::string* error = nullptr);
    [[nodiscard]] std::size_t revoke_client(
        const ClientId& client_id) noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> resolve(
        const ClientId& client_id, std::uint32_t key_id) const;
    [[nodiscard]] std::size_t active_key_count() const noexcept;
    [[nodiscard]] std::size_t key_count() const noexcept;
    [[nodiscard]] std::vector<KeyRecord> snapshot() const;
    [[nodiscard]] bool replace(std::span<const KeyRecord> records,
                               std::string* error = nullptr);

private:
    struct Entry {
        ClientId client_id{};
        std::uint32_t key_id = 0;
        std::vector<std::byte> key;
        bool revoked = false;
    };

    [[nodiscard]] static std::string make_lookup_key(
        const ClientId& client_id, std::uint32_t key_id);
    [[nodiscard]] static bool valid_key(std::span<const std::byte> key) noexcept;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace nstu::security
