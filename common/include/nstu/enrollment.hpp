#pragma once

#include "nstu/auth.hpp"
#include "nstu/key_store.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::security {

inline constexpr std::size_t kEnrollmentRequestBytes =
    kClientIdBytes + sizeof(std::uint32_t) + kNonceBytes +
    sizeof(std::uint64_t) + kSha256Bytes;

struct EnrollmentRequest {
    ClientId client_id{};
    std::uint32_t key_id = 0;
    Nonce nonce{};
    std::uint64_t unix_time_seconds = 0;
    Sha256Digest authenticator{};
};

[[nodiscard]] std::optional<EnrollmentRequest> create_enrollment_request(
    const ClientId& client_id, std::uint32_t key_id,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> enrollment_secret) noexcept;

[[nodiscard]] std::vector<std::byte> encode_enrollment_request(
    const EnrollmentRequest& request);

[[nodiscard]] std::optional<EnrollmentRequest> decode_enrollment_request(
    std::span<const std::byte> wire);

[[nodiscard]] std::optional<Sha256Digest> derive_enrolled_key(
    std::span<const std::byte> enrollment_secret,
    const EnrollmentRequest& request) noexcept;

class EnrollmentAuthority {
public:
    explicit EnrollmentAuthority(
        std::span<const std::byte> enrollment_secret,
        std::size_t replay_capacity = 4096,
        std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120));
    ~EnrollmentAuthority();
    EnrollmentAuthority(const EnrollmentAuthority&) = delete;
    EnrollmentAuthority& operator=(const EnrollmentAuthority&) = delete;

    [[nodiscard]] bool accept(const EnrollmentRequest& request,
                              std::uint64_t now_unix_seconds,
                              KeyStore& key_store,
                              std::string* error = nullptr);

private:
    std::vector<std::byte> secret_;
    ReplayProtector replay_protector_;
    std::chrono::seconds maximum_clock_skew_;
};

} // namespace nstu::security
