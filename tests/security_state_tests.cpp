#include "nstu/enrollment.hpp"
#include "nstu/keyring.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <filesystem>
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

std::vector<std::byte> make_secret(std::byte seed) {
    std::vector<std::byte> secret(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<std::byte>(
            std::to_integer<unsigned int>(seed) + index);
    }
    return secret;
}

} // namespace

int main() {
    std::string error;
    const auto first_id = make_id(std::byte{1});
    const auto second_id = make_id(std::byte{33});
    auto first_key = make_secret(std::byte{0x40});
    auto second_key = make_secret(std::byte{0x70});

    nstu::security::KeyStore source;
    assert(source.enroll(first_id, 7, first_key, &error));
    assert(source.enroll(second_id, 3, second_key, &error));
    assert(source.revoke(second_id, 3, &error));
    auto records = source.snapshot();
    assert(records.size() == 2);
    const auto wire = nstu::security::encode_keyring(records, &error);
    assert(!wire.empty());
    const auto decoded = nstu::security::decode_keyring(wire, &error);
    assert(decoded.has_value() && decoded->size() == 2);

    auto corrupt = wire;
    corrupt.push_back(std::byte{0xff});
    assert(!nstu::security::decode_keyring(corrupt, &error).has_value());

    const std::array<std::byte, 8> entropy{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto path = std::filesystem::path(temporary_directory) /
        (L"nstu-keyring-test-" + std::to_wstring(GetCurrentProcessId()) +
         L".bin");
    assert(nstu::security::save_keyring(source, path.wstring(), entropy,
                                        &error));
    nstu::security::KeyStore restored;
    assert(nstu::security::load_keyring(restored, path.wstring(), entropy,
                                        &error));
    assert(restored.resolve(first_id, 7) == first_key);
    assert(!restored.resolve(second_id, 3).has_value());
    assert(restored.key_count() == 2);
    assert(DeleteFileW(path.c_str()));

    auto enrollment_secret = make_secret(std::byte{0x11});
    const auto request = nstu::security::create_enrollment_request(
        second_id, 9, 50'000, enrollment_secret);
    assert(request.has_value());
    const auto request_wire =
        nstu::security::encode_enrollment_request(*request);
    assert(request_wire.size() == nstu::security::kEnrollmentRequestBytes);
    const auto parsed =
        nstu::security::decode_enrollment_request(request_wire);
    assert(parsed.has_value());
    const auto client_key = nstu::security::derive_enrolled_key(
        enrollment_secret, *parsed);
    assert(client_key.has_value());

    nstu::security::EnrollmentAuthority authority(enrollment_secret);
    nstu::security::KeyStore enrolled;
    assert(authority.accept(*parsed, 50'001, enrolled, &error));
    assert(enrolled.resolve(second_id, 9) ==
           std::vector<std::byte>(client_key->begin(), client_key->end()));
    assert(!authority.accept(*parsed, 50'001, enrolled, &error));

    auto tampered = *parsed;
    tampered.authenticator[0] ^= std::byte{1};
    assert(!authority.accept(tampered, 50'001, enrolled, &error));
    assert(!authority.accept(*request, 51'000, enrolled, &error));

    nstu::security::secure_zero(first_key);
    nstu::security::secure_zero(second_key);
    nstu::security::secure_zero(enrollment_secret);
    return 0;
}
