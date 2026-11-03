#pragma once

#include "nstu/client_registry.hpp"
#include "nstu/operator_role.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace nstu::server {

class ServerControlPlane;

enum class ClassDisplayMode : int {
    grid = 0,
    list = 1,
};

enum class RoomFilter : int {
    all = 0,
    online = 1,
    attention = 2,
    locked = 3,
    offline = 4,
    help = 5,
};

enum class BulkActionKind : int {
    lock = 0,
    unlock = 1,
    refresh_snapshots = 2,
    acknowledge_help = 3,
    launch_url = 4,
    reboot = 5,
    shutdown = 6,
    wake_on_lan = 7,
    distribute_file = 8,
};

struct RoomCounts {
    std::size_t online = 0;
    std::size_t attention = 0;
    std::size_t locked = 0;
    std::size_t offline = 0;
    std::size_t help = 0;
};

struct BulkActionTargetSummary {
    std::size_t total_selected = 0;
    std::size_t eligible_online = 0;
    std::size_t skipped_offline = 0;
};

struct BulkActionResult {
    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::vector<std::string> error_messages;
};

[[nodiscard]] RoomCounts count_room_statuses(
    const std::vector<ClientRecord>& clients) noexcept;

[[nodiscard]] bool client_matches_filter(
    const ClientRecord& client,
    RoomFilter filter,
    bool show_offline,
    std::string_view query) noexcept;

[[nodiscard]] std::vector<const ClientRecord*> filter_clients(
    const std::vector<ClientRecord>& clients,
    RoomFilter filter,
    bool show_offline,
    std::string_view query);

[[nodiscard]] std::unordered_set<std::uint64_t> select_all_matching(
    const std::vector<const ClientRecord*>& visible_clients);

void clear_selection(std::unordered_set<std::uint64_t>& selection) noexcept;

[[nodiscard]] bool is_selected(
    const std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id) noexcept;

void set_selected(
    std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id,
    bool selected);

void toggle_selected(
    std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id);

[[nodiscard]] BulkActionTargetSummary evaluate_bulk_targets(
    const std::vector<ClientRecord>& clients,
    const std::unordered_set<std::uint64_t>& selected_ids) noexcept;

[[nodiscard]] BulkActionResult execute_bulk_action(
    ServerControlPlane& control_plane,
    const std::vector<ClientRecord>& clients,
    const std::unordered_set<std::uint64_t>& selected_ids,
    BulkActionKind action,
    std::uint16_t snapshot_interval_seconds,
    std::string_view extra_payload = {},
    OperatorRole caller_role = OperatorRole::technician);

[[nodiscard]] std::string format_bulk_confirmation_title(
    BulkActionKind action,
    bool vietnamese);

[[nodiscard]] std::string format_bulk_confirmation_message(
    BulkActionKind action,
    std::size_t target_count,
    std::string_view room_name,
    bool vietnamese);

[[nodiscard]] std::string format_bulk_action_result_status(
    BulkActionKind action,
    const BulkActionResult& result,
    bool vietnamese);

[[nodiscard]] bool is_client_stale(
    const ClientRecord& client,
    std::chrono::steady_clock::time_point now,
    std::chrono::seconds threshold = std::chrono::seconds{30}) noexcept;

[[nodiscard]] std::chrono::seconds client_age_seconds(
    const ClientRecord& client,
    std::chrono::steady_clock::time_point now) noexcept;

[[nodiscard]] std::string format_relative_age(
    std::chrono::seconds age,
    bool is_vietnamese = false);

} // namespace nstu::server

