#include "nstu/update_manifest.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nstu::update {

std::optional<PackageRole> parse_package_role(std::string_view str) noexcept {
    if (str == "client") return PackageRole::client;
    if (str == "server") return PackageRole::server;
    return std::nullopt;
}

std::optional<ReleaseChannel> parse_release_channel(std::string_view str) noexcept {
    if (str == "ltsc-stable") return ReleaseChannel::ltsc_stable;
    if (str == "security") return ReleaseChannel::security;
    if (str == "pilot") return ReleaseChannel::pilot;
    return std::nullopt;
}

std::string SemanticVersion::to_string() const {
    std::ostringstream ss;
    ss << major << "." << minor << "." << patch;
    return ss.str();
}

std::optional<SemanticVersion> SemanticVersion::parse(std::string_view str) noexcept {
    if (str.empty()) return std::nullopt;

    std::uint32_t parts[3] = {0, 0, 0};
    std::size_t part_idx = 0;
    std::uint64_t current = 0;
    bool has_digit = false;

    for (std::size_t i = 0; i < str.size(); ++i) {
        const char c = str[i];
        if (c >= '0' && c <= '9') {
            const unsigned int digit = static_cast<unsigned int>(c - '0');
            if (current > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
                return std::nullopt; // Overflow
            }
            current = current * 10 + digit;
            has_digit = true;
        } else if (c == '.') {
            if (!has_digit || part_idx >= 2) return std::nullopt;
            parts[part_idx++] = static_cast<std::uint32_t>(current);
            current = 0;
            has_digit = false;
        } else {
            return std::nullopt; // Non-digit
        }
    }

    if (!has_digit || part_idx != 2) return std::nullopt;
    parts[part_idx] = static_cast<std::uint32_t>(current);

    return SemanticVersion{parts[0], parts[1], parts[2]};
}

std::string_view to_string(ManifestVerificationError error) noexcept {
    switch (error) {
    case ManifestVerificationError::ok: return "ok";
    case ManifestVerificationError::invalid_json: return "invalid_json";
    case ManifestVerificationError::unsupported_schema: return "unsupported_schema";
    case ManifestVerificationError::role_mismatch: return "role_mismatch";
    case ManifestVerificationError::downgrade_detected: return "downgrade_detected";
    case ManifestVerificationError::below_minimum_supported_version: return "below_minimum_supported_version";
    case ManifestVerificationError::incompatible_os_build: return "incompatible_os_build";
    case ManifestVerificationError::invalid_digest: return "invalid_digest";
    case ManifestVerificationError::invalid_signature: return "invalid_signature";
    case ManifestVerificationError::rollout_deferred: return "rollout_deferred";
    case ManifestVerificationError::invalid_url: return "invalid_url";
    case ManifestVerificationError::invalid_rollout_percentage: return "invalid_rollout_percentage";
    case ManifestVerificationError::oversized_manifest: return "oversized_manifest";
    case ManifestVerificationError::missing_required_field: return "missing_required_field";
    case ManifestVerificationError::already_up_to_date: return "already_up_to_date";
    }
    return "unknown";
}

namespace {

constexpr std::size_t kMaxManifestBytes = 64 * 1024; // 64 KiB

struct JsonReader {
    std::string_view input;
    std::size_t pos{0};

    void skip_whitespace() noexcept {
        while (pos < input.size()) {
            const char c = input[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool match_char(char expected) noexcept {
        skip_whitespace();
        if (pos < input.size() && input[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool parse_string(std::string& out) {
        skip_whitespace();
        if (pos >= input.size() || input[pos] != '"') return false;
        ++pos;
        out.clear();
        bool escaped = false;
        while (pos < input.size()) {
            const char c = input[pos++];
            if (escaped) {
                switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(c); break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return true;
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_uint64(std::uint64_t& out) noexcept {
        skip_whitespace();
        if (pos >= input.size() || !std::isdigit(static_cast<unsigned char>(input[pos]))) return false;
        std::uint64_t val = 0;
        while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
            const unsigned int digit = static_cast<unsigned int>(input[pos++] - '0');
            if (val > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return false; // Overflow protection
            }
            val = val * 10 + digit;
        }
        out = val;
        return true;
    }

    bool parse_uint32(std::uint32_t& out) noexcept {
        std::uint64_t v = 0;
        if (!parse_uint64(v) || v > 0xFFFFFFFFULL) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool parse_bool(bool& out) noexcept {
        skip_whitespace();
        if (pos + 4 <= input.size() && input.substr(pos, 4) == "true") {
            pos += 4;
            out = true;
            return true;
        }
        if (pos + 5 <= input.size() && input.substr(pos, 5) == "false") {
            pos += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool skip_value() noexcept {
        skip_whitespace();
        if (pos >= input.size()) return false;
        const char c = input[pos];
        if (c == '"') {
            std::string discard;
            return parse_string(discard);
        }
        if (c == '{') {
            ++pos;
            int depth = 1;
            while (pos < input.size() && depth > 0) {
                if (input[pos] == '"') {
                    std::string discard;
                    if (!parse_string(discard)) return false;
                    continue;
                }
                if (input[pos] == '{') ++depth;
                else if (input[pos] == '}') --depth;
                ++pos;
            }
            return depth == 0;
        }
        if (c == '[') {
            ++pos;
            int depth = 1;
            while (pos < input.size() && depth > 0) {
                if (input[pos] == '"') {
                    std::string discard;
                    if (!parse_string(discard)) return false;
                    continue;
                }
                if (input[pos] == '[') ++depth;
                else if (input[pos] == ']') --depth;
                ++pos;
            }
            return depth == 0;
        }
        while (pos < input.size() && input[pos] != ',' && input[pos] != '}' && input[pos] != ']' &&
               input[pos] != ' ' && input[pos] != '\t' && input[pos] != '\r' && input[pos] != '\n') {
            ++pos;
        }
        return true;
    }
};

std::string bytes_to_hex(std::span<const std::byte> bytes) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto b : bytes) {
        ss << std::setw(2) << static_cast<unsigned int>(std::to_integer<unsigned char>(b));
    }
    return ss.str();
}

std::optional<std::vector<std::byte>> hex_to_bytes(std::string_view hex) noexcept {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<std::byte> result;
    result.reserve(hex.size() / 2);

    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const char h1 = hex[i];
        const char h2 = hex[i + 1];
        if (!std::isxdigit(static_cast<unsigned char>(h1)) ||
            !std::isxdigit(static_cast<unsigned char>(h2))) {
            return std::nullopt;
        }
        auto char_val = [](char c) -> unsigned char {
            if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
            return 0;
        };
        const unsigned char byte_val = static_cast<unsigned char>((char_val(h1) << 4) | char_val(h2));
        result.push_back(static_cast<std::byte>(byte_val));
    }
    return result;
}

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace

std::optional<UpdateSigningKeyPair> generate_update_signing_keypair() noexcept {
    try {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
            return std::nullopt;
        }

        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCryptGenerateKeyPair(alg, &key, 256, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }

        if (BCryptFinalizeKeyPair(key, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }

        ULONG pub_len = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pub_len, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }
        std::vector<std::byte> pub_buf(pub_len);
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(pub_buf.data()), pub_len, &pub_len, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }

        ULONG priv_len = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &priv_len, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }
        std::vector<std::byte> priv_buf(priv_len);
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, reinterpret_cast<PUCHAR>(priv_buf.data()), priv_len, &priv_len, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::nullopt;
        }

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);

        return UpdateSigningKeyPair{std::move(pub_buf), std::move(priv_buf)};
    } catch (...) {
        return std::nullopt;
    }
}

std::string build_canonical_manifest_signing_payload(const UpdateManifest& manifest) {
    std::ostringstream ss;
    ss << "schema:" << manifest.schema_version << "\n";
    ss << "role:" << to_string(manifest.package_role) << "\n";
    ss << "version:" << manifest.version.to_string() << "\n";
    ss << "min_supported_version:" << manifest.min_supported_version.to_string() << "\n";
    ss << "package_url:" << manifest.package_url << "\n";
    ss << "byte_length:" << manifest.byte_length << "\n";
    ss << "sha256:" << manifest.sha256_hex << "\n";
    ss << "channel:" << to_string(manifest.channel) << "\n";
    ss << "timestamp:" << manifest.release_timestamp_utc << "\n";
    ss << "rollout_percentage:" << manifest.rollout_percentage << "\n";
    ss << "min_windows_build:" << manifest.min_windows_build << "\n";
    ss << "requires_reboot:" << (manifest.requires_reboot ? "1" : "0") << "\n";
    ss << "key_id:" << manifest.key_id << "\n";
    return ss.str();
}

bool sign_update_manifest(UpdateManifest& manifest,
                          std::span<const std::byte> private_key) noexcept {
    if (private_key.empty()) return false;

    try {
        const std::string payload = build_canonical_manifest_signing_payload(manifest);
        const auto payload_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        const auto digest = nstu::security::sha256(payload_span);
        if (!digest.has_value()) return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }

        std::vector<std::byte> full_blob;
        std::span<const std::byte> blob_to_import = private_key;
        if (private_key.size() == 96) {
            // Raw (X || Y || d), prepend BCRYPT_ECCKEY_BLOB header
            full_blob.resize(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
            auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(full_blob.data());
            header->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
            header->cbKey = 32;
            std::memcpy(full_blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), private_key.data(), 96);
            blob_to_import = full_blob;
        }

        BCRYPT_KEY_HANDLE key = nullptr;
        const NTSTATUS import_status = BCryptImportKeyPair(
            alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(blob_to_import.data())),
            static_cast<ULONG>(blob_to_import.size()), 0);

        if (import_status != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        ULONG sig_len = 0;
        if (BCryptSignHash(key, nullptr,
                           const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(digest->data())),
                           static_cast<ULONG>(digest->size()),
                           nullptr, 0, &sig_len, 0) != 0 || sig_len == 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        std::vector<std::byte> sig(sig_len);
        if (BCryptSignHash(key, nullptr,
                           const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(digest->data())),
                           static_cast<ULONG>(digest->size()),
                           reinterpret_cast<PUCHAR>(sig.data()), sig_len, &sig_len, 0) != 0) {
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);

        manifest.signature_hex = bytes_to_hex(sig);
        return true;
    } catch (...) {
        return false;
    }
}

bool verify_manifest_signature(const UpdateManifest& manifest,
                               std::span<const std::byte> public_key) noexcept {
    if (public_key.empty() || manifest.signature_hex.empty()) return false;

    try {
        const auto sig_bytes = hex_to_bytes(manifest.signature_hex);
        if (!sig_bytes.has_value() || sig_bytes->size() != 64) {
            return false; // ECDSA P-256 signature must be exactly 64 bytes (r: 32, s: 32)
        }

        const std::string payload = build_canonical_manifest_signing_payload(manifest);
        const auto payload_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        const auto digest = nstu::security::sha256(payload_span);
        if (!digest.has_value()) return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }

        std::vector<std::byte> full_blob;
        std::span<const std::byte> blob_to_import = public_key;
        if (public_key.size() == 64) {
            // Raw (X || Y), prepend BCRYPT_ECCKEY_BLOB header
            full_blob.resize(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
            auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(full_blob.data());
            header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
            header->cbKey = 32;
            std::memcpy(full_blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), public_key.data(), 64);
            blob_to_import = full_blob;
        }

        BCRYPT_KEY_HANDLE key = nullptr;
        const NTSTATUS import_status = BCryptImportKeyPair(
            alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(blob_to_import.data())),
            static_cast<ULONG>(blob_to_import.size()), 0);

        if (import_status != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        const NTSTATUS verify_status = BCryptVerifySignature(
            key, nullptr,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(digest->data())),
            static_cast<ULONG>(digest->size()),
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(sig_bytes->data())),
            static_cast<ULONG>(sig_bytes->size()), 0);

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);

        return verify_status == 0;
    } catch (...) {
        return false;
    }
}

std::optional<std::uint32_t> compute_canary_bucket(std::string_view machine_guid) noexcept {
    if (machine_guid.empty()) return std::nullopt;

    try {
        const auto span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(machine_guid.data()), machine_guid.size());
        const auto digest = nstu::security::sha256(span);
        if (!digest.has_value()) return std::nullopt;

        const auto& d = *digest;
        const std::uint32_t val = (static_cast<std::uint32_t>(d[0]) << 24) |
                                  (static_cast<std::uint32_t>(d[1]) << 16) |
                                  (static_cast<std::uint32_t>(d[2]) << 8) |
                                  static_cast<std::uint32_t>(d[3]);
        return val % 100u;
    } catch (...) {
        return std::nullopt;
    }
}

ManifestVerificationResult verify_manifest_against_environment(
    const UpdateManifest& manifest,
    const ClientEnvironment& env,
    std::span<const std::byte> public_key) noexcept {
    ManifestVerificationResult result;

    try {
        // 1. Verify digital signature using public trust root
        if (!verify_manifest_signature(manifest, public_key)) {
            result.error = ManifestVerificationError::invalid_signature;
            result.error_message = "Manifest signature is invalid or does not match public key";
            return result;
        }

        // 2. Package role match
        if (manifest.package_role != env.installed_role) {
            result.error = ManifestVerificationError::role_mismatch;
            result.error_message = "Package role does not match installed role";
            return result;
        }

        // 3. Anti-downgrade and equal-version check
        if (manifest.version < env.current_version) {
            result.error = ManifestVerificationError::downgrade_detected;
            result.error_message = "Candidate version is lower than installed version (downgrade rejected)";
            return result;
        }
        if (manifest.version == env.current_version && !env.allow_same_version) {
            result.error = ManifestVerificationError::already_up_to_date;
            result.error_message = "Candidate version equals installed version (already up to date, reinstallation rejected)";
            return result;
        }

        // 4. Minimum supported version upgrade path
        if (env.current_version < manifest.min_supported_version) {
            result.error = ManifestVerificationError::below_minimum_supported_version;
            result.error_message = "Installed version is below candidate minimum supported version";
            return result;
        }

        // 5. Operating system build compatibility
        if (env.current_windows_build < manifest.min_windows_build) {
            result.error = ManifestVerificationError::incompatible_os_build;
            result.error_message = "Current Windows build is below candidate minimum requirement";
            return result;
        }

        // 6. Rollout percentage bounds
        if (manifest.rollout_percentage > 100) {
            result.error = ManifestVerificationError::invalid_rollout_percentage;
            result.error_message = "Rollout percentage must be in [0, 100]";
            return result;
        }

        // 7. Canary bucket rollout gating
        if (manifest.rollout_percentage < 100) {
            const auto bucket = compute_canary_bucket(env.machine_guid);
            if (!bucket.has_value()) {
                result.error = ManifestVerificationError::rollout_deferred;
                result.error_message = "Client machine GUID is missing or invalid for canary bucket determination";
                return result;
            }
            if (*bucket >= manifest.rollout_percentage) {
                result.error = ManifestVerificationError::rollout_deferred;
                result.error_message = "Client canary bucket (" + std::to_string(*bucket) +
                                       ") is above release rollout percentage (" +
                                       std::to_string(manifest.rollout_percentage) + "%)";
                return result;
            }
        }

        // 8. Payload SHA-256 validation (must be 64 valid hex chars)
        if (manifest.sha256_hex.size() != 64) {
            result.error = ManifestVerificationError::invalid_digest;
            result.error_message = "SHA-256 digest hex length must be exactly 64";
            return result;
        }
        for (const char c : manifest.sha256_hex) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                result.error = ManifestVerificationError::invalid_digest;
                result.error_message = "SHA-256 digest contains non-hexadecimal character";
                return result;
            }
        }

        // 9. Package URL validation (mandates HTTPS by default)
        if (!manifest.package_url.starts_with("https://") &&
            (!env.allow_insecure_http || !manifest.package_url.starts_with("http://"))) {
            result.error = ManifestVerificationError::invalid_url;
            result.error_message = "Package URL must use secure https:// transport";
            return result;
        }

        result.error = ManifestVerificationError::ok;
        return result;
    } catch (...) {
        result.error = ManifestVerificationError::invalid_signature;
        result.error_message = "Unexpected exception during verification";
        return result;
    }
}

std::string export_update_manifest_json(const UpdateManifest& manifest) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema_version\": \"" << escape_json(manifest.schema_version) << "\",\n";
    ss << "  \"package_role\": \"" << to_string(manifest.package_role) << "\",\n";
    ss << "  \"version\": \"" << manifest.version.to_string() << "\",\n";
    ss << "  \"min_supported_version\": \"" << manifest.min_supported_version.to_string() << "\",\n";
    ss << "  \"package_url\": \"" << escape_json(manifest.package_url) << "\",\n";
    ss << "  \"byte_length\": " << manifest.byte_length << ",\n";
    ss << "  \"sha256_hex\": \"" << escape_json(manifest.sha256_hex) << "\",\n";
    ss << "  \"channel\": \"" << to_string(manifest.channel) << "\",\n";
    ss << "  \"release_timestamp_utc\": " << manifest.release_timestamp_utc << ",\n";
    ss << "  \"rollout_percentage\": " << manifest.rollout_percentage << ",\n";
    ss << "  \"min_windows_build\": " << manifest.min_windows_build << ",\n";
    ss << "  \"requires_reboot\": " << (manifest.requires_reboot ? "true" : "false") << ",\n";
    ss << "  \"key_id\": " << manifest.key_id << ",\n";
    ss << "  \"signature_hex\": \"" << escape_json(manifest.signature_hex) << "\"\n";
    ss << "}\n";
    return ss.str();
}

std::pair<std::optional<UpdateManifest>, ManifestVerificationResult>
import_update_manifest_json(std::string_view json_str) {
    ManifestVerificationResult err;
    if (json_str.size() > kMaxManifestBytes) {
        err.error = ManifestVerificationError::oversized_manifest;
        err.error_message = "Manifest size exceeds maximum limit of 64 KiB";
        return {std::nullopt, err};
    }

    JsonReader reader{json_str, 0};
    if (!reader.match_char('{')) {
        err.error = ManifestVerificationError::invalid_json;
        err.error_message = "Expected '{' at start of JSON manifest";
        return {std::nullopt, err};
    }

    UpdateManifest manifest;
    bool has_role = false;
    bool has_version = false;
    bool has_url = false;
    bool has_sha256 = false;
    bool has_byte_length = false;
    bool has_channel = false;
    bool has_min_windows_build = false;
    bool has_release_timestamp_utc = false;

    while (true) {
        reader.skip_whitespace();
        if (reader.match_char('}')) {
            break;
        }

        std::string key;
        if (!reader.parse_string(key) || !reader.match_char(':')) {
            err.error = ManifestVerificationError::invalid_json;
            err.error_message = "Failed to parse key";
            return {std::nullopt, err};
        }

        if (key == "schema_version") {
            std::string ver;
            if (!reader.parse_string(ver)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            if (ver != "1.0") {
                err.error = ManifestVerificationError::unsupported_schema;
                err.error_message = "Unsupported schema_version: " + ver;
                return {std::nullopt, err};
            }
            manifest.schema_version = ver;
        } else if (key == "package_role") {
            std::string r;
            if (!reader.parse_string(r)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            const auto parsed = parse_package_role(r);
            if (!parsed) {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Invalid package_role: " + r;
                return {std::nullopt, err};
            }
            manifest.package_role = *parsed;
            has_role = true;
        } else if (key == "version") {
            std::string v;
            if (!reader.parse_string(v)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            const auto parsed = SemanticVersion::parse(v);
            if (!parsed) {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Invalid version format: " + v;
                return {std::nullopt, err};
            }
            manifest.version = *parsed;
            has_version = true;
        } else if (key == "min_supported_version") {
            std::string v;
            if (!reader.parse_string(v)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            const auto parsed = SemanticVersion::parse(v);
            if (!parsed) {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Invalid version format: " + v;
                return {std::nullopt, err};
            }
            manifest.min_supported_version = *parsed;
        } else if (key == "package_url") {
            if (!reader.parse_string(manifest.package_url)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            has_url = true;
        } else if (key == "byte_length") {
            if (!reader.parse_uint64(manifest.byte_length)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            has_byte_length = true;
        } else if (key == "sha256_hex") {
            if (!reader.parse_string(manifest.sha256_hex)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            has_sha256 = true;
        } else if (key == "channel") {
            std::string ch;
            if (!reader.parse_string(ch)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            const auto parsed = parse_release_channel(ch);
            if (!parsed) {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Invalid channel: " + ch;
                return {std::nullopt, err};
            }
            manifest.channel = *parsed;
            has_channel = true;
        } else if (key == "release_timestamp_utc") {
            if (!reader.parse_uint64(manifest.release_timestamp_utc)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            has_release_timestamp_utc = true;
        } else if (key == "rollout_percentage") {
            if (!reader.parse_uint32(manifest.rollout_percentage)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
        } else if (key == "min_windows_build") {
            if (!reader.parse_uint32(manifest.min_windows_build)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
            has_min_windows_build = true;
        } else if (key == "requires_reboot") {
            if (!reader.parse_bool(manifest.requires_reboot)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
        } else if (key == "key_id") {
            if (!reader.parse_uint32(manifest.key_id)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
        } else if (key == "signature_hex") {
            if (!reader.parse_string(manifest.signature_hex)) {
                err.error = ManifestVerificationError::invalid_json;
                return {std::nullopt, err};
            }
        } else {
            if (!reader.skip_value()) {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Malformed value for unknown key";
                return {std::nullopt, err};
            }
        }

        reader.skip_whitespace();
        if (reader.match_char(',')) {
            reader.skip_whitespace();
            if (reader.pos < reader.input.size() && reader.input[reader.pos] == '}') {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Trailing comma not allowed in manifest JSON";
                return {std::nullopt, err};
            }
        } else {
            reader.skip_whitespace();
            if (reader.pos < reader.input.size() && reader.input[reader.pos] != '}') {
                err.error = ManifestVerificationError::invalid_json;
                err.error_message = "Missing comma separator between object entries";
                return {std::nullopt, err};
            }
        }
    }

    // Trailing content check
    reader.skip_whitespace();
    if (reader.pos != json_str.size()) {
        err.error = ManifestVerificationError::invalid_json;
        err.error_message = "Unexpected trailing data after closing JSON object";
        return {std::nullopt, err};
    }

    if (!has_role || !has_version || !has_url || !has_sha256 ||
        !has_byte_length || !has_channel || !has_min_windows_build || !has_release_timestamp_utc) {
        err.error = ManifestVerificationError::missing_required_field;
        err.error_message = "Missing required manifest field";
        return {std::nullopt, err};
    }

    err.error = ManifestVerificationError::ok;
    return {manifest, err};
}

} // namespace nstu::update
