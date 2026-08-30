#include "nstu/key_store.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <tuple>

namespace nstu::security {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

KeyStore::~KeyStore() {
    std::unique_lock lock(mutex_);
    for (auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        secure_zero(entry.key);
    }
}

bool KeyStore::enroll(const ClientId& client_id, std::uint32_t key_id,
                      std::span<const std::byte> key, std::string* error) {
    if (key_id == 0 || !valid_key(key)) {
        set_error(error, "invalid key enrollment");
        return false;
    }
    const auto lookup_key = make_lookup_key(client_id, key_id);
    std::unique_lock lock(mutex_);
    if (entries_.contains(lookup_key)) {
        set_error(error, "key ID is already allocated");
        return false;
    }
    Entry entry;
    entry.client_id = client_id;
    entry.key_id = key_id;
    entry.key.assign(key.begin(), key.end());
    entries_.emplace(lookup_key, std::move(entry));
    return true;
}

std::optional<std::uint32_t> KeyStore::rotate(
    const ClientId& client_id, std::span<const std::byte> replacement_key,
    std::string* error) {
    if (!valid_key(replacement_key)) {
        set_error(error, "invalid replacement key");
        return std::nullopt;
    }
    std::unique_lock lock(mutex_);
    std::uint32_t highest = 0;
    for (const auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        if (entry.client_id == client_id) {
            highest = std::max(highest, entry.key_id);
        }
    }
    if (highest == std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "key ID space is exhausted");
        return std::nullopt;
    }
    const auto next_id = highest + 1;
    Entry replacement;
    replacement.client_id = client_id;
    replacement.key_id = next_id;
    replacement.key.assign(replacement_key.begin(), replacement_key.end());
    entries_.emplace(make_lookup_key(client_id, next_id),
                     std::move(replacement));
    for (auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        if (entry.client_id == client_id && entry.key_id != next_id &&
            !entry.revoked) {
            entry.revoked = true;
            secure_zero(entry.key);
        }
    }
    return next_id;
}

bool KeyStore::revoke(const ClientId& client_id, std::uint32_t key_id,
                      std::string* error) {
    if (key_id == 0) {
        set_error(error, "invalid key ID");
        return false;
    }
    std::unique_lock lock(mutex_);
    const auto found = entries_.find(make_lookup_key(client_id, key_id));
    if (found == entries_.end() || found->second.revoked) {
        set_error(error, "key is not active");
        return false;
    }
    found->second.revoked = true;
    secure_zero(found->second.key);
    return true;
}

std::size_t KeyStore::revoke_client(const ClientId& client_id) noexcept {
    std::unique_lock lock(mutex_);
    std::size_t revoked = 0;
    for (auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        if (entry.client_id == client_id && !entry.revoked) {
            entry.revoked = true;
            secure_zero(entry.key);
            ++revoked;
        }
    }
    return revoked;
}

std::optional<std::vector<std::byte>> KeyStore::resolve(
    const ClientId& client_id, std::uint32_t key_id) const {
    if (key_id == 0) {
        return std::nullopt;
    }
    std::shared_lock lock(mutex_);
    const auto found = entries_.find(make_lookup_key(client_id, key_id));
    if (found == entries_.end() || found->second.revoked ||
        !valid_key(found->second.key)) {
        return std::nullopt;
    }
    return found->second.key;
}

std::size_t KeyStore::active_key_count() const noexcept {
    std::shared_lock lock(mutex_);
    std::size_t count = 0;
    for (const auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        if (!entry.revoked) {
            ++count;
        }
    }
    return count;
}

std::size_t KeyStore::key_count() const noexcept {
    std::shared_lock lock(mutex_);
    return entries_.size();
}

std::vector<KeyRecord> KeyStore::snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<KeyRecord> records;
    records.reserve(entries_.size());
    for (const auto& [lookup_key, entry] : entries_) {
        (void)lookup_key;
        KeyRecord record;
        record.client_id = entry.client_id;
        record.key_id = entry.key_id;
        record.revoked = entry.revoked;
        if (!entry.revoked) {
            record.key = entry.key;
        }
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const auto& left,
                                                  const auto& right) {
        return std::tie(left.client_id, left.key_id) <
               std::tie(right.client_id, right.key_id);
    });
    return records;
}

bool KeyStore::replace(std::span<const KeyRecord> records,
                       std::string* error) {
    std::unordered_map<std::string, Entry> replacement;
    replacement.reserve(records.size());
    const auto wipe_replacement = [&replacement] {
        for (auto& [lookup_key, entry] : replacement) {
            (void)lookup_key;
            secure_zero(entry.key);
        }
    };
    for (const auto& record : records) {
        if (record.key_id == 0 ||
            (record.revoked ? !record.key.empty() : !valid_key(record.key))) {
            wipe_replacement();
            set_error(error, "invalid keyring record");
            return false;
        }
        Entry entry;
        entry.client_id = record.client_id;
        entry.key_id = record.key_id;
        entry.revoked = record.revoked;
        if (!record.revoked) {
            entry.key = record.key;
        }
        const auto lookup_key = make_lookup_key(record.client_id,
                                                 record.key_id);
        if (!replacement.emplace(lookup_key, std::move(entry)).second) {
            secure_zero(entry.key);
            wipe_replacement();
            set_error(error, "duplicate keyring record");
            return false;
        }
    }

    std::unique_lock lock(mutex_);
    entries_.swap(replacement);
    lock.unlock();
    wipe_replacement();
    return true;
}

std::string KeyStore::make_lookup_key(const ClientId& client_id,
                                      std::uint32_t key_id) {
    std::string lookup;
    lookup.resize(client_id.size() + sizeof(key_id));
    for (std::size_t index = 0; index < client_id.size(); ++index) {
        lookup[index] = static_cast<char>(client_id[index]);
    }
    for (std::size_t index = 0; index < sizeof(key_id); ++index) {
        lookup[client_id.size() + index] = static_cast<char>(key_id & 0xffu);
        key_id >>= 8u;
    }
    return lookup;
}

bool KeyStore::valid_key(std::span<const std::byte> key) noexcept {
    if (key.size() < kMinimumProtocolKeyBytes) {
        return false;
    }
    return std::any_of(key.begin(), key.end(),
                       [](std::byte value) { return value != std::byte{0}; });
}

} // namespace nstu::security
