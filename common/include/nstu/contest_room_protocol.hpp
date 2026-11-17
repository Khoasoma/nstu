#pragma once

#include "nstu/control_channel.hpp"
#include "nstu/pairing.hpp"

namespace nstu::contest_room {

inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kMaximumTeacherIdBytes = 96;
inline constexpr std::size_t kMaximumMessageBytes = 60u * 1024u;
inline constexpr std::size_t kMaximumSnapshotBytes = 48u * 1024u;
inline constexpr std::size_t kMaximumChunkDataBytes = 32768;
inline constexpr std::uint64_t kMaximumArchiveBytes = 256u * 1024u * 1024u;
inline constexpr std::size_t kMessageHeaderBytes = 24;
inline constexpr std::size_t kPrefaceHeaderBytes = 10 + security::kNonceBytes;
inline constexpr std::size_t kChunkHeaderBytes = security::kSha256Bytes + 16;

// teacher_id is the configured ASCII broker owner, never the discovery label.
struct Realm {
    std::uint16_t version = kVersion;
    std::string teacher_id;
    std::string room_id;
    std::string round_id;
    [[nodiscard]] bool operator==(const Realm&) const = default;
};

enum class Side : std::uint16_t { candidate = 1, teacher = 2 };
enum class Kind : std::uint16_t {
    join = 1, heartbeat = 2, package_ack = 3, prepared = 4,
    programming_request = 5, standings_request = 6, leave = 7,
    join_status = 8, package_offer = 9, package_chunk = 10,
    programming_reply = 11, lifecycle = 12, standings = 13,
};

// LE u16 length-prefix per field: domain NSTU_CONTEST_V1, version (LE u16),
// teacher_id, room_id, round_id, fixed authority string contest_candidate.
[[nodiscard]] std::optional<std::vector<std::byte>> encode_realm(const Realm& realm);
// Appends a u16-length-prefixed existing PairingTranscript to canonical realm;
// SHA256 is the HKDF salt, NSTU_CONTEST_V1/ECDH the info. Only this fresh room
// PSK may enter a separately owned room resolver, never a managed key store.
[[nodiscard]] std::optional<pairing::PairingSecrets> derive_room_secrets(
    std::span<const std::byte> shared_secret, const Realm& realm,
    const pairing::PairingTranscript& transcript);

struct Preface { Side side = Side::candidate; security::Nonce nonce{}; };
// NSCP, LE u16 version/side/realm length, nonce[32], canonical realm bytes.
// Teacher replies echo the candidate nonce before any generic handshake.
[[nodiscard]] std::vector<std::byte> encode_preface(
    const Realm& realm, Side side, const security::Nonce& nonce);
[[nodiscard]] std::optional<Preface> decode_preface(
    std::span<const std::byte> wire, const Realm& pinned, Side expected_side,
    std::optional<security::Nonce> expected_nonce = std::nullopt);
[[nodiscard]] bool send_preface(net::TcpSocket& socket, const Realm& realm,
    Side side, const security::Nonce& nonce,
    std::chrono::steady_clock::time_point deadline, std::string* error = nullptr);
[[nodiscard]] std::optional<Preface> receive_preface(net::TcpSocket& socket,
    const Realm& pinned, Side expected_side,
    std::chrono::steady_clock::time_point deadline,
    std::optional<security::Nonce> expected_nonce = std::nullopt,
    std::string* error = nullptr);

struct Message { Kind kind = Kind::heartbeat; std::uint64_t request_id = 0; std::vector<std::byte> payload; };
[[nodiscard]] bool allowed(Side sender, Kind kind) noexcept;
// NSCM, LE u16 version/side/kind/realm length, u32 body length, u64 request ID,
// canonical realm, body. No trailing bytes. Each full message is <=60 KiB.
// Heartbeat/standings request/leave are empty; join<=256, join status<=4096;
// offer/ACK=nonzero digest[32]+LE u64 size/next offset; prepared=digest[32];
// chunk is below; source uses existing programming::decode_request with no
// workstation policy assertion; reply/standings<=48KiB, lifecycle<=256.
// Reply/standings/lifecycle content still requires typed consumer validation.
[[nodiscard]] std::vector<std::byte> encode_message(const Realm& realm, Side sender, const Message& message);
[[nodiscard]] std::optional<Message> decode_message(
    std::span<const std::byte> wire, const Realm& pinned, Side expected_sender);
// The only permitted outer carrier is heartbeat; its authenticated request ID
// must agree with the inner ID. All managed command types are rejected.
[[nodiscard]] std::optional<Message> decode_command(
    const control::AuthenticatedCommand& command, const Realm& pinned, Side expected_sender);

struct PackageChunk {
    security::Sha256Digest digest{};
    std::uint64_t offset = 0;
    std::uint64_t total_bytes = 0;
    std::vector<std::byte> data;
};
// digest[32], LE u64 offset/total bytes, 1..32768 data bytes; archive<=256MiB.
[[nodiscard]] std::vector<std::byte> encode_chunk(const PackageChunk& chunk);
[[nodiscard]] std::optional<PackageChunk> decode_chunk(std::span<const std::byte> wire);

// One owner at a time, including construction/send/receive/close; no locks or
// management dispatch. Call only after contest preface and both-screen SAS.
// Local candidate maps to existing client crypto direction, teacher to server.
class RoomChannel {
public:
    RoomChannel(net::TcpSocket socket, control::AuthenticatedSession&& session,
                Realm pinned, Side local_side);
    RoomChannel(RoomChannel&&) noexcept = default;
    RoomChannel& operator=(RoomChannel&&) noexcept = default;
    RoomChannel(const RoomChannel&) = delete;
    RoomChannel& operator=(const RoomChannel&) = delete;
    [[nodiscard]] bool send(const Message& message,
        std::chrono::steady_clock::time_point deadline, std::string* error = nullptr);
    [[nodiscard]] std::optional<Message> receive(
        std::chrono::steady_clock::time_point deadline, std::string* error = nullptr);
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;
private:
    std::uintptr_t native_handle_;
    control::AuthenticatedControlChannel channel_;
    Realm pinned_;
    Side local_side_;
};

} // namespace nstu::contest_room
