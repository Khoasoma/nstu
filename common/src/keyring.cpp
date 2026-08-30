#include "nstu/keyring.hpp"

#include "nstu/secret_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::security {
namespace {

inline constexpr std::uint32_t kKeyringMagic = 0x31524b4eu; // "NKR1"
inline constexpr std::uint16_t kKeyringVersion = 1;
inline constexpr std::uint16_t kKeyringHeaderBytes = 12;
inline constexpr std::uint32_t kRevokedFlag = 1;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset + sizeof(T) > input.size()) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

void clear_records(std::span<KeyRecord> records) noexcept {
    for (auto& record : records) {
        secure_zero(record.key);
    }
}

} // namespace

std::vector<std::byte> encode_keyring(std::span<const KeyRecord> records,
                                      std::string* error) {
    if (records.size() > kMaximumKeyringRecords ||
        records.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "keyring record limit exceeded");
        return {};
    }
    std::size_t total_bytes = kKeyringHeaderBytes;
    for (const auto& record : records) {
        if (record.key_id == 0 || record.key.size() >
                std::numeric_limits<std::uint32_t>::max() ||
            (record.revoked ? !record.key.empty()
                            : record.key.size() < kMinimumProtocolKeyBytes)) {
            set_error(error, "invalid keyring record");
            return {};
        }
        constexpr std::size_t fixed_record_bytes =
            kClientIdBytes + sizeof(std::uint32_t) * 3;
        if (record.key.size() > std::numeric_limits<std::size_t>::max() -
                                    fixed_record_bytes ||
            fixed_record_bytes + record.key.size() >
                std::numeric_limits<std::size_t>::max() - total_bytes) {
            set_error(error, "keyring size overflow");
            return {};
        }
        total_bytes += fixed_record_bytes + record.key.size();
    }

    std::vector<std::byte> wire;
    wire.reserve(total_bytes);
    append_le(wire, kKeyringMagic);
    append_le(wire, kKeyringVersion);
    append_le(wire, kKeyringHeaderBytes);
    append_le(wire, static_cast<std::uint32_t>(records.size()));
    for (const auto& record : records) {
        wire.insert(wire.end(), record.client_id.begin(),
                    record.client_id.end());
        append_le(wire, record.key_id);
        append_le(wire, record.revoked ? kRevokedFlag : 0u);
        append_le(wire, static_cast<std::uint32_t>(record.key.size()));
        wire.insert(wire.end(), record.key.begin(), record.key.end());
    }
    return wire;
}

std::optional<std::vector<KeyRecord>> decode_keyring(
    std::span<const std::byte> wire, std::string* error) {
    if (wire.size() < kKeyringHeaderBytes) {
        set_error(error, "keyring header is truncated");
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t record_count = 0;
    if (!read_le(wire, offset, magic) || !read_le(wire, offset, version) ||
        !read_le(wire, offset, header_bytes) ||
        !read_le(wire, offset, record_count) || magic != kKeyringMagic ||
        version != kKeyringVersion || header_bytes != kKeyringHeaderBytes ||
        record_count > kMaximumKeyringRecords) {
        set_error(error, "invalid keyring header");
        return std::nullopt;
    }

    std::vector<KeyRecord> records;
    records.reserve(record_count);
    for (std::uint32_t index = 0; index < record_count; ++index) {
        constexpr std::size_t fixed_record_bytes =
            kClientIdBytes + sizeof(std::uint32_t) * 3;
        if (wire.size() - offset < fixed_record_bytes) {
            clear_records(records);
            set_error(error, "keyring record is truncated");
            return std::nullopt;
        }
        KeyRecord record;
        std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                    record.client_id.size(), record.client_id.begin());
        offset += record.client_id.size();
        std::uint32_t flags = 0;
        std::uint32_t key_bytes = 0;
        if (!read_le(wire, offset, record.key_id) ||
            !read_le(wire, offset, flags) ||
            !read_le(wire, offset, key_bytes) ||
            (flags & ~kRevokedFlag) != 0 || record.key_id == 0 ||
            key_bytes > wire.size() - offset) {
            clear_records(records);
            set_error(error, "invalid keyring record");
            return std::nullopt;
        }
        record.revoked = (flags & kRevokedFlag) != 0;
        if ((record.revoked && key_bytes != 0) ||
            (!record.revoked && key_bytes < kMinimumProtocolKeyBytes)) {
            clear_records(records);
            set_error(error, "invalid keyring key length");
            return std::nullopt;
        }
        record.key.assign(
            wire.begin() + static_cast<std::ptrdiff_t>(offset),
            wire.begin() + static_cast<std::ptrdiff_t>(offset + key_bytes));
        offset += key_bytes;
        records.push_back(std::move(record));
    }
    if (offset != wire.size()) {
        clear_records(records);
        set_error(error, "keyring has trailing data");
        return std::nullopt;
    }
    return records;
}

bool save_keyring(const KeyStore& store, std::wstring_view path,
                  std::span<const std::byte> optional_entropy,
                  std::string* error) {
    auto records = store.snapshot();
    auto wire = encode_keyring(records, error);
    const bool succeeded = !wire.empty() &&
        save_machine_secret(path, wire, optional_entropy, error);
    secure_zero(wire);
    clear_records(records);
    return succeeded;
}

bool load_keyring(KeyStore& store, std::wstring_view path,
                  std::span<const std::byte> optional_entropy,
                  std::string* error) {
    auto wire = load_machine_secret(path, optional_entropy, error);
    if (wire.empty()) {
        return false;
    }
    auto records = decode_keyring(wire, error);
    secure_zero(wire);
    if (!records) {
        return false;
    }
    const bool succeeded = store.replace(*records, error);
    clear_records(*records);
    return succeeded;
}

} // namespace nstu::security
