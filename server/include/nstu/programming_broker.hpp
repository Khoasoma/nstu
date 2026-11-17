#pragma once

#include "nstu/programming.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nstu::server {

struct ProgrammingBrokerConfig {
    std::string url;
    std::string token;
};

enum class RoomPublicationPolicy { teacher_only, live };
enum class RoomPhase { draft, joining, prepared, running, closed };
enum class StandingsFreshness { current, pending, unavailable };

struct ImmutableRoomState {
    std::string room_id;
    std::string round_id;
    std::string owner_id;
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::Sha256Digest grading_digest{};
    std::string runtime_identity;
    RoomPublicationPolicy publication_policy = RoomPublicationPolicy::teacher_only;
    std::uint32_t duration_seconds = 0;
};

struct RoomMember {
    exam::StateRequest context;
    std::string display_name;
    bool prepared = false;
    std::uint64_t member_version = 0;
};

struct RoomState {
    ImmutableRoomState immutable;
    RoomPhase phase = RoomPhase::draft;
    std::uint64_t revision = 0;
    std::uint64_t server_start = 0;
    std::uint64_t deadline = 0;
    std::uint64_t server_time = 0;
    std::vector<RoomMember> members;
};

struct StandingsRow {
    std::uint32_t rank = 0;
    std::string display_name;
    std::uint32_t solved = 0;
    std::uint64_t penalty_minutes = 0;
};

struct StandingsSnapshot {
    std::string room_id;
    std::string round_id;
    std::uint64_t revision = 0;
    std::uint64_t generated_time = 0;
    StandingsFreshness freshness = StandingsFreshness::unavailable;
    std::vector<StandingsRow> rows;
};

[[nodiscard]] ProgrammingBrokerConfig programming_broker_environment();
[[nodiscard]] bool validate_programming_broker_config(const ProgrammingBrokerConfig& config);

class ProgrammingBroker {
public:
    using Transport = std::function<std::optional<std::string>(std::string_view,
                                                              std::string_view)>;
    using Completion = std::function<void(std::string)>;
    explicit ProgrammingBroker(ProgrammingBrokerConfig config, Transport transport = {});
    ~ProgrammingBroker();
    ProgrammingBroker(const ProgrammingBroker&) = delete;
    ProgrammingBroker& operator=(const ProgrammingBroker&) = delete;
    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] bool submit(exam::programming::Request request, Completion completion,
                              bool recovery_only = false);
    [[nodiscard]] bool prepare_session(const exam::StateRequest& context,
                                      std::string_view mode, std::string_view grading_digest);
    [[nodiscard]] bool ready(const exam::StateRequest& context);
    [[nodiscard]] std::optional<exam::programming::LifecycleReply> begin_session(
        const exam::StateRequest& context, std::string_view request_id,
        std::uint32_t duration_seconds);
    [[nodiscard]] bool stop_session(const exam::StateRequest& context,
                                    std::string_view request_id);
    // Operator-only calls: invoke from a worker, outside UI/control-plane locks.
    [[nodiscard]] bool register_package(std::string_view package_id,
        const security::Sha256Digest& package_digest, std::string_view manifest_json);
    [[nodiscard]] bool register_grading(std::string_view package_id,
        const security::Sha256Digest& package_digest,
        const security::Sha256Digest& grading_digest, std::string_view problems_json);
    [[nodiscard]] std::optional<RoomState> create_room(const ImmutableRoomState& expected);
    [[nodiscard]] std::optional<RoomState> room_state(const ImmutableRoomState& expected);
    [[nodiscard]] std::optional<RoomState> approve_member(const RoomState& expected,
        const exam::StateRequest& context, std::string_view display_name);
    [[nodiscard]] std::optional<RoomState> remove_member(const RoomState& expected,
        const exam::StateRequest& context, std::uint64_t member_version);
    [[nodiscard]] std::optional<RoomState> prepare_member(const RoomState& expected,
        const exam::StateRequest& context, std::uint64_t member_version);
    [[nodiscard]] std::optional<RoomState> begin_room(const RoomState& expected,
        std::string_view request_id);
    [[nodiscard]] std::optional<RoomState> stop_room(const RoomState& expected,
        std::string_view request_id);
    [[nodiscard]] std::optional<StandingsSnapshot> room_standings(const ImmutableRoomState& expected);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
