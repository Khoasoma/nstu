#pragma once

#include "nstu/contest_discovery.hpp"
#include "nstu/contest_package.hpp"
#include "nstu/contest_room_protocol.hpp"
#include "nstu/programming_broker.hpp"

#include <filesystem>

namespace nstu::server {

inline constexpr std::size_t kMaximumRoomMembers = 64;
inline constexpr std::size_t kMaximumRoomHistory = 256;
inline constexpr std::size_t kMaximumRoomOperations = 128;
inline constexpr std::size_t kMaximumPeerMessages = 16;
inline constexpr std::size_t kMaximumPeerQueueBytes = 256u * 1024u;

// Only deterministic tests inject this memory fixture. Production leaves it
// empty and uses current-user DPAPI, protected owner ACLs and flushed replace.
struct RoomMemoryPersistenceFixture {
    std::function<std::optional<std::vector<std::byte>>()> load;
    std::function<bool(std::span<const std::byte>)> save;
    // Deterministic race scheduling only, invoked outside the state mutex.
    std::function<void()> before_preparation_publication = {};
};

struct RoomPublicArchiveSource {
    std::filesystem::path archive;
    std::filesystem::path packages_root;
    // Operator-supplied copy of the hash-pinned reviewed packaging helper.
    std::filesystem::path extractor_script;
    // Deployment-owned PowerShell 7 runtime, never resolved through PATH.
    std::filesystem::path powershell_executable;
    std::string powershell_sha256;
};

// Deterministic transport tests only; this is not native archive validation.
// Requires memory_fixture as well; production supplies public_archive instead.
struct RoomMemoryArchiveFixture {
    std::vector<std::byte> archive;
};

struct ContestRoomServerConfig {
    ImmutableRoomState room;
    std::string title;
    std::string teacher_label;
    // Trusted operator configuration; must be an absolute existing directory
    // owned exclusively by this user. A protected contest-profile subdirectory
    // is created underneath it. Candidate packets never choose this path.
    std::filesystem::path state_root;
    std::uint16_t tcp_port = contest_discovery::kDefaultPort;
    std::uint16_t udp_port = contest_discovery::kDefaultPort;
    std::chrono::milliseconds pairing_timeout{30000};
    std::chrono::milliseconds peer_timeout{15000};
    std::optional<RoomMemoryPersistenceFixture> memory_fixture;
    std::optional<RoomPublicArchiveSource> public_archive;
    std::optional<RoomMemoryArchiveFixture> archive_fixture;
};

struct PendingRoomPairing {
    std::uint64_t peer_id = 0;
    std::string peer_user_uuid;
    std::string display_name;
    std::string sas;
};

struct RoomPeerStatus {
    std::uint64_t peer_id = 0;
    exam::StateRequest context;
    bool connected = false;
    bool recovery_only = false;
};

struct ContestRoomSnapshot {
    bool running = false;
    std::uint16_t tcp_port = 0;
    std::uint16_t udp_port = 0;
    std::optional<RoomState> room;
    std::vector<PendingRoomPairing> pending;
    std::vector<RoomPeerStatus> peers;
    std::string error;
};

struct RoomPublicPackagePin {
    contest_room::Realm realm;
    std::string package_id;
    security::Sha256Digest digest{};
};

class ContestRoomServer {
public:
    // broker must outlive this owner; start/stop are serialized by the owner.
    explicit ContestRoomServer(ProgrammingBroker& broker);
    ~ContestRoomServer();
    ContestRoomServer(const ContestRoomServer&) = delete;
    ContestRoomServer& operator=(const ContestRoomServer&) = delete;
    // Disk and typed broker reconciliation happen here, off the UI callback.
    [[nodiscard]] bool start(ContestRoomServerConfig config, std::string* error = nullptr);
    void stop() noexcept;
    [[nodiscard]] ContestRoomSnapshot snapshot() const;
    // These calls only validate/copy into bounded queues, never do I/O.
    [[nodiscard]] bool approve(std::uint64_t peer_id, std::string_view confirmed_sas);
    [[nodiscard]] bool reject(std::uint64_t peer_id);
    [[nodiscard]] bool begin(std::string_view request_id);
    [[nodiscard]] bool finish(std::string_view request_id);
    [[nodiscard]] bool remove(const exam::StateRequest& member);
    [[nodiscard]] bool refresh();
    [[nodiscard]] std::optional<RoomPublicPackagePin> public_package_pin() const;
    [[nodiscard]] bool queue_message(std::uint64_t peer_id, contest_room::Message message);
    // Observation only; never grants Prepared. Actual transfer verification,
    // exact bound ACK and durable typed broker preparation own that authority.
    using FinalAckHandler = std::function<void(std::uint64_t, const exam::StateRequest&,
                                               const security::Sha256Digest&)>;
    void set_final_ack_handler(FinalAckHandler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
