#pragma once

// Fixed-width wire contracts for the gateway and exam synchronization paths.
// These structs describe byte layout only; callers must still validate fields,
// enforce limits, and convert multi-byte integers to/from little-endian before
// placing them on the network.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nstu::wire {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint8_t kProtocolMagic[4] = {'N', 'S', 'T', 'U'};
inline constexpr std::size_t kMacAddressBytes = 6;
inline constexpr std::size_t kHandshakeNonceBytes = 32;

enum class GatewayRole : std::uint8_t {
    client = 1,
    server = 2,
};

enum class GatewayMessage : std::uint8_t {
    hello = 1,
    hello_ack = 2,
};

enum class GatewayFlags : std::uint8_t {
    none = 0,
    hardware_video = 1u << 0,
    multicast = 1u << 1,
    exam_sync = 1u << 2,
};

enum class RemoteInputType : std::uint8_t {
    mouse = 1,
    keyboard = 2,
};

enum class RemoteInputFlags : std::uint8_t {
    none = 0,
    key_up = 1u << 0,
    mouse_absolute = 1u << 1,
    mouse_left_down = 1u << 2,
    mouse_left_up = 1u << 3,
    mouse_right_down = 1u << 4,
    mouse_right_up = 1u << 5,
    mouse_normalized = 1u << 6,
};

// Exactly 11 bytes. This prefix is suitable for a cheap admission check before
// allocating a connection parser or entering the authenticated handshake.
#pragma pack(push, 1)
struct ProtocolPrefix {
    std::uint8_t magic[4];
    std::uint8_t version;
    std::uint8_t role;
    std::uint8_t message;
    std::uint16_t key_id;
    std::uint16_t payload_bytes;
};

// 58-byte fixed handshake body. The MAC address is the stable deployment
// identity; the nonce and timestamp are inputs to the authenticated exchange.
struct GatewayHandshake {
    ProtocolPrefix prefix;
    std::uint8_t flags;
    std::uint8_t mac_address[kMacAddressBytes];
    std::uint8_t nonce[kHandshakeNonceBytes];
    std::uint64_t unix_time_seconds;
};

// The answer bytes immediately follow this 34-byte header. `answer_bytes`
// is bounded by the command-channel maximum and is never inferred from a
// terminator or a host-language object size.
struct ExamSyncPayload {
    std::uint32_t exam_id;
    std::uint64_t sequence;
    std::uint8_t mac_address[kMacAddressBytes];
    std::uint32_t question_id;
    std::uint8_t answer_kind;
    std::uint8_t flags;
    std::uint16_t answer_bytes;
    std::uint64_t client_time_milliseconds;
};

// 26-byte acknowledgement header. No variable data follows this structure.
struct ExamSyncAckPayload {
    std::uint32_t exam_id;
    std::uint64_t sequence;
    std::uint8_t status;
    std::uint8_t reserved;
    std::uint64_t server_time_milliseconds;
    std::uint32_t state_hash;
};

// Fixed remote-control event. Mouse coordinates are desktop pixels unless
// mouse_normalized is set, in which case they are normalized to 0..65535.
// `virtual_key` is used only for keyboard events.
struct RemoteInputPacket {
    std::uint8_t input_type;
    std::uint8_t flags;
    std::int32_t x;
    std::int32_t y;
    std::uint16_t virtual_key;
    std::uint16_t reserved;
    std::uint32_t mouse_data;
};
#pragma pack(pop)

static_assert(sizeof(ProtocolPrefix) == 11);
static_assert(sizeof(GatewayHandshake) == 58);
static_assert(sizeof(ExamSyncPayload) == 34);
static_assert(sizeof(ExamSyncAckPayload) == 26);
static_assert(sizeof(RemoteInputPacket) == 18);
static_assert(std::is_trivially_copyable_v<ProtocolPrefix>);
static_assert(std::is_trivially_copyable_v<GatewayHandshake>);
static_assert(std::is_trivially_copyable_v<ExamSyncPayload>);
static_assert(std::is_trivially_copyable_v<ExamSyncAckPayload>);
static_assert(std::is_trivially_copyable_v<RemoteInputPacket>);

inline constexpr std::size_t kProtocolPrefixBytes = sizeof(ProtocolPrefix);
inline constexpr std::size_t kGatewayHandshakeBytes = sizeof(GatewayHandshake);
inline constexpr std::size_t kExamSyncHeaderBytes = sizeof(ExamSyncPayload);
inline constexpr std::size_t kExamSyncAckBytes = sizeof(ExamSyncAckPayload);

} // namespace nstu::wire
