#include "nstu/key_store.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

namespace {

nstu::security::ClientId make_id(std::byte seed) {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(
            std::to_integer<unsigned int>(seed) + index);
    }
    return id;
}

std::vector<std::byte> make_key(std::byte seed) {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(
            std::to_integer<unsigned int>(seed) + index);
    }
    return key;
}

} // namespace

int main() {
    nstu::security::KeyStore store;
    const auto first_id = make_id(std::byte{1});
    const auto second_id = make_id(std::byte{33});
    const auto first_key = make_key(std::byte{0x40});
    const auto replacement_key = make_key(std::byte{0x70});
    const auto second_key = make_key(std::byte{0x20});
    std::string error;

    assert(!store.enroll(first_id, 0, first_key, &error));
    const std::array<std::byte, 1> short_key{std::byte{1}};
    assert(!store.enroll(first_id, 1, short_key, &error));
    assert(store.enroll(first_id, 7, first_key, &error));
    assert(!store.enroll(first_id, 7, first_key, &error));
    assert(store.resolve(first_id, 7) == first_key);
    assert(store.active_key_count() == 1);

    const auto rotated_id = store.rotate(first_id, replacement_key, &error);
    assert(rotated_id.has_value() && *rotated_id == 8);
    assert(!store.resolve(first_id, 7).has_value());
    assert(store.resolve(first_id, 8) == replacement_key);
    assert(store.key_count() == 2);
    assert(store.active_key_count() == 1);
    assert(!store.enroll(first_id, 7, first_key, &error));

    assert(store.enroll(second_id, 3, second_key, &error));
    assert(store.revoke(second_id, 3, &error));
    assert(!store.resolve(second_id, 3).has_value());
    assert(!store.revoke(second_id, 3, &error));
    assert(store.revoke_client(first_id) == 1);
    assert(store.active_key_count() == 0);
    assert(!store.resolve(first_id, 8).has_value());
    assert(store.revoke_client(first_id) == 0);
    return 0;
}
