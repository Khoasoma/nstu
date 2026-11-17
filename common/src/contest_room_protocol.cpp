#include "nstu/contest_room_protocol.hpp"
#include "nstu/programming.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#endif

#include <algorithm>
#include <array>
#include <thread>
#include <type_traits>
#include <utility>

namespace nstu::contest_room {
namespace {
constexpr std::string_view kDomain = "NSTU_CONTEST_V1";
constexpr std::string_view kAuthority = "contest_candidate";
constexpr std::string_view kKeyInfo = "NSTU_CONTEST_V1/ECDH";
constexpr std::size_t kAuthenticatedOverhead = 8 + control::kAeadAuthTagBytes;
static_assert(kMaximumMessageBytes + kAuthenticatedOverhead <= protocol::kMaxCommandPayload);

void set_error(std::string* error, const char* message) { if (error) *error = message; }
bool valid_side(Side side) noexcept { return side == Side::candidate || side == Side::teacher; }
bool nonzero(std::span<const std::byte> value) noexcept {
    return std::any_of(value.begin(), value.end(), [](std::byte byte) { return byte != std::byte{0}; });
}
bool valid_id(const std::string& value) noexcept {
    return value.size() == 32 &&
        value.find_first_not_of("0123456789abcdef") == std::string::npos &&
        value.find_first_not_of('0') != std::string::npos;
}
bool valid_teacher(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaximumTeacherIdBytes && value.front() != '.' && value.front() != '-' &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}
template<typename T> void append(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = 0; i < sizeof(T); ++i) { output.push_back(static_cast<std::byte>(value & 0xffu)); value >>= 8u; }
}
template<typename T> T read(std::span<const std::byte> input, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto byte = static_cast<T>(std::to_integer<unsigned int>(input[offset + i]));
        value = static_cast<T>(value | static_cast<T>(byte << (8u * i)));
    }
    return value;
}
void append_bytes(std::vector<std::byte>& output, std::span<const std::byte> bytes) { output.insert(output.end(), bytes.begin(), bytes.end()); }
void field(std::vector<std::byte>& output, std::span<const std::byte> bytes) { append(output, static_cast<std::uint16_t>(bytes.size())); append_bytes(output, bytes); }
void field(std::vector<std::byte>& output, std::string_view text) { field(output, std::as_bytes(std::span(text))); }
bool magic(std::span<const std::byte> wire, std::string_view value) {
    return wire.size() >= value.size() && std::equal(value.begin(), value.end(), wire.begin(), [](char c, std::byte byte) { return static_cast<std::byte>(c) == byte; });
}
bool valid_chunk(std::span<const std::byte> wire) {
    if (wire.size() <= kChunkHeaderBytes ||
        wire.size() > kChunkHeaderBytes + kMaximumChunkDataBytes ||
        !nonzero(wire.first(security::kSha256Bytes))) return false;
    const auto offset = read<std::uint64_t>(wire, 32);
    const auto total = read<std::uint64_t>(wire, 40);
    return total > 0 && total <= kMaximumArchiveBytes && offset < total && wire.size() - kChunkHeaderBytes <= total - offset;
}
bool valid_body(Kind kind, std::span<const std::byte> body) {
    switch (kind) {
    case Kind::heartbeat: case Kind::standings_request: case Kind::leave: return body.empty();
    case Kind::join: return !body.empty() && body.size() <= 256;
    case Kind::join_status: return !body.empty() && body.size() <= 4096;
    case Kind::package_offer: case Kind::package_ack:
        if (body.size() != 40 || !nonzero(body.first(32))) return false;
        return read<std::uint64_t>(body, 32) <= kMaximumArchiveBytes && (kind != Kind::package_offer || read<std::uint64_t>(body, 32) != 0);
    case Kind::prepared: return body.size() == 32 && nonzero(body);
    case Kind::package_chunk: return valid_chunk(body);
    case Kind::programming_request: {
        const auto request = exam::programming::decode_request(body);
        return request && !request->workstation_policy_ready;
    }
    case Kind::programming_reply: case Kind::standings: return !body.empty() && body.size() <= kMaximumSnapshotBytes;
    case Kind::lifecycle: return !body.empty() && body.size() <= 256;
    }
    return false;
}

// Peek only under this object's exclusive socket ownership. A larger receive
// buffer lets one bounded frame arrive fully before the existing blocking
// decoder consumes it; partial frames are polled under one absolute deadline.
template<typename FrameSize>
bool wait_complete(std::uintptr_t handle, std::size_t prefix_bytes, FrameSize frame_size,
                   std::chrono::steady_clock::time_point deadline, std::string* error) {
#if defined(_WIN32)
    std::array<std::byte, 4 + protocol::kCommandHeaderBytes> prefix{};
    const auto socket = static_cast<SOCKET>(handle);
    while (std::chrono::steady_clock::now() < deadline) {
        u_long available = 0;
        if (ioctlsocket(socket, FIONREAD, &available) == SOCKET_ERROR) { set_error(error, "room receive availability failed"); return false; }
        if (available >= prefix_bytes) {
            const int count = recv(socket, reinterpret_cast<char*>(prefix.data()), static_cast<int>(prefix_bytes), MSG_PEEK);
            if (count != static_cast<int>(prefix_bytes)) { set_error(error, "room frame prefix failed"); return false; }
            const auto total = frame_size(std::span(prefix).first(prefix_bytes));
            if (!total) { set_error(error, "room frame prefix rejected"); return false; }
            if (available >= *total) return std::chrono::steady_clock::now() < deadline;
        } else if (available == 0) {
            fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable);
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) break;
            const auto wait = net::detail::deadline_wait_duration(remaining);
            timeval timeout{wait.seconds, wait.microseconds};
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) { set_error(error, "room receive wait failed"); return false; }
            if (ready == 0) break;
            const int count = recv(socket, reinterpret_cast<char*>(prefix.data()), 1, MSG_PEEK);
            if (count <= 0) { set_error(error, "room peer closed"); return false; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    set_error(error, "room receive deadline expired");
#else
    (void)handle; (void)prefix_bytes; (void)frame_size; (void)deadline;
    set_error(error, "room transport requires Windows");
#endif
    return false;
}
}

std::optional<std::vector<std::byte>> encode_realm(const Realm& realm) {
    if (realm.version != kVersion || !valid_teacher(realm.teacher_id) || !valid_id(realm.room_id) || !valid_id(realm.round_id)) return std::nullopt;
    std::vector<std::byte> wire;
    field(wire, kDomain); append(wire, std::uint16_t{2}); append(wire, realm.version);
    field(wire, realm.teacher_id); field(wire, realm.room_id); field(wire, realm.round_id); field(wire, kAuthority);
    return wire;
}
std::optional<pairing::PairingSecrets> derive_room_secrets(std::span<const std::byte> shared_secret, const Realm& realm, const pairing::PairingTranscript& transcript) {
    auto canonical = encode_realm(realm);
    if (!canonical || transcript.version != pairing::kPairingVersion || shared_secret.size() < security::kMinimumProtocolKeyBytes) return std::nullopt;
    const auto encoded = pairing::encode_transcript(transcript); if (!encoded) return std::nullopt;
    field(*canonical, *encoded);
    const auto salt = security::sha256(*canonical); if (!salt) return std::nullopt;
    auto bound = pairing::hkdf_sha256(shared_secret, *salt, std::as_bytes(std::span(kKeyInfo))); if (!bound) return std::nullopt;
    auto secrets = pairing::derive_pairing_secrets(*bound, transcript);
    security::secure_zero(*bound);
    return secrets;
}
std::vector<std::byte> encode_preface(const Realm& realm, Side side, const security::Nonce& nonce) {
    const auto encoded = encode_realm(realm); if (!encoded || !valid_side(side) || !nonzero(nonce)) return {};
    std::vector<std::byte> wire;
    append_bytes(wire, std::as_bytes(std::span(std::string_view{"NSCP"})));
    append(wire, kVersion); append(wire, static_cast<std::uint16_t>(side)); append(wire, static_cast<std::uint16_t>(encoded->size()));
    append_bytes(wire, nonce); append_bytes(wire, *encoded); return wire;
}
std::optional<Preface> decode_preface(std::span<const std::byte> wire, const Realm& pinned, Side expected_side, std::optional<security::Nonce> expected_nonce) {
    const auto encoded = encode_realm(pinned);
    if (!encoded || !valid_side(expected_side) ||
        wire.size() != kPrefaceHeaderBytes + encoded->size() ||
        !magic(wire, "NSCP") || read<std::uint16_t>(wire, 4) != kVersion ||
        read<std::uint16_t>(wire, 6) != static_cast<std::uint16_t>(expected_side) ||
        read<std::uint16_t>(wire, 8) != encoded->size() ||
        !std::equal(encoded->begin(), encoded->end(),
                    wire.begin() + static_cast<std::ptrdiff_t>(kPrefaceHeaderBytes))) return std::nullopt;
    Preface result{expected_side, {}};
    std::copy_n(wire.begin() + 10, result.nonce.size(), result.nonce.begin());
    if (!nonzero(result.nonce) || (expected_nonce && !security::constant_time_equal(result.nonce, *expected_nonce))) return std::nullopt;
    return result;
}
bool send_preface(net::TcpSocket& socket, const Realm& realm, Side side, const security::Nonce& nonce, std::chrono::steady_clock::time_point deadline, std::string* error) {
    const auto wire = encode_preface(realm, side, nonce);
    if (wire.empty()) { set_error(error, "invalid contest preface"); return false; }
    return socket.send_all(wire, deadline, error) == static_cast<int>(wire.size());
}
std::optional<Preface> receive_preface(net::TcpSocket& socket, const Realm& pinned, Side expected_side, std::chrono::steady_clock::time_point deadline, std::optional<security::Nonce> expected_nonce, std::string* error) {
    const auto encoded = encode_realm(pinned);
    if (!encoded || !valid_side(expected_side)) { set_error(error, "invalid contest realm"); socket.close(); return std::nullopt; }
    const auto total = kPrefaceHeaderBytes + encoded->size();
    if (!wait_complete(socket.native_handle(), 10, [&](std::span<const std::byte> prefix) -> std::optional<std::size_t> {
        if (!magic(prefix, "NSCP") || read<std::uint16_t>(prefix, 4) != kVersion || read<std::uint16_t>(prefix, 6) != static_cast<std::uint16_t>(expected_side) || read<std::uint16_t>(prefix, 8) != encoded->size()) return std::nullopt;
        return total;
    }, deadline, error)) { socket.close(); return std::nullopt; }
    std::vector<std::byte> wire(total);
    std::size_t offset = 0;
    while (offset < total) { const auto count = socket.receive(std::span(wire).subspan(offset), error); if (count <= 0) { socket.close(); return std::nullopt; } offset += static_cast<std::size_t>(count); }
    auto decoded = decode_preface(wire, pinned, expected_side, expected_nonce);
    if (!decoded) { set_error(error, "contest preface rejected"); socket.close(); }
    return decoded;
}
bool allowed(Side sender, Kind kind) noexcept {
    switch (kind) {
    case Kind::heartbeat: return valid_side(sender);
    case Kind::join: case Kind::package_ack: case Kind::prepared: case Kind::programming_request: case Kind::standings_request: case Kind::leave: return sender == Side::candidate;
    case Kind::join_status: case Kind::package_offer: case Kind::package_chunk: case Kind::programming_reply: case Kind::lifecycle: case Kind::standings: return sender == Side::teacher;
    }
    return false;
}
std::vector<std::byte> encode_message(const Realm& realm, Side sender, const Message& message) {
    const auto encoded = encode_realm(realm);
    if (!encoded || !allowed(sender, message.kind) ||
        message.payload.size() > kMaximumMessageBytes - kMessageHeaderBytes - encoded->size() ||
        !valid_body(message.kind, message.payload)) return {};
    std::vector<std::byte> wire;
    wire.reserve(kMessageHeaderBytes + encoded->size() + message.payload.size());
    append_bytes(wire, std::as_bytes(std::span(std::string_view{"NSCM"})));
    append(wire, kVersion); append(wire, static_cast<std::uint16_t>(sender)); append(wire, static_cast<std::uint16_t>(message.kind));
    append(wire, static_cast<std::uint16_t>(encoded->size())); append(wire, static_cast<std::uint32_t>(message.payload.size())); append(wire, message.request_id);
    append_bytes(wire, *encoded); append_bytes(wire, message.payload); return wire;
}
std::optional<Message> decode_message(std::span<const std::byte> wire, const Realm& pinned, Side expected_sender) {
    const auto encoded = encode_realm(pinned);
    if (!encoded || wire.size() < kMessageHeaderBytes + encoded->size() ||
        wire.size() > kMaximumMessageBytes || !magic(wire, "NSCM") ||
        read<std::uint16_t>(wire, 4) != kVersion ||
        read<std::uint16_t>(wire, 6) != static_cast<std::uint16_t>(expected_sender) ||
        read<std::uint16_t>(wire, 10) != encoded->size() ||
        !std::equal(encoded->begin(), encoded->end(),
                    wire.begin() + static_cast<std::ptrdiff_t>(kMessageHeaderBytes))) return std::nullopt;
    const auto kind = static_cast<Kind>(read<std::uint16_t>(wire, 8));
    const auto body = wire.subspan(kMessageHeaderBytes + encoded->size());
    if (read<std::uint32_t>(wire, 12) != body.size() || !allowed(expected_sender, kind) || !valid_body(kind, body)) return std::nullopt;
    return Message{kind, read<std::uint64_t>(wire, 16), std::vector<std::byte>(body.begin(), body.end())};
}
std::optional<Message> decode_command(const control::AuthenticatedCommand& command, const Realm& pinned, Side expected_sender) {
    if (command.envelope.type != protocol::CommandType::heartbeat) return std::nullopt;
    auto message = decode_message(command.payload, pinned, expected_sender);
    if (!message || message->request_id != command.envelope.request_id) return std::nullopt;
    return message;
}
std::vector<std::byte> encode_chunk(const PackageChunk& chunk) {
    if (!nonzero(chunk.digest) || chunk.total_bytes == 0 ||
        chunk.total_bytes > kMaximumArchiveBytes || chunk.offset >= chunk.total_bytes ||
        chunk.data.empty() || chunk.data.size() > kMaximumChunkDataBytes ||
        chunk.data.size() > chunk.total_bytes - chunk.offset) return {};
    std::vector<std::byte> wire; wire.reserve(kChunkHeaderBytes + chunk.data.size());
    append_bytes(wire, chunk.digest); append(wire, chunk.offset); append(wire, chunk.total_bytes); append_bytes(wire, chunk.data); return wire;
}
std::optional<PackageChunk> decode_chunk(std::span<const std::byte> wire) {
    if (!valid_chunk(wire)) return std::nullopt;
    PackageChunk chunk; std::copy_n(wire.begin(), chunk.digest.size(), chunk.digest.begin());
    chunk.offset = read<std::uint64_t>(wire, 32); chunk.total_bytes = read<std::uint64_t>(wire, 40);
    chunk.data.assign(wire.begin() + static_cast<std::ptrdiff_t>(kChunkHeaderBytes), wire.end()); return chunk;
}

RoomChannel::RoomChannel(net::TcpSocket socket, control::AuthenticatedSession&& session, Realm pinned, Side local_side)
    : native_handle_(socket.native_handle()), channel_([&] {
        const auto expected = local_side == Side::candidate ? protocol::ConnectionRole::client : protocol::ConnectionRole::server;
        const bool valid = valid_side(local_side) && session.aead_enabled && session.role == expected;
        control::AuthenticatedControlChannel channel(std::move(socket), std::move(session));
        if (!valid) channel.close();
        return channel;
      }()), pinned_(std::move(pinned)), local_side_(local_side) {
    if (!encode_realm(pinned_) || !channel_.is_aead()) { close(); return; }
#if defined(_WIN32)
    const int buffer = static_cast<int>(2 * (4 + protocol::kCommandHeaderBytes + kMaximumMessageBytes + kAuthenticatedOverhead));
    if (setsockopt(static_cast<SOCKET>(native_handle_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), static_cast<int>(sizeof(buffer))) == SOCKET_ERROR) close();
#else
    close();
#endif
}
bool RoomChannel::send(const Message& message, std::chrono::steady_clock::time_point deadline, std::string* error) {
    if (!is_open()) { set_error(error, "room AEAD channel unavailable"); return false; }
    const auto payload = encode_message(pinned_, local_side_, message);
    if (payload.empty()) { set_error(error, "invalid room message"); return false; }
    return channel_.send(protocol::CommandType::heartbeat, message.request_id, payload, deadline, error);
}
std::optional<Message> RoomChannel::receive(std::chrono::steady_clock::time_point deadline, std::string* error) {
    if (!is_open()) { set_error(error, "room AEAD channel unavailable"); return std::nullopt; }
    if (!wait_complete(native_handle_, 4 + protocol::kCommandHeaderBytes, [](std::span<const std::byte> prefix) -> std::optional<std::size_t> {
        const auto envelope = protocol::decode_command_header(prefix.subspan(4));
        if (!envelope || envelope->type != protocol::CommandType::heartbeat ||
            envelope->payload_bytes < kAuthenticatedOverhead + kMessageHeaderBytes ||
            envelope->payload_bytes > kMaximumMessageBytes + kAuthenticatedOverhead ||
            read<std::uint32_t>(prefix, 0) != protocol::kCommandHeaderBytes + envelope->payload_bytes) return std::nullopt;
        return 4 + protocol::kCommandHeaderBytes + envelope->payload_bytes;
    }, deadline, error)) { close(); return std::nullopt; }
    const auto command = channel_.receive(error);
    const auto sender = local_side_ == Side::candidate ? Side::teacher : Side::candidate;
    auto message = command ? decode_command(*command, pinned_, sender) : std::nullopt;
    if (!message) { set_error(error, "authenticated room message rejected"); close(); }
    return message;
}
bool RoomChannel::is_open() const noexcept { return channel_.is_open() && channel_.is_aead(); }
void RoomChannel::close() noexcept { channel_.close(); native_handle_ = net::kInvalidNativeHandle; }

} // namespace nstu::contest_room
