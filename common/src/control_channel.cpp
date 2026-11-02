#include "nstu/control_channel.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#ifndef BCRYPT_CHAIN_MODE_GCM
#define BCRYPT_CHAIN_MODE_GCM L"ChainingModeGCM"
#endif
#ifndef BCRYPT_AES_ALGORITHM
#define BCRYPT_AES_ALGORITHM L"AES"
#endif
#ifndef BCRYPT_CHAINING_MODE
#define BCRYPT_CHAINING_MODE L"ChainingMode"
#endif

namespace nstu::control {
namespace {

inline constexpr std::size_t kAuthenticatedOverhead =
    sizeof(std::uint64_t) + security::kControlAuthTagBytes;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

void write_u64_le(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

std::uint64_t read_u64_le(std::span<const std::byte> input) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(input[index]))
                 << (index * 8u);
    }
    return value;
}

bool encrypt_aead_gcm(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> nonce,
    std::span<const std::byte> associated_data,
    std::span<const std::byte> plaintext,
    std::span<std::byte> ciphertext_out,
    std::span<std::byte, kAeadAuthTagBytes> tag_out,
    std::string* error) {
    if (key.size() != kAeadKeyBytes || ciphertext_out.size() != plaintext.size()) {
        set_error(error, "invalid AEAD key or buffer size");
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        set_error(error, "failed to open AES algorithm provider");
        return false;
    }
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to set GCM chaining mode");
        return false;
    }
    DWORD object_bytes = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes),
                          sizeof(object_bytes), &copied, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to query key object size");
        return false;
    }
    std::vector<UCHAR> key_object(object_bytes);
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptGenerateSymmetricKey(
            algorithm, &key_handle, key_object.data(), object_bytes,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
            static_cast<ULONG>(key.size()), 0) != 0) {
        security::secure_zero({reinterpret_cast<std::byte*>(key_object.data()), key_object.size()});
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to generate symmetric key");
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(nonce.data()));
    auth_info.cbNonce = static_cast<ULONG>(nonce.size());
    if (!associated_data.empty()) {
        auth_info.pbAuthData = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(associated_data.data()));
        auth_info.cbAuthData = static_cast<ULONG>(associated_data.size());
    }
    auth_info.pbTag = reinterpret_cast<PUCHAR>(tag_out.data());
    auth_info.cbTag = static_cast<ULONG>(tag_out.size());

    PUCHAR in_buf = plaintext.empty() ? nullptr : const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(plaintext.data()));
    ULONG in_len = static_cast<ULONG>(plaintext.size());
    PUCHAR out_buf = ciphertext_out.empty() ? nullptr : reinterpret_cast<PUCHAR>(ciphertext_out.data());
    ULONG out_len = static_cast<ULONG>(ciphertext_out.size());
    ULONG result_bytes = 0;

    const NTSTATUS encrypt_status = BCryptEncrypt(
        key_handle, in_buf, in_len, &auth_info, nullptr, 0,
        out_buf, out_len, &result_bytes, 0);

    BCryptDestroyKey(key_handle);
    security::secure_zero({reinterpret_cast<std::byte*>(key_object.data()), key_object.size()});
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (encrypt_status != 0 || result_bytes != plaintext.size()) {
        set_error(error, "BCryptEncrypt GCM failed");
        return false;
    }
    return true;
}

bool decrypt_aead_gcm(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> nonce,
    std::span<const std::byte> associated_data,
    std::span<const std::byte> ciphertext,
    std::span<const std::byte, kAeadAuthTagBytes> tag,
    std::span<std::byte> plaintext_out,
    std::string* error) {
    if (key.size() != kAeadKeyBytes || plaintext_out.size() != ciphertext.size()) {
        set_error(error, "invalid AEAD key or buffer size");
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        set_error(error, "failed to open AES algorithm provider");
        return false;
    }
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to set GCM chaining mode");
        return false;
    }
    DWORD object_bytes = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes),
                          sizeof(object_bytes), &copied, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to query key object size");
        return false;
    }
    std::vector<UCHAR> key_object(object_bytes);
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptGenerateSymmetricKey(
            algorithm, &key_handle, key_object.data(), object_bytes,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
            static_cast<ULONG>(key.size()), 0) != 0) {
        security::secure_zero({reinterpret_cast<std::byte*>(key_object.data()), key_object.size()});
        BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "failed to generate symmetric key");
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(nonce.data()));
    auth_info.cbNonce = static_cast<ULONG>(nonce.size());
    if (!associated_data.empty()) {
        auth_info.pbAuthData = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(associated_data.data()));
        auth_info.cbAuthData = static_cast<ULONG>(associated_data.size());
    }
    auth_info.pbTag = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(tag.data()));
    auth_info.cbTag = static_cast<ULONG>(tag.size());

    PUCHAR in_buf = ciphertext.empty() ? nullptr : const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(ciphertext.data()));
    ULONG in_len = static_cast<ULONG>(ciphertext.size());
    PUCHAR out_buf = plaintext_out.empty() ? nullptr : reinterpret_cast<PUCHAR>(plaintext_out.data());
    ULONG out_len = static_cast<ULONG>(plaintext_out.size());
    ULONG result_bytes = 0;

    const NTSTATUS decrypt_status = BCryptDecrypt(
        key_handle, in_buf, in_len, &auth_info, nullptr, 0,
        out_buf, out_len, &result_bytes, 0);

    BCryptDestroyKey(key_handle);
    security::secure_zero({reinterpret_cast<std::byte*>(key_object.data()), key_object.size()});
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (decrypt_status != 0 || result_bytes != ciphertext.size()) {
        set_error(error, "authenticated command AEAD decryption failed");
        return false;
    }
    return true;
}

std::vector<std::byte> make_aead_aad(const protocol::CommandEnvelope& envelope,
                                     std::uint64_t sequence) {
    auto header = protocol::encode_command_header(envelope);
    header.resize(protocol::kCommandHeaderBytes + sizeof(sequence));
    write_u64_le(std::span<std::byte>(header).subspan(protocol::kCommandHeaderBytes),
                 sequence);
    return header;
}

bool receive_exact(net::TcpSocket& socket, std::span<std::byte> output,
                   std::string* error) {
    std::size_t received = 0;
    while (received < output.size()) {
        const int result = socket.receive(output.subspan(received), error);
        if (result <= 0) {
            if (result == 0) {
                set_error(error, "peer closed the TCP connection");
            }
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return true;
}

bool send_preamble(net::TcpSocket& socket, protocol::ConnectionRole role,
                   const security::ClientId& identity, std::uint32_t key_id,
                   std::uint8_t flags,
                   std::string* error) {
    protocol::ConnectionPreamble preamble;
    preamble.role = role;
    preamble.flags = flags;
    preamble.key_id = key_id;
    preamble.identity = identity;
    if (role == protocol::ConnectionRole::server) {
        preamble.key_id = 0;
        preamble.identity.fill(std::byte{0});
    }
    const auto wire = protocol::encode_connection_preamble(preamble);
    return !wire.empty() &&
           socket.send_all(wire, error) == static_cast<int>(wire.size());
}

std::optional<protocol::ConnectionPreamble> receive_preamble(
    net::TcpSocket& socket, std::string* error) {
    std::array<std::byte, protocol::kConnectionPreambleBytes> wire{};
    if (!receive_exact(socket, wire, error)) {
        return std::nullopt;
    }
    auto preamble = protocol::decode_connection_preamble(wire);
    if (!preamble) {
        set_error(error, "invalid connection preamble");
    }
    return preamble;
}

std::optional<protocol::TcpFrame> receive_plain_frame(net::TcpSocket& socket,
                                                      std::string* error) {
    std::array<std::byte, sizeof(std::uint32_t)> prefix{};
    if (!receive_exact(socket, prefix, error)) {
        return std::nullopt;
    }
    std::uint32_t body_bytes = 0;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        body_bytes |= static_cast<std::uint32_t>(
                          std::to_integer<unsigned int>(prefix[index]))
                      << (index * 8u);
    }
    if (body_bytes < protocol::kCommandHeaderBytes ||
        body_bytes > protocol::kCommandHeaderBytes +
                         protocol::kMaxCommandPayload) {
        set_error(error, "invalid TCP frame size");
        return std::nullopt;
    }
    std::vector<std::byte> body(body_bytes);
    if (!receive_exact(socket, body, error)) {
        return std::nullopt;
    }
    const auto envelope = protocol::decode_command_header(body);
    if (!envelope || envelope->payload_bytes !=
                         body.size() - protocol::kCommandHeaderBytes) {
        set_error(error, "invalid TCP command envelope");
        return std::nullopt;
    }
    protocol::TcpFrame frame{*envelope, {}};
    const auto payload = std::span<const std::byte>(body).subspan(
        protocol::kCommandHeaderBytes);
    frame.payload.assign(payload.begin(), payload.end());
    return frame;
}

bool send_plain_frame(net::TcpSocket& socket, protocol::CommandType type,
                      std::uint64_t request_id,
                      std::span<const std::byte> payload,
                      std::string* error) {
    if (payload.size() > protocol::kMaxCommandPayload) {
        set_error(error, "command payload exceeds protocol limit");
        return false;
    }
    const protocol::CommandEnvelope envelope{
        .version = protocol::kCommandVersion,
        .type = type,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    const auto wire = protocol::encode_tcp_frame(envelope, payload);
    return !wire.empty() && socket.send_all(wire, error) ==
                                static_cast<int>(wire.size());
}

bool time_is_valid(std::uint64_t peer_time, std::uint64_t now,
                   std::chrono::seconds maximum_clock_skew) {
    const auto allowed = static_cast<std::uint64_t>(
        std::max<std::int64_t>(maximum_clock_skew.count(), 1));
    const auto difference = peer_time > now ? peer_time - now : now - peer_time;
    return difference <= allowed;
}

std::uint64_t random_request_id() {
    std::array<std::byte, sizeof(std::uint64_t)> random{};
    if (!security::generate_random(random)) {
        return 0;
    }
    const auto value = read_u64_le(random);
    return value == 0 ? 1 : value;
}

bool is_handshake_command(protocol::CommandType type) {
    return type == protocol::CommandType::auth_hello ||
           type == protocol::CommandType::auth_challenge ||
           type == protocol::CommandType::auth_proof ||
           type == protocol::CommandType::auth_accept;
}

} // namespace

AuthenticatedSession::AuthenticatedSession(
    security::Sha256Digest key, security::ClientId id,
    std::uint32_t requested_key_id, bool is_aead,
    protocol::ConnectionRole session_role) noexcept
    : session_key(key), client_id(id), key_id(requested_key_id),
      aead_enabled(is_aead), role(session_role) {}

AuthenticatedSession::~AuthenticatedSession() {
    security::secure_zero(session_key);
}

AuthenticatedSession::AuthenticatedSession(AuthenticatedSession&& other) noexcept
    : session_key(other.session_key), client_id(other.client_id),
      key_id(other.key_id), aead_enabled(other.aead_enabled),
      role(other.role) {
    security::secure_zero(other.session_key);
    other.key_id = 0;
    other.aead_enabled = false;
}

AuthenticatedSession& AuthenticatedSession::operator=(
    AuthenticatedSession&& other) noexcept {
    if (this != &other) {
        security::secure_zero(session_key);
        session_key = other.session_key;
        client_id = other.client_id;
        key_id = other.key_id;
        aead_enabled = other.aead_enabled;
        role = other.role;
        security::secure_zero(other.session_key);
        other.key_id = 0;
        other.aead_enabled = false;
    }
    return *this;
}

std::optional<AuthenticatedSession> client_handshake(
    net::TcpSocket& socket, const security::ClientId& client_id,
    std::uint32_t key_id, std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew, std::string* error,
    bool request_aead) {
    security::AuthHello hello;
    hello.client_id = client_id;
    hello.unix_time_seconds = now_unix_seconds;
    hello.key_id = key_id;
    if (!security::generate_random(hello.client_nonce) ||
        !security::validate_auth_hello(hello)) {
        set_error(error, "client hello generation failed");
        return std::nullopt;
    }
    const auto request_id = random_request_id();
    const std::uint8_t preamble_flags =
        request_aead ? protocol::kPreambleFlagAead : protocol::kPreambleFlagNone;
    if (request_id == 0 ||
        !send_preamble(socket, protocol::ConnectionRole::client, client_id,
                       key_id, preamble_flags, error) ||
        !send_plain_frame(socket, protocol::CommandType::auth_hello, request_id,
                          security::encode_auth_hello(hello), error)) {
        return std::nullopt;
    }
    const auto server_preamble = receive_preamble(socket, error);
    if (!server_preamble ||
        server_preamble->role != protocol::ConnectionRole::server) {
        set_error(error, "invalid server connection preamble");
        return std::nullopt;
    }
    const bool aead_negotiated = request_aead &&
        ((server_preamble->flags & protocol::kPreambleFlagAead) != 0);
    const auto challenge_frame = receive_plain_frame(socket, error);
    if (!challenge_frame ||
        challenge_frame->envelope.type !=
            protocol::CommandType::auth_challenge ||
        challenge_frame->envelope.request_id != request_id) {
        set_error(error, "unexpected authentication challenge");
        return std::nullopt;
    }
    const auto challenge =
        security::decode_auth_challenge(challenge_frame->payload);
    if (!challenge || !security::validate_auth_challenge(*challenge) ||
        !time_is_valid(challenge->unix_time_seconds, now_unix_seconds,
                       maximum_clock_skew)) {
        set_error(error, "invalid or stale authentication challenge");
        return std::nullopt;
    }
    auto client_proof =
        security::compute_client_proof(pre_shared_key, hello, *challenge);
    auto session_key =
        security::derive_session_key(pre_shared_key, hello, *challenge);
    if (!client_proof || !session_key ||
        !send_plain_frame(socket, protocol::CommandType::auth_proof, request_id,
                          *client_proof, error)) {
        if (session_key) {
            security::secure_zero(*session_key);
        }
        return std::nullopt;
    }
    security::secure_zero(*client_proof);
    const auto accept_frame = receive_plain_frame(socket, error);
    if (!accept_frame ||
        accept_frame->envelope.type != protocol::CommandType::auth_accept ||
        accept_frame->envelope.request_id != request_id ||
        accept_frame->payload.size() != security::kSha256Bytes) {
        security::secure_zero(*session_key);
        set_error(error, "unexpected authentication acceptance frame");
        return std::nullopt;
    }
    auto expected_server_proof =
        security::compute_server_proof(*session_key, hello, *challenge);
    if (!expected_server_proof ||
        !security::constant_time_equal(*expected_server_proof,
                                        accept_frame->payload)) {
        security::secure_zero(*session_key);
        set_error(error, "server authentication proof failed");
        return std::nullopt;
    }
    security::secure_zero(*expected_server_proof);
    return AuthenticatedSession{*session_key, client_id, key_id,
                                aead_negotiated, protocol::ConnectionRole::client};
}

std::optional<AuthenticatedSession> server_handshake(
    net::TcpSocket& socket, const KeyResolver& key_resolver,
    security::ReplayProtector& replay_protector,
    std::uint64_t now_unix_seconds, std::string* error,
    bool allow_aead) {
    const auto preamble = receive_preamble(socket, error);
    if (!preamble || preamble->role != protocol::ConnectionRole::client) {
        set_error(error, "expected client connection preamble");
        return std::nullopt;
    }
    const bool aead_negotiated = allow_aead &&
        ((preamble->flags & protocol::kPreambleFlagAead) != 0);
    const auto hello_frame = receive_plain_frame(socket, error);
    if (!hello_frame ||
        hello_frame->envelope.type != protocol::CommandType::auth_hello) {
        set_error(error, "expected authentication hello");
        return std::nullopt;
    }
    const auto hello = security::decode_auth_hello(hello_frame->payload);
    if (!hello || !security::validate_auth_hello(*hello)) {
        set_error(error, "invalid authentication hello");
        return std::nullopt;
    }
    if (hello->client_id != preamble->identity ||
        hello->key_id != preamble->key_id) {
        set_error(error, "connection preamble identity mismatch");
        return std::nullopt;
    }
    auto pre_shared_key = key_resolver(hello->client_id, hello->key_id);
    if (!pre_shared_key ||
        pre_shared_key->size() < security::kMinimumProtocolKeyBytes) {
        set_error(error, "unknown client key");
        return std::nullopt;
    }
    const auto wipe_key = [&pre_shared_key] {
        if (pre_shared_key) {
            security::secure_zero(*pre_shared_key);
        }
    };
    security::AuthChallenge challenge;
    challenge.unix_time_seconds = now_unix_seconds;
    const std::uint8_t server_flags =
        aead_negotiated ? protocol::kPreambleFlagAead : protocol::kPreambleFlagNone;
    if (!security::generate_random(challenge.server_nonce) ||
        !send_preamble(socket, protocol::ConnectionRole::server, {}, 0,
                       server_flags, error) ||
        !send_plain_frame(socket, protocol::CommandType::auth_challenge,
                          hello_frame->envelope.request_id,
                          security::encode_auth_challenge(challenge), error)) {
        wipe_key();
        return std::nullopt;
    }
    const auto proof_frame = receive_plain_frame(socket, error);
    if (!proof_frame ||
        proof_frame->envelope.type != protocol::CommandType::auth_proof ||
        proof_frame->envelope.request_id != hello_frame->envelope.request_id ||
        proof_frame->payload.size() != security::kSha256Bytes) {
        wipe_key();
        set_error(error, "unexpected client authentication proof");
        return std::nullopt;
    }
    auto expected_client_proof =
        security::compute_client_proof(*pre_shared_key, *hello, challenge);
    if (!expected_client_proof ||
        !security::constant_time_equal(*expected_client_proof,
                                       proof_frame->payload) ||
        !replay_protector.accept(*hello, now_unix_seconds)) {
        if (expected_client_proof) {
            security::secure_zero(*expected_client_proof);
        }
        wipe_key();
        set_error(error, "client authentication proof or replay check failed");
        return std::nullopt;
    }
    security::secure_zero(*expected_client_proof);
    auto session_key =
        security::derive_session_key(*pre_shared_key, *hello, challenge);
    wipe_key();
    if (!session_key) {
        set_error(error, "session key derivation failed");
        return std::nullopt;
    }
    auto server_proof =
        security::compute_server_proof(*session_key, *hello, challenge);
    if (!server_proof ||
        !send_plain_frame(socket, protocol::CommandType::auth_accept,
                          hello_frame->envelope.request_id, *server_proof,
                          error)) {
        security::secure_zero(*session_key);
        return std::nullopt;
    }
    security::secure_zero(*server_proof);
    return AuthenticatedSession{*session_key, hello->client_id, hello->key_id,
                                aead_negotiated, protocol::ConnectionRole::server};
}

std::vector<std::byte> encode_authenticated_command(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload) {
    if (is_handshake_command(envelope.type) ||
        payload.size() > protocol::kMaxCommandPayload - kAuthenticatedOverhead) {
        return {};
    }
    protocol::CommandEnvelope normalized = envelope;
    normalized.version = protocol::kCommandVersion;
    normalized.payload_bytes = static_cast<std::uint32_t>(payload.size());
    const auto tag = security::compute_control_auth_tag(
        session_key, normalized, sequence, payload);
    if (!tag) {
        return {};
    }
    std::vector<std::byte> authenticated_payload(kAuthenticatedOverhead +
                                                  payload.size());
    write_u64_le(authenticated_payload, sequence);
    std::copy(tag->begin(), tag->end(),
              authenticated_payload.begin() + sizeof(sequence));
    std::copy(payload.begin(), payload.end(),
              authenticated_payload.begin() + kAuthenticatedOverhead);
    protocol::CommandEnvelope outer = normalized;
    outer.payload_bytes =
        static_cast<std::uint32_t>(authenticated_payload.size());
    return protocol::encode_tcp_frame(outer, authenticated_payload);
}

std::optional<AuthenticatedCommand> decode_authenticated_command(
    std::span<const std::byte> session_key,
    const protocol::TcpFrame& wire_frame, std::string* error) {
    if (wire_frame.payload.size() < kAuthenticatedOverhead ||
        wire_frame.envelope.payload_bytes != wire_frame.payload.size() ||
        is_handshake_command(wire_frame.envelope.type)) {
        set_error(error, "authenticated command wrapper is truncated");
        return std::nullopt;
    }
    const auto sequence = read_u64_le(wire_frame.payload);
    const auto tag = std::span<const std::byte>(wire_frame.payload).subspan(
        sizeof(sequence), security::kControlAuthTagBytes);
    const auto payload = std::span<const std::byte>(wire_frame.payload).subspan(
        kAuthenticatedOverhead);
    protocol::CommandEnvelope normalized = wire_frame.envelope;
    normalized.payload_bytes = static_cast<std::uint32_t>(payload.size());
    if (!security::verify_control_auth_tag(session_key, normalized, sequence,
                                           payload, tag)) {
        set_error(error, "authenticated command MAC failed");
        return std::nullopt;
    }
    AuthenticatedCommand command{normalized, sequence, {}};
    command.payload.assign(payload.begin(), payload.end());
    return command;
}

std::optional<AeadSessionKeys> derive_aead_session_keys(
    std::span<const std::byte> session_key) noexcept {
    if (session_key.size() < security::kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    constexpr char c2s_key_label[] = "NSTU-AEAD-C2S-KEY-V1";
    constexpr char s2c_key_label[] = "NSTU-AEAD-S2C-KEY-V1";
    constexpr char c2s_iv_label[] = "NSTU-AEAD-C2S-IV-V1";
    constexpr char s2c_iv_label[] = "NSTU-AEAD-S2C-IV-V1";

    const auto c2s_key = security::hmac_sha256(
        session_key,
        {reinterpret_cast<const std::byte*>(c2s_key_label), sizeof(c2s_key_label) - 1});
    const auto s2c_key = security::hmac_sha256(
        session_key,
        {reinterpret_cast<const std::byte*>(s2c_key_label), sizeof(s2c_key_label) - 1});
    const auto c2s_iv = security::hmac_sha256(
        session_key,
        {reinterpret_cast<const std::byte*>(c2s_iv_label), sizeof(c2s_iv_label) - 1});
    const auto s2c_iv = security::hmac_sha256(
        session_key,
        {reinterpret_cast<const std::byte*>(s2c_iv_label), sizeof(s2c_iv_label) - 1});

    if (!c2s_key || !s2c_key || !c2s_iv || !s2c_iv) {
        return std::nullopt;
    }

    AeadSessionKeys keys;
    keys.client_to_server_key = *c2s_key;
    keys.server_to_client_key = *s2c_key;
    std::copy_n(c2s_iv->begin(), kAeadNonceBytes, keys.client_to_server_iv.begin());
    std::copy_n(s2c_iv->begin(), kAeadNonceBytes, keys.server_to_client_iv.begin());
    return keys;
}

std::array<std::byte, kAeadNonceBytes> construct_aead_nonce(
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    std::uint64_t sequence) noexcept {
    std::array<std::byte, kAeadNonceBytes> nonce{};
    std::copy(iv_base.begin(), iv_base.end(), nonce.begin());
    for (std::size_t i = 0; i < sizeof(sequence); ++i) {
        nonce[4 + i] ^= static_cast<std::byte>((sequence >> (i * 8u)) & 0xffu);
    }
    return nonce;
}

std::vector<std::byte> encode_authenticated_command_aead(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload) {
    if (is_handshake_command(envelope.type) ||
        payload.size() > protocol::kMaxCommandPayload - kAuthenticatedOverhead) {
        return {};
    }
    protocol::CommandEnvelope normalized = envelope;
    normalized.version = protocol::kCommandVersion;
    normalized.payload_bytes = static_cast<std::uint32_t>(payload.size());

    const auto aad = make_aead_aad(normalized, sequence);
    const auto nonce = construct_aead_nonce(iv_base, sequence);

    std::vector<std::byte> ciphertext(payload.size());
    std::array<std::byte, kAeadAuthTagBytes> tag{};
    std::string encrypt_error;
    if (!encrypt_aead_gcm(key, nonce, aad, payload, ciphertext, tag, &encrypt_error)) {
        return {};
    }

    std::vector<std::byte> authenticated_payload(kAuthenticatedOverhead + payload.size());
    write_u64_le(authenticated_payload, sequence);
    std::copy(tag.begin(), tag.end(), authenticated_payload.begin() + sizeof(sequence));
    if (!ciphertext.empty()) {
        std::copy(ciphertext.begin(), ciphertext.end(),
                  authenticated_payload.begin() + kAuthenticatedOverhead);
    }

    protocol::CommandEnvelope outer = normalized;
    outer.payload_bytes = static_cast<std::uint32_t>(authenticated_payload.size());
    return protocol::encode_tcp_frame(outer, authenticated_payload);
}

std::optional<AuthenticatedCommand> decode_authenticated_command_aead(
    std::span<const std::byte> key,
    std::span<const std::byte, kAeadNonceBytes> iv_base,
    const protocol::TcpFrame& wire_frame, std::string* error) {
    if (wire_frame.payload.size() < kAuthenticatedOverhead ||
        wire_frame.envelope.payload_bytes != wire_frame.payload.size() ||
        is_handshake_command(wire_frame.envelope.type)) {
        set_error(error, "authenticated command wrapper is truncated");
        return std::nullopt;
    }
    const auto sequence = read_u64_le(wire_frame.payload);
    std::array<std::byte, kAeadAuthTagBytes> tag{};
    std::copy_n(wire_frame.payload.begin() + sizeof(sequence),
                kAeadAuthTagBytes, tag.begin());
    const auto ciphertext = std::span<const std::byte>(wire_frame.payload).subspan(
        kAuthenticatedOverhead);

    protocol::CommandEnvelope normalized = wire_frame.envelope;
    normalized.payload_bytes = static_cast<std::uint32_t>(ciphertext.size());

    const auto aad = make_aead_aad(normalized, sequence);
    const auto nonce = construct_aead_nonce(iv_base, sequence);

    std::vector<std::byte> plaintext(ciphertext.size());
    if (!decrypt_aead_gcm(key, nonce, aad, ciphertext, tag, plaintext, error)) {
        return std::nullopt;
    }

    AuthenticatedCommand command{normalized, sequence, std::move(plaintext)};
    return command;
}

AuthenticatedControlChannel::AuthenticatedControlChannel(
    net::TcpSocket socket, security::Sha256Digest session_key) noexcept
    : socket_(std::move(socket)), session_key_(session_key),
      aead_enabled_(false), role_(protocol::ConnectionRole::client) {
    receive_sequence_.reset(0);
}

AuthenticatedControlChannel::AuthenticatedControlChannel(
    net::TcpSocket socket, AuthenticatedSession&& session) noexcept
    : socket_(std::move(socket)), session_key_(session.session_key),
      aead_enabled_(session.aead_enabled), role_(session.role) {
    if (aead_enabled_) {
        const auto keys = derive_aead_session_keys(session_key_);
        if (keys) {
            if (role_ == protocol::ConnectionRole::client) {
                send_key_ = keys->client_to_server_key;
                send_iv_base_ = keys->client_to_server_iv;
                receive_key_ = keys->server_to_client_key;
                receive_iv_base_ = keys->server_to_client_iv;
            } else {
                send_key_ = keys->server_to_client_key;
                send_iv_base_ = keys->server_to_client_iv;
                receive_key_ = keys->client_to_server_key;
                receive_iv_base_ = keys->client_to_server_iv;
            }
        } else {
            aead_enabled_ = false;
        }
    }
    security::secure_zero(session.session_key);
    receive_sequence_.reset(0);
}

AuthenticatedControlChannel::~AuthenticatedControlChannel() {
    close();
}

AuthenticatedControlChannel::AuthenticatedControlChannel(
    AuthenticatedControlChannel&& other) noexcept
    : socket_(std::move(other.socket_)), session_key_(other.session_key_),
      aead_enabled_(other.aead_enabled_), role_(other.role_),
      send_key_(other.send_key_), send_iv_base_(other.send_iv_base_),
      receive_key_(other.receive_key_), receive_iv_base_(other.receive_iv_base_),
      receive_sequence_(other.receive_sequence_),
      send_sequence_(other.send_sequence_),
      send_exhausted_(other.send_exhausted_) {
    security::secure_zero(other.session_key_);
    security::secure_zero(other.send_key_);
    security::secure_zero(other.receive_key_);
    security::secure_zero(other.send_iv_base_);
    security::secure_zero(other.receive_iv_base_);
    other.aead_enabled_ = false;
    other.send_exhausted_ = true;
}

AuthenticatedControlChannel& AuthenticatedControlChannel::operator=(
    AuthenticatedControlChannel&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::move(other.socket_);
        session_key_ = other.session_key_;
        aead_enabled_ = other.aead_enabled_;
        role_ = other.role_;
        send_key_ = other.send_key_;
        send_iv_base_ = other.send_iv_base_;
        receive_key_ = other.receive_key_;
        receive_iv_base_ = other.receive_iv_base_;
        receive_sequence_ = other.receive_sequence_;
        send_sequence_ = other.send_sequence_;
        send_exhausted_ = other.send_exhausted_;
        security::secure_zero(other.session_key_);
        security::secure_zero(other.send_key_);
        security::secure_zero(other.receive_key_);
        security::secure_zero(other.send_iv_base_);
        security::secure_zero(other.receive_iv_base_);
        other.aead_enabled_ = false;
        other.send_exhausted_ = true;
    }
    return *this;
}

bool AuthenticatedControlChannel::send(protocol::CommandType type,
                                       std::uint64_t request_id,
                                       std::span<const std::byte> payload,
                                       std::string* error) {
    return send_impl(type, request_id, payload, std::nullopt, error);
}

bool AuthenticatedControlChannel::send(
    protocol::CommandType type, std::uint64_t request_id,
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point deadline,
    std::string* error) {
    return send_impl(type, request_id, payload, deadline, error);
}

bool AuthenticatedControlChannel::send_impl(
    protocol::CommandType type, std::uint64_t request_id,
    std::span<const std::byte> payload,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    std::string* error) {
    if (!socket_.is_open() || send_exhausted_ || is_handshake_command(type)) {
        set_error(error, "authenticated channel cannot send");
        return false;
    }
    const protocol::CommandEnvelope envelope{
        .version = protocol::kCommandVersion,
        .type = type,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    std::vector<std::byte> wire;
    if (aead_enabled_) {
        wire = encode_authenticated_command_aead(
            send_key_, send_iv_base_, envelope, send_sequence_, payload);
    } else {
        wire = encode_authenticated_command(
            session_key_, envelope, send_sequence_, payload);
    }
    if (wire.empty()) {
        if (deadline) set_error(error, "authenticated command encoding failed");
        return false;
    }
    const int sent = deadline ? socket_.send_all(wire, *deadline, error)
                              : socket_.send_all(wire, error);
    if (sent != static_cast<int>(wire.size())) {
        // Preserve ordinary-send failure behavior; deadline failures invalidate
        // the stream and securely close the authenticated channel.
        if (deadline) close();
        return false;
    }
    if (send_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        send_exhausted_ = true;
    } else {
        ++send_sequence_;
    }
    return true;
}

bool AuthenticatedControlChannel::send_with_deadline(
    protocol::CommandType type, std::uint64_t request_id,
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point deadline,
    std::string* error) {
    return send(type, request_id, payload, deadline, error);
}

std::optional<AuthenticatedCommand> AuthenticatedControlChannel::receive(
    std::string* error) {
    if (!socket_.is_open()) {
        set_error(error, "authenticated channel is closed");
        return std::nullopt;
    }
    const auto frame = receive_plain_frame(socket_, error);
    if (!frame) {
        return std::nullopt;
    }
    std::optional<AuthenticatedCommand> command;
    if (aead_enabled_) {
        command = decode_authenticated_command_aead(
            receive_key_, receive_iv_base_, *frame, error);
    } else {
        command = decode_authenticated_command(session_key_, *frame, error);
    }
    if (!command || !receive_sequence_.accept(command->sequence)) {
        set_error(error, "authenticated command sequence rejected");
        return std::nullopt;
    }
    return command;
}

bool AuthenticatedControlChannel::wait_readable(std::uint32_t timeout_ms,
                                                std::string* error) const {
    return socket_.wait_readable(timeout_ms, error);
}

bool AuthenticatedControlChannel::is_open() const noexcept {
    return socket_.is_open();
}

bool AuthenticatedControlChannel::is_aead() const noexcept {
    return aead_enabled_;
}

void AuthenticatedControlChannel::close() noexcept {
    socket_.close();
    security::secure_zero(session_key_);
    security::secure_zero(send_key_);
    security::secure_zero(receive_key_);
    security::secure_zero(send_iv_base_);
    security::secure_zero(receive_iv_base_);
    aead_enabled_ = false;
    send_exhausted_ = true;
}

} // namespace nstu::control
