#include "nstu/control_channel.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

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
                   std::string* error) {
    protocol::ConnectionPreamble preamble;
    preamble.role = role;
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
    std::uint32_t requested_key_id) noexcept
    : session_key(key), client_id(id), key_id(requested_key_id) {}

AuthenticatedSession::~AuthenticatedSession() {
    security::secure_zero(session_key);
}

AuthenticatedSession::AuthenticatedSession(AuthenticatedSession&& other) noexcept
    : session_key(other.session_key), client_id(other.client_id),
      key_id(other.key_id) {
    security::secure_zero(other.session_key);
    other.key_id = 0;
}

AuthenticatedSession& AuthenticatedSession::operator=(
    AuthenticatedSession&& other) noexcept {
    if (this != &other) {
        security::secure_zero(session_key);
        session_key = other.session_key;
        client_id = other.client_id;
        key_id = other.key_id;
        security::secure_zero(other.session_key);
        other.key_id = 0;
    }
    return *this;
}

std::optional<AuthenticatedSession> client_handshake(
    net::TcpSocket& socket, const security::ClientId& client_id,
    std::uint32_t key_id, std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew, std::string* error) {
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
    if (request_id == 0 ||
        !send_preamble(socket, protocol::ConnectionRole::client, client_id,
                       key_id, error) ||
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
    return AuthenticatedSession{*session_key, client_id, key_id};
}

std::optional<AuthenticatedSession> server_handshake(
    net::TcpSocket& socket, const KeyResolver& key_resolver,
    security::ReplayProtector& replay_protector,
    std::uint64_t now_unix_seconds, std::string* error) {
    const auto preamble = receive_preamble(socket, error);
    if (!preamble || preamble->role != protocol::ConnectionRole::client) {
        set_error(error, "expected client connection preamble");
        return std::nullopt;
    }
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
    if (!security::generate_random(challenge.server_nonce) ||
        !send_preamble(socket, protocol::ConnectionRole::server, {}, 0,
                       error) ||
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
    return AuthenticatedSession{*session_key, hello->client_id, hello->key_id};
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

AuthenticatedControlChannel::AuthenticatedControlChannel(
    net::TcpSocket socket, security::Sha256Digest session_key) noexcept
    : socket_(std::move(socket)), session_key_(session_key) {
    receive_sequence_.reset(0);
}

AuthenticatedControlChannel::AuthenticatedControlChannel(
    net::TcpSocket socket, AuthenticatedSession&& session) noexcept
    : socket_(std::move(socket)), session_key_(session.session_key) {
    security::secure_zero(session.session_key);
    receive_sequence_.reset(0);
}

AuthenticatedControlChannel::~AuthenticatedControlChannel() {
    close();
}

AuthenticatedControlChannel::AuthenticatedControlChannel(
    AuthenticatedControlChannel&& other) noexcept
    : socket_(std::move(other.socket_)), session_key_(other.session_key_),
      receive_sequence_(other.receive_sequence_),
      send_sequence_(other.send_sequence_),
      send_exhausted_(other.send_exhausted_) {
    security::secure_zero(other.session_key_);
    other.send_exhausted_ = true;
}

AuthenticatedControlChannel& AuthenticatedControlChannel::operator=(
    AuthenticatedControlChannel&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::move(other.socket_);
        session_key_ = other.session_key_;
        receive_sequence_ = other.receive_sequence_;
        send_sequence_ = other.send_sequence_;
        send_exhausted_ = other.send_exhausted_;
        security::secure_zero(other.session_key_);
        other.send_exhausted_ = true;
    }
    return *this;
}

bool AuthenticatedControlChannel::send(protocol::CommandType type,
                                       std::uint64_t request_id,
                                       std::span<const std::byte> payload,
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
    const auto wire = encode_authenticated_command(
        session_key_, envelope, send_sequence_, payload);
    if (wire.empty() || socket_.send_all(wire, error) !=
                            static_cast<int>(wire.size())) {
        return false;
    }
    if (send_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        send_exhausted_ = true;
    } else {
        ++send_sequence_;
    }
    return true;
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
    auto command = decode_authenticated_command(session_key_, *frame, error);
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

void AuthenticatedControlChannel::close() noexcept {
    socket_.close();
    security::secure_zero(session_key_);
    send_exhausted_ = true;
}

} // namespace nstu::control
