#pragma once

#include "nstu/auth.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nstu::update {

enum class PackageRole : int {
    client = 1,
    server = 2
};

[[nodiscard]] constexpr std::string_view to_string(PackageRole role) noexcept {
    switch (role) {
    case PackageRole::client: return "client";
    case PackageRole::server: return "server";
    }
    return "unknown";
}

[[nodiscard]] std::optional<PackageRole> parse_package_role(std::string_view str) noexcept;

enum class ReleaseChannel : int {
    ltsc_stable = 1,
    security = 2,
    pilot = 3
};

[[nodiscard]] constexpr std::string_view to_string(ReleaseChannel channel) noexcept {
    switch (channel) {
    case ReleaseChannel::ltsc_stable: return "ltsc-stable";
    case ReleaseChannel::security: return "security";
    case ReleaseChannel::pilot: return "pilot";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ReleaseChannel> parse_release_channel(std::string_view str) noexcept;

struct SemanticVersion {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    auto operator<=>(const SemanticVersion&) const = default;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] static std::optional<SemanticVersion> parse(std::string_view str) noexcept;
};

struct UpdateManifest {
    std::string schema_version{"1.0"};
    PackageRole package_role{PackageRole::client};
    SemanticVersion version{0, 0, 0};
    SemanticVersion min_supported_version{0, 0, 0};
    std::string package_url;
    std::uint64_t byte_length{0};
    std::string sha256_hex;
    ReleaseChannel channel{ReleaseChannel::ltsc_stable};
    std::uint64_t release_timestamp_utc{0};
    std::uint32_t rollout_percentage{100};       // Bounds: [0, 100]
    std::uint32_t min_windows_build{17763};     // Minimum Windows build (e.g. 17763 for LTSC 2019)
    bool requires_reboot{true};
    std::uint32_t key_id{1};                    // Signing key rotation ID
    std::string signature_hex;                  // Hex-encoded asymmetric ECDSA P-256 signature (64 bytes / 128 hex chars)
};

struct ClientEnvironment {
    PackageRole installed_role{PackageRole::client};
    SemanticVersion current_version{0, 0, 0};
    std::uint32_t current_windows_build{19045}; // e.g. 19045 for Windows 10 22H2
    std::string machine_guid;                  // Stable client identity used for canary bucket hashing
    bool allow_same_version{false};            // When false, candidate == current version is rejected as already_up_to_date
    bool allow_insecure_http{false};           // When false, package_url must strictly start with https://
};

enum class ManifestVerificationError : int {
    ok = 0,
    invalid_json = 1,
    unsupported_schema = 2,
    role_mismatch = 3,
    downgrade_detected = 4,
    below_minimum_supported_version = 5,
    incompatible_os_build = 6,
    invalid_digest = 7,
    invalid_signature = 8,
    rollout_deferred = 9,
    invalid_url = 10,
    invalid_rollout_percentage = 11,
    oversized_manifest = 12,
    missing_required_field = 13,
    already_up_to_date = 14
};

[[nodiscard]] std::string_view to_string(ManifestVerificationError error) noexcept;

struct ManifestVerificationResult {
    ManifestVerificationError error{ManifestVerificationError::ok};
    std::string error_message;

    [[nodiscard]] bool ok() const noexcept {
        return error == ManifestVerificationError::ok;
    }
};

struct UpdateSigningKeyPair {
    std::vector<std::byte> public_key;  // BCRYPT_ECCPUBLIC_BLOB (72 bytes) or raw 64 bytes
    std::vector<std::byte> private_key; // BCRYPT_ECCPRIVATE_BLOB (104 bytes) or raw 96 bytes
};

// Generates an asymmetric ECDSA P-256 signing key pair using Windows BCrypt.
[[nodiscard]] std::optional<UpdateSigningKeyPair> generate_update_signing_keypair() noexcept;

// Builds canonical deterministic signing payload from manifest properties.
[[nodiscard]] std::string build_canonical_manifest_signing_payload(const UpdateManifest& manifest);

// Signs the manifest in-place using asymmetric ECDSA P-256 (SHA-256 digest) and populates manifest.signature_hex.
[[nodiscard]] bool sign_update_manifest(UpdateManifest& manifest,
                                        std::span<const std::byte> private_key) noexcept;

// Verifies manifest signature using public key (trust root) against the canonical signing payload.
[[nodiscard]] bool verify_manifest_signature(const UpdateManifest& manifest,
                                             std::span<const std::byte> public_key) noexcept;

// Calculates deterministic canary bucket [0, 99] for a client machine GUID.
// Returns std::nullopt if machine_guid is empty.
[[nodiscard]] std::optional<std::uint32_t> compute_canary_bucket(std::string_view machine_guid) noexcept;

// Alias for compute_canary_bucket.
[[nodiscard]] inline std::optional<std::uint32_t> compute_canary_rollout_bucket(std::string_view machine_guid) noexcept {
    return compute_canary_bucket(machine_guid);
}

// Comprehensive client environment verification: signature, role, anti-downgrade, OS build, and canary rollout.
[[nodiscard]] ManifestVerificationResult verify_manifest_against_environment(
    const UpdateManifest& manifest,
    const ClientEnvironment& env,
    std::span<const std::byte> public_key) noexcept;

// Export manifest to clean canonical JSON format.
[[nodiscard]] std::string export_update_manifest_json(const UpdateManifest& manifest);

// Parse and validate manifest from JSON string (bounded <= 64 KiB, fail-closed).
[[nodiscard]] std::pair<std::optional<UpdateManifest>, ManifestVerificationResult>
import_update_manifest_json(std::string_view json_str);

} // namespace nstu::update
