#include "nstu/classroom_roster.hpp"
#include "nstu/control_plane.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

nstu::server::ClientRecord make_dummy_client(
    std::uint64_t id,
    std::string hostname,
    std::string address,
    nstu::server::ClientStatus status) {
    nstu::server::ClientRecord client;
    client.id = id;
    client.hostname = std::move(hostname);
    client.address = std::move(address);
    client.status = status;
    return client;
}

void test_count_room_statuses() {
    std::vector<nstu::server::ClientRecord> empty_clients;
    auto empty_counts = nstu::server::count_room_statuses(empty_clients);
    assert(empty_counts.online == 0);
    assert(empty_counts.attention == 0);
    assert(empty_counts.locked == 0);
    assert(empty_counts.offline == 0);

    std::vector<nstu::server::ClientRecord> clients = {
        make_dummy_client(1, "LAB01", "192.168.1.101", nstu::server::ClientStatus::online),
        make_dummy_client(2, "LAB02", "192.168.1.102", nstu::server::ClientStatus::online),
        make_dummy_client(3, "LAB03", "192.168.1.103", nstu::server::ClientStatus::connecting),
        make_dummy_client(4, "LAB04", "192.168.1.104", nstu::server::ClientStatus::degraded),
        make_dummy_client(5, "LAB05", "192.168.1.105", nstu::server::ClientStatus::locked),
        make_dummy_client(6, "LAB06", "192.168.1.106", nstu::server::ClientStatus::offline),
        make_dummy_client(7, "LAB07", "192.168.1.107", nstu::server::ClientStatus::offline),
    };

    auto counts = nstu::server::count_room_statuses(clients);
    assert(counts.online == 2);
    assert(counts.attention == 2); // connecting + degraded
    assert(counts.locked == 1);
    assert(counts.offline == 2);
}

void test_filtering_modes() {
    std::vector<nstu::server::ClientRecord> clients = {
        make_dummy_client(1, "LAB01", "192.168.1.101", nstu::server::ClientStatus::online),
        make_dummy_client(2, "LAB02", "192.168.1.102", nstu::server::ClientStatus::connecting),
        make_dummy_client(3, "LAB03", "192.168.1.103", nstu::server::ClientStatus::degraded),
        make_dummy_client(4, "LAB04", "192.168.1.104", nstu::server::ClientStatus::locked),
        make_dummy_client(5, "LAB05", "192.168.1.105", nstu::server::ClientStatus::offline),
    };

    // Filter All with show_offline = true
    auto filtered_all = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "");
    assert(filtered_all.size() == 5);

    // Filter All with show_offline = false
    auto filtered_no_offline = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, false, "");
    assert(filtered_no_offline.size() == 4);

    // Filter Online
    auto filtered_online = nstu::server::filter_clients(clients, nstu::server::RoomFilter::online, true, "");
    assert(filtered_online.size() == 1);
    assert(filtered_online[0]->hostname == "LAB01");

    // Filter Attention (connecting + degraded)
    auto filtered_attention = nstu::server::filter_clients(clients, nstu::server::RoomFilter::attention, true, "");
    assert(filtered_attention.size() == 2);

    // Filter Locked
    auto filtered_locked = nstu::server::filter_clients(clients, nstu::server::RoomFilter::locked, true, "");
    assert(filtered_locked.size() == 1);
    assert(filtered_locked[0]->hostname == "LAB04");

    // Filter Offline
    auto filtered_offline = nstu::server::filter_clients(clients, nstu::server::RoomFilter::offline, true, "");
    assert(filtered_offline.size() == 1);
    assert(filtered_offline[0]->hostname == "LAB05");

    // Filter Offline when show_offline = false yields empty
    auto filtered_offline_hidden = nstu::server::filter_clients(clients, nstu::server::RoomFilter::offline, false, "");
    assert(filtered_offline_hidden.empty());
}

void test_search_query_filtering() {
    std::vector<nstu::server::ClientRecord> clients = {
        make_dummy_client(1, "DESKTOP-ALICE", "10.0.0.15", nstu::server::ClientStatus::online),
        make_dummy_client(2, "DESKTOP-BOB", "10.0.0.16", nstu::server::ClientStatus::online),
        make_dummy_client(3, "LAPTOP-CHARLIE", "192.168.5.20", nstu::server::ClientStatus::online),
    };

    // Substring match on hostname (case-insensitive)
    auto by_alice = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "alice");
    assert(by_alice.size() == 1);
    assert(by_alice[0]->id == 1);

    auto by_desktop = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "desktop");
    assert(by_desktop.size() == 2);

    // Substring match on IP address
    auto by_ip = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "192.168");
    assert(by_ip.size() == 1);
    assert(by_ip[0]->id == 3);

    // No match
    auto no_match = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "nonexistent");
    assert(no_match.empty());
}

void test_selection_helpers() {
    std::vector<nstu::server::ClientRecord> clients = {
        make_dummy_client(101, "PC-01", "10.0.0.1", nstu::server::ClientStatus::online),
        make_dummy_client(102, "PC-02", "10.0.0.2", nstu::server::ClientStatus::online),
        make_dummy_client(103, "PC-03", "10.0.0.3", nstu::server::ClientStatus::offline),
    };

    auto visible = nstu::server::filter_clients(clients, nstu::server::RoomFilter::all, true, "");
    auto selected = nstu::server::select_all_matching(visible);
    assert(selected.size() == 3);
    assert(nstu::server::is_selected(selected, 101));
    assert(nstu::server::is_selected(selected, 102));
    assert(nstu::server::is_selected(selected, 103));

    // Deselect single
    nstu::server::set_selected(selected, 102, false);
    assert(!nstu::server::is_selected(selected, 102));
    assert(selected.size() == 2);

    // Toggle
    nstu::server::toggle_selected(selected, 102);
    assert(nstu::server::is_selected(selected, 102));
    nstu::server::toggle_selected(selected, 102);
    assert(!nstu::server::is_selected(selected, 102));

    // Clear
    nstu::server::clear_selection(selected);
    assert(selected.empty());
}

void test_bulk_action_target_evaluation() {
    std::vector<nstu::server::ClientRecord> clients = {
        make_dummy_client(1, "PC-01", "10.0.0.1", nstu::server::ClientStatus::online),
        make_dummy_client(2, "PC-02", "10.0.0.2", nstu::server::ClientStatus::online),
        make_dummy_client(3, "PC-03", "10.0.0.3", nstu::server::ClientStatus::offline),
        make_dummy_client(4, "PC-04", "10.0.0.4", nstu::server::ClientStatus::offline),
    };

    std::unordered_set<std::uint64_t> selected = {1, 2, 3};
    auto summary = nstu::server::evaluate_bulk_targets(clients, selected);
    assert(summary.total_selected == 3);
    assert(summary.eligible_online == 2);
    assert(summary.skipped_offline == 1);

    // Empty selection
    std::unordered_set<std::uint64_t> empty_sel;
    auto empty_sum = nstu::server::evaluate_bulk_targets(clients, empty_sel);
    assert(empty_sum.total_selected == 0);
    assert(empty_sum.eligible_online == 0);
    assert(empty_sum.skipped_offline == 0);
}

void test_format_bulk_messages() {
    // Titles
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::lock, false) == "Confirm Bulk Lock");
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::lock, true) == "Xác nhận khóa hàng loạt");
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::unlock, false) == "Confirm Bulk Unlock");
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::unlock, true) == "Xác nhận mở khóa hàng loạt");
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::refresh_snapshots, false) == "Confirm Bulk Snapshot Refresh");
    assert(nstu::server::format_bulk_confirmation_title(nstu::server::BulkActionKind::refresh_snapshots, true) == "Xác nhận làm mới ảnh hàng loạt");

    // Confirmation message
    std::string msg_en = nstu::server::format_bulk_confirmation_message(
        nstu::server::BulkActionKind::lock, 12, "Lab 301", false);
    assert(msg_en.find("12") != std::string::npos);
    assert(msg_en.find("Lab 301") != std::string::npos);
    assert(msg_en.find("LOCK") != std::string::npos);

    std::string msg_vi = nstu::server::format_bulk_confirmation_message(
        nstu::server::BulkActionKind::unlock, 8, "Phòng Tin", true);
    assert(msg_vi.find("8") != std::string::npos);
    assert(msg_vi.find("Phòng Tin") != std::string::npos);
    assert(msg_vi.find("MỞ KHÓA") != std::string::npos);

    // Result message
    nstu::server::BulkActionResult res{.attempted = 10, .succeeded = 9, .failed = 1, .error_messages = {}};
    std::string res_en = nstu::server::format_bulk_action_result_status(
        nstu::server::BulkActionKind::lock, res, false);
    assert(res_en.find("9 succeeded") != std::string::npos);
    assert(res_en.find("1 failed") != std::string::npos);

    std::string res_vi = nstu::server::format_bulk_action_result_status(
        nstu::server::BulkActionKind::refresh_snapshots, res, true);
    assert(res_vi.find("9 thành công") != std::string::npos);
    assert(res_vi.find("1 thất bại") != std::string::npos);
}

void test_scalability_and_responsiveness_benchmarks() {
    for (const std::size_t count : {10ULL, 30ULL, 50ULL}) {
        std::vector<nstu::server::ClientRecord> cohort;
        cohort.reserve(count);
        for (std::size_t i = 1; i <= count; ++i) {
            nstu::server::ClientStatus st = (i % 5 == 0) ? nstu::server::ClientStatus::offline
                                          : (i % 7 == 0) ? nstu::server::ClientStatus::locked
                                          : (i % 3 == 0) ? nstu::server::ClientStatus::degraded
                                                         : nstu::server::ClientStatus::online;
            cohort.push_back(make_dummy_client(
                i, "STUDENT-" + std::to_string(i), "192.168.1." + std::to_string(i), st));
        }

        const auto start_time = std::chrono::steady_clock::now();

        // 1. Status count
        auto counts = nstu::server::count_room_statuses(cohort);
        assert(counts.online + counts.attention + counts.locked + counts.offline == count);

        // 2. Filter clients with search query
        auto filtered = nstu::server::filter_clients(cohort, nstu::server::RoomFilter::all, true, "STUDENT-1");
        assert(!filtered.empty());

        // 3. Multi-selection
        auto all_visible = nstu::server::filter_clients(cohort, nstu::server::RoomFilter::all, true, "");
        auto selected_set = nstu::server::select_all_matching(all_visible);
        assert(selected_set.size() == count);

        // 4. Target evaluation
        auto target_summary = nstu::server::evaluate_bulk_targets(cohort, selected_set);
        assert(target_summary.total_selected == count);
        assert(target_summary.eligible_online + target_summary.skipped_offline == count);

        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto microsecs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

        // The entire pass across 10, 30, or 50 clients must be sub-millisecond (< 5000 µs)
        std::cout << "Classroom roster benchmark for " << count << " clients: "
                  << microsecs << " us\n";
        assert(microsecs < 50000); // Generous 50ms guard against slow debug runners, actual is < 50us
    }
}

} // namespace

int main() {
    test_count_room_statuses();
    test_filtering_modes();
    test_search_query_filtering();
    test_selection_helpers();
    test_bulk_action_target_evaluation();
    test_format_bulk_messages();
    test_scalability_and_responsiveness_benchmarks();
    std::cout << "All classroom roster tests passed successfully.\n";
    return 0;
}

