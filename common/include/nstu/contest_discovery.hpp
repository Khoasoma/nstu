#pragma once

#include "nstu/auth.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::contest_discovery {

inline constexpr std::uint16_t kDiscoveryVersion = 2;
inline constexpr std::uint16_t kRoomProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 47002;
inline constexpr std::size_t kMaximumDatagramBytes = 512;
inline constexpr std::size_t kMaximumLabelBytes = 64;
inline constexpr std::size_t kMaximumTeacherIdBytes = 96;
inline constexpr std::size_t kMaximumCandidates = 32;
inline constexpr std::size_t kProbeBytes = 8 + security::kNonceBytes;
inline constexpr std::size_t kReplyHeaderBytes = kProbeBytes + 12 + 64;
inline constexpr std::chrono::milliseconds kProbeBudget{1200};
inline constexpr std::chrono::seconds kHintLifetime{10};

struct Probe {
    security::Nonce nonce{};
};

// join_closed advertises an active round after Begin. A closed room disables
// its beacon entirely with set_beacon(nullopt).
enum class JoinState : std::uint16_t { open = 1, join_closed = 2 };

struct RoomAdvertisement {
    std::string room_id;
    std::string round_id;
    std::string title;
    std::string teacher_label;
    JoinState join_state = JoinState::open;
    std::uint16_t protocol_version = kRoomProtocolVersion;
    std::uint16_t control_port = kDefaultPort;
    // Untrusted realm hint; two-screen SAS still authenticates this identity.
    std::string teacher_id;

    [[nodiscard]] bool operator==(const RoomAdvertisement&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_probe(const Probe& probe);
[[nodiscard]] std::optional<Probe> decode_probe(std::span<const std::byte> wire);
[[nodiscard]] std::vector<std::byte> encode_reply(
    const Probe& probe, const RoomAdvertisement& room);
[[nodiscard]] std::optional<RoomAdvertisement> decode_reply(
    std::span<const std::byte> wire, const Probe& probe);

struct Endpoint {
    std::string address;
    std::uint16_t port = kDefaultPort;
};

// All fields remain spoofable hints. Only subsequent two-screen SAS and
// authenticated room membership can authorize a connection.
struct RoomCandidate {
    std::string address;
    RoomAdvertisement room;
    std::chrono::steady_clock::time_point observed_at;
};

class CandidateSet {
public:
    CandidateSet(Probe probe, std::chrono::steady_clock::time_point started,
                 std::size_t maximum_candidates = kMaximumCandidates);
    [[nodiscard]] bool accept(std::span<const std::byte> wire,
                              const std::string& source_address,
                              std::chrono::steady_clock::time_point now);
    [[nodiscard]] const std::vector<RoomCandidate>& candidates(
        std::chrono::steady_clock::time_point now);

private:
    Probe probe_;
    std::chrono::steady_clock::time_point started_;
    std::size_t limit_;
    std::vector<RoomCandidate> candidates_;
};

struct DiscoveryOptions {
    std::uint16_t discovery_port = kDefaultPort;
    std::chrono::milliseconds timeout = kProbeBudget;
    std::size_t maximum_candidates = kMaximumCandidates;
    // Empty enumerates directed broadcasts of active IPv4 LAN adapters only.
    // Explicit literal IPv4:port targets are the manual fallback and test path.
    std::vector<Endpoint> manual_targets;
};

[[nodiscard]] std::vector<RoomCandidate> discover_rooms(
    const DiscoveryOptions& options = {}, std::string* error = nullptr);

class RoomDiscoveryResponder {
public:
    RoomDiscoveryResponder();
    ~RoomDiscoveryResponder();
    RoomDiscoveryResponder(const RoomDiscoveryResponder&) = delete;
    RoomDiscoveryResponder& operator=(const RoomDiscoveryResponder&) = delete;

    // The owner serializes start/stop. Port zero requests an ephemeral test port.
    [[nodiscard]] bool start(std::uint16_t port = kDefaultPort,
                             std::string* error = nullptr);
    void stop() noexcept;
    // Thread-safe; invalid metadata disables the beacon and returns false.
    [[nodiscard]] bool set_beacon(std::optional<RoomAdvertisement> room,
                                  std::string* error = nullptr);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::uint64_t beacons_sent() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::contest_discovery
