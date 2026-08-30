#include "nstu/enrollment.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::security {
namespace {

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

std::vector<std::byte> enrollment_message(const char* label,
                                           const EnrollmentRequest& request) {
    const auto label_bytes = std::strlen(label);
    std::vector<std::byte> message{
        reinterpret_cast<const std::byte*>(label),
        reinterpret_cast<const std::byte*>(label) + label_bytes};
    message.insert(message.end(), request.client_id.begin(),
                   request.client_id.end());
    append_le(message, request.key_id);
    message.insert(message.end(), request.nonce.begin(), request.nonce.end());
    append_le(message, request.unix_time_seconds);
    return message;
}

bool valid_request_fields(const EnrollmentRequest& request) noexcept {
    return request.key_id != 0 && request.unix_time_seconds != 0 &&
        std::any_of(request.client_id.begin(), request.client_id.end(),
                    [](std::byte value) { return value != std::byte{0}; }) &&
        std::any_of(request.nonce.begin(), request.nonce.end(),
                    [](std::byte value) { return value != std::byte{0}; });
}

} // namespace

std::optional<EnrollmentRequest> create_enrollment_request(
    const ClientId& client_id, std::uint32_t key_id,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> enrollment_secret) noexcept {
    if (enrollment_secret.size() < kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    EnrollmentRequest request;
    request.client_id = client_id;
    request.key_id = key_id;
    request.unix_time_seconds = unix_time_seconds;
    if (!generate_random(request.nonce) || !valid_request_fields(request)) {
        return std::nullopt;
    }
    const auto message = enrollment_message("NSTU-ENROLLMENT-AUTH-V1", request);
    const auto authenticator = hmac_sha256(enrollment_secret, message);
    if (!authenticator) {
        return std::nullopt;
    }
    request.authenticator = *authenticator;
    return request;
}

std::vector<std::byte> encode_enrollment_request(
    const EnrollmentRequest& request) {
    if (!valid_request_fields(request)) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kEnrollmentRequestBytes);
    wire.insert(wire.end(), request.client_id.begin(), request.client_id.end());
    append_le(wire, request.key_id);
    wire.insert(wire.end(), request.nonce.begin(), request.nonce.end());
    append_le(wire, request.unix_time_seconds);
    wire.insert(wire.end(), request.authenticator.begin(),
                request.authenticator.end());
    return wire;
}

std::optional<EnrollmentRequest> decode_enrollment_request(
    std::span<const std::byte> wire) {
    if (wire.size() != kEnrollmentRequestBytes) {
        return std::nullopt;
    }
    EnrollmentRequest request;
    std::size_t offset = 0;
    std::copy_n(wire.begin(), request.client_id.size(),
                request.client_id.begin());
    offset += request.client_id.size();
    if (!read_le(wire, offset, request.key_id)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                request.nonce.size(), request.nonce.begin());
    offset += request.nonce.size();
    if (!read_le(wire, offset, request.unix_time_seconds)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                request.authenticator.size(), request.authenticator.begin());
    return valid_request_fields(request) ? std::optional{request}
                                         : std::nullopt;
}

std::optional<Sha256Digest> derive_enrolled_key(
    std::span<const std::byte> enrollment_secret,
    const EnrollmentRequest& request) noexcept {
    if (enrollment_secret.size() < kMinimumProtocolKeyBytes ||
        !valid_request_fields(request)) {
        return std::nullopt;
    }
    const auto message = enrollment_message("NSTU-ENROLLED-KEY-V1", request);
    return hmac_sha256(enrollment_secret, message);
}

EnrollmentAuthority::EnrollmentAuthority(
    std::span<const std::byte> enrollment_secret,
    std::size_t replay_capacity,
    std::chrono::seconds maximum_clock_skew)
    : secret_(enrollment_secret.begin(), enrollment_secret.end()),
      replay_protector_(replay_capacity, maximum_clock_skew),
      maximum_clock_skew_(std::max(maximum_clock_skew,
                                   std::chrono::seconds(1))) {}

EnrollmentAuthority::~EnrollmentAuthority() {
    secure_zero(secret_);
}

bool EnrollmentAuthority::accept(const EnrollmentRequest& request,
                                 std::uint64_t now_unix_seconds,
                                 KeyStore& key_store,
                                 std::string* error) {
    if (secret_.size() < kMinimumProtocolKeyBytes ||
        !valid_request_fields(request)) {
        set_error(error, "invalid enrollment request");
        return false;
    }
    const auto skew = static_cast<std::uint64_t>(maximum_clock_skew_.count());
    const auto difference = request.unix_time_seconds > now_unix_seconds
        ? request.unix_time_seconds - now_unix_seconds
        : now_unix_seconds - request.unix_time_seconds;
    if (difference > skew) {
        set_error(error, "enrollment request is outside the clock window");
        return false;
    }
    const auto auth_message = enrollment_message(
        "NSTU-ENROLLMENT-AUTH-V1", request);
    const auto expected = hmac_sha256(secret_, auth_message);
    if (!expected || !constant_time_equal(*expected, request.authenticator)) {
        set_error(error, "enrollment authentication failed");
        return false;
    }
    AuthHello replay_identity;
    replay_identity.client_id = request.client_id;
    replay_identity.client_nonce = request.nonce;
    replay_identity.unix_time_seconds = request.unix_time_seconds;
    replay_identity.key_id = request.key_id;
    if (!replay_protector_.accept(replay_identity, now_unix_seconds)) {
        set_error(error, "enrollment request was replayed");
        return false;
    }
    auto key = derive_enrolled_key(secret_, request);
    if (!key) {
        set_error(error, "enrollment key derivation failed");
        return false;
    }
    const bool enrolled = key_store.enroll(request.client_id, request.key_id,
                                           *key, error);
    secure_zero(*key);
    return enrolled;
}

} // namespace nstu::security
