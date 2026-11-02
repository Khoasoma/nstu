#include "nstu/classroom_roster.hpp"
#include "nstu/control_plane.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace nstu::server {

namespace {

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return result;
}

} // namespace

RoomCounts count_room_statuses(const std::vector<ClientRecord>& clients) noexcept {
    RoomCounts counts;
    for (const auto& client : clients) {
        switch (client.status) {
        case ClientStatus::online:
            ++counts.online;
            break;
        case ClientStatus::connecting:
        case ClientStatus::degraded:
            ++counts.attention;
            break;
        case ClientStatus::locked:
            ++counts.locked;
            break;
        case ClientStatus::offline:
            ++counts.offline;
            break;
        }
    }
    return counts;
}

bool client_matches_filter(
    const ClientRecord& client,
    RoomFilter filter,
    bool show_offline,
    std::string_view query) noexcept {
    if (!show_offline && client.status == ClientStatus::offline) {
        return false;
    }
    switch (filter) {
    case RoomFilter::all:
        break;
    case RoomFilter::online:
        if (client.status != ClientStatus::online) {
            return false;
        }
        break;
    case RoomFilter::attention:
        if (client.status != ClientStatus::connecting &&
            client.status != ClientStatus::degraded) {
            return false;
        }
        break;
    case RoomFilter::locked:
        if (client.status != ClientStatus::locked) {
            return false;
        }
        break;
    case RoomFilter::offline:
        if (client.status != ClientStatus::offline) {
            return false;
        }
        break;
    }
    if (query.empty()) {
        return true;
    }
    const std::string lower_query = lower_ascii(query);
    return lower_ascii(client.hostname).find(lower_query) != std::string::npos ||
           lower_ascii(client.address).find(lower_query) != std::string::npos;
}

std::vector<const ClientRecord*> filter_clients(
    const std::vector<ClientRecord>& clients,
    RoomFilter filter,
    bool show_offline,
    std::string_view query) {
    std::vector<const ClientRecord*> visible;
    visible.reserve(clients.size());
    for (const auto& client : clients) {
        if (client_matches_filter(client, filter, show_offline, query)) {
            visible.push_back(&client);
        }
    }
    return visible;
}

std::unordered_set<std::uint64_t> select_all_matching(
    const std::vector<const ClientRecord*>& visible_clients) {
    std::unordered_set<std::uint64_t> selected;
    selected.reserve(visible_clients.size());
    for (const auto* client : visible_clients) {
        if (client != nullptr) {
            selected.insert(client->id);
        }
    }
    return selected;
}

void clear_selection(std::unordered_set<std::uint64_t>& selection) noexcept {
    selection.clear();
}

bool is_selected(
    const std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id) noexcept {
    return selection.find(client_id) != selection.end();
}

void set_selected(
    std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id,
    bool selected) {
    if (selected) {
        selection.insert(client_id);
    } else {
        selection.erase(client_id);
    }
}

void toggle_selected(
    std::unordered_set<std::uint64_t>& selection,
    std::uint64_t client_id) {
    const auto iter = selection.find(client_id);
    if (iter == selection.end()) {
        selection.insert(client_id);
    } else {
        selection.erase(iter);
    }
}

BulkActionTargetSummary evaluate_bulk_targets(
    const std::vector<ClientRecord>& clients,
    const std::unordered_set<std::uint64_t>& selected_ids) noexcept {
    BulkActionTargetSummary summary;
    for (const auto& client : clients) {
        if (selected_ids.find(client.id) != selected_ids.end()) {
            ++summary.total_selected;
            if (client.status != ClientStatus::offline) {
                ++summary.eligible_online;
            } else {
                ++summary.skipped_offline;
            }
        }
    }
    return summary;
}

BulkActionResult execute_bulk_action(
    ServerControlPlane& control_plane,
    const std::vector<ClientRecord>& clients,
    const std::unordered_set<std::uint64_t>& selected_ids,
    BulkActionKind action,
    std::uint16_t snapshot_interval_seconds) {
    BulkActionResult result;
    for (const auto& client : clients) {
        if (selected_ids.find(client.id) == selected_ids.end()) {
            continue;
        }
        if (client.status == ClientStatus::offline) {
            continue;
        }
        ++result.attempted;
        std::string error;
        bool ok = false;
        switch (action) {
        case BulkActionKind::lock:
            ok = control_plane.set_locked(client.id, true, &error);
            break;
        case BulkActionKind::unlock:
            ok = control_plane.set_locked(client.id, false, &error);
            break;
        case BulkActionKind::refresh_snapshots:
            ok = control_plane.set_snapshots(client.id, true, snapshot_interval_seconds, &error);
            break;
        }
        if (ok) {
            ++result.succeeded;
        } else {
            ++result.failed;
            if (error.empty()) {
                error = "operation failed";
            }
            result.error_messages.push_back(client.hostname + ": " + error);
        }
    }
    return result;
}

std::string format_bulk_confirmation_title(BulkActionKind action, bool vietnamese) {
    switch (action) {
    case BulkActionKind::lock:
        return vietnamese ? "Xác nhận khóa hàng loạt" : "Confirm Bulk Lock";
    case BulkActionKind::unlock:
        return vietnamese ? "Xác nhận mở khóa hàng loạt" : "Confirm Bulk Unlock";
    case BulkActionKind::refresh_snapshots:
        return vietnamese ? "Xác nhận làm mới ảnh hàng loạt" : "Confirm Bulk Snapshot Refresh";
    }
    return "Confirm Bulk Action";
}

std::string format_bulk_confirmation_message(
    BulkActionKind action,
    std::size_t target_count,
    std::string_view room_name,
    bool vietnamese) {
    const std::string room_label = room_name.empty() ? (vietnamese ? "mặc định" : "default") : std::string(room_name);
    std::ostringstream stream;
    if (vietnamese) {
        switch (action) {
        case BulkActionKind::lock:
            stream << "Bạn có chắc chắn muốn KHÓA " << target_count << " máy tính đang chọn trong phòng '" << room_label << "' không?";
            break;
        case BulkActionKind::unlock:
            stream << "Bạn có chắc chắn muốn MỞ KHÓA " << target_count << " máy tính đang chọn trong phòng '" << room_label << "' không?";
            break;
        case BulkActionKind::refresh_snapshots:
            stream << "Bạn có chắc chắn muốn làm mới ảnh chụp cho " << target_count << " máy tính đang chọn trong phòng '" << room_label << "' không?";
            break;
        }
    } else {
        switch (action) {
        case BulkActionKind::lock:
            stream << "Are you sure you want to LOCK " << target_count << " selected computer(s) in room '" << room_label << "'?";
            break;
        case BulkActionKind::unlock:
            stream << "Are you sure you want to UNLOCK " << target_count << " selected computer(s) in room '" << room_label << "'?";
            break;
        case BulkActionKind::refresh_snapshots:
            stream << "Are you sure you want to REFRESH snapshots for " << target_count << " selected computer(s) in room '" << room_label << "'?";
            break;
        }
    }
    return stream.str();
}

std::string format_bulk_action_result_status(
    BulkActionKind action,
    const BulkActionResult& result,
    bool vietnamese) {
    std::ostringstream stream;
    if (vietnamese) {
        const char* action_str = "thao tác";
        switch (action) {
        case BulkActionKind::lock: action_str = "khóa"; break;
        case BulkActionKind::unlock: action_str = "mở khóa"; break;
        case BulkActionKind::refresh_snapshots: action_str = "làm mới ảnh chụp"; break;
        }
        stream << "Hoàn tất " << action_str << " hàng loạt: "
               << result.succeeded << " thành công, "
               << result.failed << " thất bại.";
    } else {
        const char* action_str = "action";
        switch (action) {
        case BulkActionKind::lock: action_str = "lock"; break;
        case BulkActionKind::unlock: action_str = "unlock"; break;
        case BulkActionKind::refresh_snapshots: action_str = "snapshot refresh"; break;
        }
        stream << "Bulk " << action_str << " completed: "
               << result.succeeded << " succeeded, "
               << result.failed << " failed.";
    }
    return stream.str();
}

} // namespace nstu::server

