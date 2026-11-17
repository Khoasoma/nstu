#include "nstu/secret_store.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <span>
#include <string>

int main() {
    using namespace nstu::security;
    const std::array source{std::byte{0}, std::byte{7}, std::byte{0xff}};
    const std::array entropy{std::byte{0x31}, std::byte{0x45}};
    const std::array wrong_entropy{std::byte{0x32}, std::byte{0x45}};
    std::string error;
    assert(protect_user_secret({}, entropy, &error).empty());
    assert(error == "invalid secret size");
    assert(unprotect_user_secret({}, entropy, &error).empty());
    assert(error == "invalid protected secret size");

    const auto protected_user = protect_user_secret(source, entropy, &error);
    assert(!protected_user.empty());
    assert(protected_user.size() > source.size());
    assert(unprotect_user_secret(protected_user, entropy) ==
           std::vector<std::byte>(source.begin(), source.end()));
    assert(unprotect_user_secret(protected_user, wrong_entropy, &error).empty());
    assert(error == "CryptUnprotectData failed");
    auto tampered = protected_user;
    tampered.back() ^= std::byte{1};
    assert(unprotect_user_secret(tampered, entropy, &error).empty());
    assert(error == "CryptUnprotectData failed");

    const auto protected_machine = protect_machine_secret(source, entropy);
    assert(!protected_machine.empty());
    assert(unprotect_machine_secret(protected_machine, entropy) ==
           std::vector<std::byte>(source.begin(), source.end()));
    assert(unprotect_machine_secret(protected_machine, wrong_entropy).empty());
}
