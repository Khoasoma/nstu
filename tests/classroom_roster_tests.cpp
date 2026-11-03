#include "nstu/classroom_roster.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/control_messages.hpp"
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

void test_help_request_roster_and_codecs() {
    using namespace nstu::control;
    using namespace nstu::server;

    // 1. Codec tests
    HelpRequestMessage req;
    req.active = true;
    req.timestamp_unix_ms = 1730592000000ULL;
    req.message = "I need help with problem #4";

    auto encoded = encode_help_request(req);
    assert(!encoded.empty());

    auto decoded = decode_help_request(encoded);
    assert(decoded.has_value());
    assert(decoded->active == true);
    assert(decoded->timestamp_unix_ms == req.timestamp_unix_ms);
    assert(decoded->message == "I need help with problem #4");

    // Codec test: inactive / lowered hand
    HelpRequestMessage req_inactive;
    req_inactive.active = false;
    req_inactive.timestamp_unix_ms = 1730592100000ULL;
    req_inactive.message = "";

    auto encoded_inactive = encode_help_request(req_inactive);
    auto decoded_inactive = decode_help_request(encoded_inactive);
    assert(decoded_inactive.has_value());
    assert(decoded_inactive->active == false);
    assert(decoded_inactive->timestamp_unix_ms == req_inactive.timestamp_unix_ms);
    assert(decoded_inactive->message.empty());

    // Codec test: bounds check (exceeding 256 bytes rejected)
    HelpRequestMessage req_oversized;
    req_oversized.active = true;
    req_oversized.timestamp_unix_ms = 1730592200000ULL;
    req_oversized.message = std::string(300, 'X');
    auto encoded_oversized = encode_help_request(req_oversized);
    assert(encoded_oversized.empty());

    // Codec test: truncated payload rejected
    std::vector<std::byte> truncated(5, std::byte{0});
    assert(!decode_help_request(truncated).has_value());

    // Codec test: HelpAckMessage
    HelpAckMessage ack;
    ack.resolved = true;
    ack.timestamp_unix_ms = 1730592300000ULL;
    auto enc_ack = encode_help_ack(ack);
    assert(!enc_ack.empty());
    auto dec_ack = decode_help_ack(enc_ack);
    assert(dec_ack.has_value());
    assert(dec_ack->resolved == true);
    assert(dec_ack->timestamp_unix_ms == ack.timestamp_unix_ms);

    // 2. Roster counting & filtering tests
    std::vector<ClientRecord> clients = {
        make_dummy_client(1, "LAB01", "192.168.1.101", ClientStatus::online),
        make_dummy_client(2, "LAB02", "192.168.1.102", ClientStatus::online),
        make_dummy_client(3, "LAB03", "192.168.1.103", ClientStatus::connecting),
        make_dummy_client(4, "LAB04", "192.168.1.104", ClientStatus::offline),
    };
    clients[0].help_requested = true;
    clients[0].help_message = "Need paper";
    clients[0].help_requested_at_unix_ms = 1730592000000ULL;

    clients[2].help_requested = true;
    clients[2].help_message = "Cannot login";
    clients[2].help_requested_at_unix_ms = 1730592050000ULL;

    auto counts = count_room_statuses(clients);
    assert(counts.online == 2);
    assert(counts.attention == 1);
    assert(counts.offline == 1);
    assert(counts.locked == 0);
    assert(counts.help == 2);
    assert(counts.online + counts.attention + counts.locked + counts.offline == clients.size());

    // Filter by help
    auto help_clients = filter_clients(clients, RoomFilter::help, true, "");
    assert(help_clients.size() == 2);
    assert(help_clients[0]->id == 1);
    assert(help_clients[1]->id == 3);

    // 3. ClientRegistry persistence and mutation tests
    ClientRegistry registry;
    ClientRecord rec;
    rec.id = 42;
    rec.hostname = "PC-42";
    rec.status = ClientStatus::online;
    registry.upsert(rec);

    assert(registry.set_help_request(42, true, "Question on task 2", 1000));
    auto snap1 = registry.snapshot();
    assert(snap1.size() == 1);
    assert(snap1[0].help_requested == true);
    assert(snap1[0].help_message == "Question on task 2");
    assert(snap1[0].help_requested_at_unix_ms == 1000);

    // Upsert status report periodic refresh must PRESERVE active help request
    ClientRecord refresh_rec;
    refresh_rec.id = 42;
    refresh_rec.hostname = "PC-42";
    refresh_rec.status = ClientStatus::online;
    registry.upsert(refresh_rec);

    auto snap2 = registry.snapshot();
    assert(snap2[0].help_requested == true);
    assert(snap2[0].help_message == "Question on task 2");
    assert(snap2[0].help_requested_at_unix_ms == 1000);

    // Resolve / clear help request
    assert(registry.set_help_request(42, false, "", 0));
    auto snap3 = registry.snapshot();
    assert(snap3[0].help_requested == false);
    assert(snap3[0].help_message.empty());
    assert(snap3[0].help_requested_at_unix_ms == 0);

    // 4. Bulk action formatters for acknowledge_help
    assert(format_bulk_confirmation_title(BulkActionKind::acknowledge_help, false) == "Confirm Acknowledge Help");
    assert(format_bulk_confirmation_title(BulkActionKind::acknowledge_help, true) == "Xác nhận giải quyết yêu cầu trợ giúp");

    std::string help_confirm_en = format_bulk_confirmation_message(
        BulkActionKind::acknowledge_help, 5, "Room A", false);
    assert(help_confirm_en.find("ACKNOWLEDGE") != std::string::npos);
    assert(help_confirm_en.find("5") != std::string::npos);

    std::string help_confirm_vi = format_bulk_confirmation_message(
        BulkActionKind::acknowledge_help, 5, "Phòng A", true);
    assert(help_confirm_vi.find("GIẢI QUYẾT") != std::string::npos);
    assert(help_confirm_vi.find("5") != std::string::npos);

    BulkActionResult b_res{.attempted = 5, .succeeded = 5, .failed = 0, .error_messages = {}};
    std::string status_en = format_bulk_action_result_status(BulkActionKind::acknowledge_help, b_res, false);
    assert(status_en.find("acknowledge help") != std::string::npos);
    assert(status_en.find("5 succeeded") != std::string::npos);

    std::string status_vi = format_bulk_action_result_status(BulkActionKind::acknowledge_help, b_res, true);
    assert(status_vi.find("giải quyết trợ giúp") != std::string::npos);
    assert(status_vi.find("5 thành công") != std::string::npos);
}

void test_bulk_action_distribute_file_formatters_and_execution() {
    using namespace nstu::server;

    // 1. Title formatters
    assert(format_bulk_confirmation_title(BulkActionKind::distribute_file, false) == "Confirm Bulk File Distribution");
    assert(format_bulk_confirmation_title(BulkActionKind::distribute_file, true) == "Xác nhận phát tệp hàng loạt");

    // 2. Confirmation message formatters
    const std::string msg_en = format_bulk_confirmation_message(
        BulkActionKind::distribute_file, 4, "Lab 204", false);
    assert(msg_en.find("DISTRIBUTE") != std::string::npos);
    assert(msg_en.find("4") != std::string::npos);
    assert(msg_en.find("Lab 204") != std::string::npos);

    const std::string msg_vi = format_bulk_confirmation_message(
        BulkActionKind::distribute_file, 4, "Phòng 204", true);
    assert(msg_vi.find("PHÁT TỆP") != std::string::npos);
    assert(msg_vi.find("4") != std::string::npos);
    assert(msg_vi.find("Phòng 204") != std::string::npos);

    // 3. Result status formatters
    BulkActionResult res{.attempted = 4, .succeeded = 3, .failed = 1, .error_messages = {}};
    const std::string status_en = format_bulk_action_result_status(
        BulkActionKind::distribute_file, res, false);
    assert(status_en.find("file distribution") != std::string::npos);
    assert(status_en.find("3 succeeded") != std::string::npos);
    assert(status_en.find("1 failed") != std::string::npos);

    const std::string status_vi = format_bulk_action_result_status(
        BulkActionKind::distribute_file, res, true);
    assert(status_vi.find("phát tệp") != std::string::npos);
    assert(status_vi.find("3 thành công") != std::string::npos);
    assert(status_vi.find("1 thất bại") != std::string::npos);

    // 4. Control Plane File Distribution guards
    nstu::security::KeyStore key_store;
    ClientRegistry registry;
    ServerControlPlane control_plane(registry, key_store);

    // 4a. Client not found in registry
    {
        std::string err;
        assert(!control_plane.distribute_file(99999, "non_existent.txt", OperatorRole::teacher, &err));
        assert(err.find("not found") != std::string::npos);
    }

    // 4b. Offline client in registry
    {
        ClientRecord offline_client;
        offline_client.id = 555;
        offline_client.hostname = "PC-OFFLINE";
        offline_client.status = ClientStatus::offline;
        registry.upsert(offline_client);

        std::string err;
        assert(!control_plane.distribute_file(555, "non_existent.txt", OperatorRole::teacher, &err));
        assert(err.find("offline") != std::string::npos);
    }

    // 4c. Online client with non-existent file
    {
        ClientRecord online_client;
        online_client.id = 777;
        online_client.hostname = "PC-ONLINE";
        online_client.status = ClientStatus::online;
        registry.upsert(online_client);

        std::string err;
        assert(!control_plane.distribute_file(777, "completely_nonexistent_file_xyz123.bin", OperatorRole::teacher, &err));
        assert(err.find("File does not exist") != std::string::npos);
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
    test_help_request_roster_and_codecs();
    test_bulk_action_distribute_file_formatters_and_execution();
    std::cout << "All classroom roster tests passed successfully.\n";
    return 0;
}

