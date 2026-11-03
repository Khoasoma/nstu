#include "nstu/classroom_roster.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/multicast.hpp"
#include "nstu/operator_role.hpp"
#include "nstu/protocol.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 11);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x60 + index);
    }
    return key;
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void test_power_action_codecs() {
    using namespace nstu::control;

    // 1. Reboot graceful (force = false)
    {
        const PowerActionMessage msg{
            .action = PowerActionKind::reboot,
            .force = false,
        };
        const auto encoded = encode_power_action(msg);
        assert(encoded.size() == 2);
        assert(encoded[0] == std::byte{1});
        assert(encoded[1] == std::byte{0});

        const auto decoded = decode_power_action(encoded);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::reboot);
        assert(!decoded->force);
    }

    // 2. Reboot forced (force = true)
    {
        const PowerActionMessage msg{
            .action = PowerActionKind::reboot,
            .force = true,
        };
        const auto encoded = encode_power_action(msg);
        assert(encoded.size() == 2);
        assert(encoded[0] == std::byte{1});
        assert(encoded[1] == std::byte{1});

        const auto decoded = decode_power_action(encoded);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::reboot);
        assert(decoded->force);
    }

    // 3. Shutdown graceful (force = false)
    {
        const PowerActionMessage msg{
            .action = PowerActionKind::shutdown,
            .force = false,
        };
        const auto encoded = encode_power_action(msg);
        assert(encoded.size() == 2);
        assert(encoded[0] == std::byte{2});
        assert(encoded[1] == std::byte{0});

        const auto decoded = decode_power_action(encoded);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::shutdown);
        assert(!decoded->force);
    }

    // 4. Shutdown forced (force = true)
    {
        const PowerActionMessage msg{
            .action = PowerActionKind::shutdown,
            .force = true,
        };
        const auto encoded = encode_power_action(msg);
        assert(encoded.size() == 2);
        assert(encoded[0] == std::byte{2});
        assert(encoded[1] == std::byte{1});

        const auto decoded = decode_power_action(encoded);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::shutdown);
        assert(decoded->force);
    }

    // 5. Invalid action kind in encoding
    {
        const PowerActionMessage invalid_msg{
            .action = static_cast<PowerActionKind>(99),
            .force = false,
        };
        const auto encoded = encode_power_action(invalid_msg);
        assert(encoded.empty());
    }

    // 6. Decode rejection: invalid sizes
    {
        // Empty
        assert(!decode_power_action({}).has_value());
        // 1 byte
        const std::vector<std::byte> one_byte{std::byte{1}};
        assert(!decode_power_action(one_byte).has_value());
        // 3 bytes (oversized)
        const std::vector<std::byte> three_bytes{std::byte{1}, std::byte{0}, std::byte{0}};
        assert(!decode_power_action(three_bytes).has_value());
        // 4 bytes
        const std::vector<std::byte> four_bytes{std::byte{2}, std::byte{1}, std::byte{0}, std::byte{0}};
        assert(!decode_power_action(four_bytes).has_value());
    }

    // 7. Decode rejection: invalid action enum
    {
        const std::vector<std::byte> zero_action{std::byte{0}, std::byte{0}};
        assert(!decode_power_action(zero_action).has_value());
        const std::vector<std::byte> invalid_action{std::byte{3}, std::byte{0}};
        assert(!decode_power_action(invalid_action).has_value());
        const std::vector<std::byte> max_action{std::byte{0xFF}, std::byte{0}};
        assert(!decode_power_action(max_action).has_value());
    }

    // 8. Decode rejection: non-zero reserved flag bits (bits 1-7)
    {
        // bit 1 set (0x02)
        const std::vector<std::byte> reserved_bit1{std::byte{1}, std::byte{0x02}};
        assert(!decode_power_action(reserved_bit1).has_value());
        // bit 7 set (0x80)
        const std::vector<std::byte> reserved_bit7{std::byte{2}, std::byte{0x80}};
        assert(!decode_power_action(reserved_bit7).has_value());
        // all bits set (0xFF)
        const std::vector<std::byte> all_bits{std::byte{1}, std::byte{0xFF}};
        assert(!decode_power_action(all_bits).has_value());
    }
}

void test_operator_role_authorization() {
    using namespace nstu::server;

    // Teacher cannot perform disruptive power maintenance
    assert(!is_action_permitted(OperatorRole::teacher, OperatorAction::reboot_client));
    assert(!is_action_permitted(OperatorRole::teacher, OperatorAction::shutdown_client));

    // Technician is authorized for power maintenance
    assert(is_action_permitted(OperatorRole::technician, OperatorAction::reboot_client));
    assert(is_action_permitted(OperatorRole::technician, OperatorAction::shutdown_client));

    // Developer is authorized for power maintenance
    assert(is_action_permitted(OperatorRole::developer, OperatorAction::reboot_client));
    assert(is_action_permitted(OperatorRole::developer, OperatorAction::shutdown_client));
}

void test_classroom_roster_power_integration() {
    using namespace nstu::server;

    // Title formatters
    assert(format_bulk_confirmation_title(BulkActionKind::reboot, false) ==
           "Confirm Bulk Reboot");
    assert(format_bulk_confirmation_title(BulkActionKind::reboot, true) ==
           "Xác nhận khởi động lại hàng loạt");
    assert(format_bulk_confirmation_title(BulkActionKind::shutdown, false) ==
           "Confirm Bulk Shutdown");
    assert(format_bulk_confirmation_title(BulkActionKind::shutdown, true) ==
           "Xác nhận tắt máy hàng loạt");

    // Message formatters
    const std::string reboot_en = format_bulk_confirmation_message(
        BulkActionKind::reboot, 12, "Lab 201", false);
    assert(reboot_en.find("REBOOT") != std::string::npos);
    assert(reboot_en.find("12") != std::string::npos);
    assert(reboot_en.find("Lab 201") != std::string::npos);
    assert(reboot_en.find("Any unsaved work may be lost.") != std::string::npos);

    const std::string reboot_vi = format_bulk_confirmation_message(
        BulkActionKind::reboot, 12, "Phòng 201", true);
    assert(reboot_vi.find("KHỞI ĐỘNG LẠI") != std::string::npos);
    assert(reboot_vi.find("12") != std::string::npos);
    assert(reboot_vi.find("Phòng 201") != std::string::npos);
    assert(reboot_vi.find("Các thay đổi chưa lưu có thể bị mất.") != std::string::npos);

    const std::string shutdown_en = format_bulk_confirmation_message(
        BulkActionKind::shutdown, 6, "Lab 202", false);
    assert(shutdown_en.find("SHUT DOWN") != std::string::npos);
    assert(shutdown_en.find("6") != std::string::npos);
    assert(shutdown_en.find("Lab 202") != std::string::npos);
    assert(shutdown_en.find("Any unsaved work may be lost.") != std::string::npos);

    const std::string shutdown_vi = format_bulk_confirmation_message(
        BulkActionKind::shutdown, 6, "Phòng 202", true);
    assert(shutdown_vi.find("TẮT") != std::string::npos);
    assert(shutdown_vi.find("6") != std::string::npos);
    assert(shutdown_vi.find("Phòng 202") != std::string::npos);
    assert(shutdown_vi.find("Các thay đổi chưa lưu có thể bị mất.") != std::string::npos);

    // Empty room defaults
    const std::string default_room_en = format_bulk_confirmation_message(
        BulkActionKind::reboot, 1, "", false);
    assert(default_room_en.find("'default'") != std::string::npos);

    const std::string default_room_vi = format_bulk_confirmation_message(
        BulkActionKind::shutdown, 1, "", true);
    assert(default_room_vi.find("'mặc định'") != std::string::npos);

    // Status formatters
    const BulkActionResult success_result{.attempted = 10, .succeeded = 10, .failed = 0, .error_messages = {}};
    const std::string status_reboot_en = format_bulk_action_result_status(
        BulkActionKind::reboot, success_result, false);
    assert(status_reboot_en.find("reboot") != std::string::npos);
    assert(status_reboot_en.find("10 succeeded") != std::string::npos);
    assert(status_reboot_en.find("0 failed") != std::string::npos);

    const std::string status_reboot_vi = format_bulk_action_result_status(
        BulkActionKind::reboot, success_result, true);
    assert(status_reboot_vi.find("khởi động lại") != std::string::npos);
    assert(status_reboot_vi.find("10 thành công") != std::string::npos);
    assert(status_reboot_vi.find("0 thất bại") != std::string::npos);

    const BulkActionResult partial_result{.attempted = 4, .succeeded = 3, .failed = 1, .error_messages = {"PC-04: offline"}};
    const std::string status_shut_en = format_bulk_action_result_status(
        BulkActionKind::shutdown, partial_result, false);
    assert(status_shut_en.find("shutdown") != std::string::npos);
    assert(status_shut_en.find("3 succeeded") != std::string::npos);
    assert(status_shut_en.find("1 failed") != std::string::npos);

    const std::string status_shut_vi = format_bulk_action_result_status(
        BulkActionKind::shutdown, partial_result, true);
    assert(status_shut_vi.find("tắt máy") != std::string::npos);
    assert(status_shut_vi.find("3 thành công") != std::string::npos);
    assert(status_shut_vi.find("1 thất bại") != std::string::npos);

    // Roster target evaluation with offline skip semantics
    std::vector<ClientRecord> roster_clients;
    ClientRecord online1;
    online1.id = 101;
    online1.hostname = "PC-101";
    online1.status = ClientStatus::online;
    roster_clients.push_back(online1);

    ClientRecord online2;
    online2.id = 102;
    online2.hostname = "PC-102";
    online2.status = ClientStatus::online;
    roster_clients.push_back(online2);

    ClientRecord offline3;
    offline3.id = 103;
    offline3.hostname = "PC-103";
    offline3.status = ClientStatus::offline;
    roster_clients.push_back(offline3);

    const std::unordered_set<std::uint64_t> selected{101, 102, 103};
    const auto target_summary = evaluate_bulk_targets(roster_clients, selected);
    assert(target_summary.total_selected == 3);
    assert(target_summary.eligible_online == 2);
    assert(target_summary.skipped_offline == 1);
}

void test_control_plane_power_action() {
    using namespace nstu::control;
    using namespace nstu::server;

    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const auto id = client_id();
    auto key = client_key();
    constexpr std::uint32_t key_id = 9;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));

    ClientRegistry registry;
    ServerControlPlane control_plane(registry, key_store);
    ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 16;
    assert(control_plane.start(std::move(config), &error));

    // Connect client
    nstu::net::TcpSocket socket;
    assert(socket.connect("127.0.0.1", control_plane.local_port(), &error));
    assert(socket.set_io_timeouts(5000, 5000, &error));

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session = client_handshake(
        socket, id, key_id, key, now, std::chrono::seconds(120), &error);
    assert(session.has_value());

    AuthenticatedControlChannel channel(std::move(socket), std::move(*session));

    // Report client status to register client in registry
    ClientStatusReport status;
    status.hostname = "LAB-PC-PWR";
    status.session_id = 1;
    status.latency_ms = 4;
    status.packet_loss_per_mille = 0;
    status.packet_loss_sample_size = 100;
    const auto status_payload = encode_status_report(status);
    assert(channel.send(nstu::protocol::CommandType::hello, 1, status_payload, &error));

    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return snapshot.size() == 1 && snapshot[0].hostname == "LAB-PC-PWR";
    }));
    const auto clients = registry.snapshot();
    assert(clients.size() == 1);
    const auto registry_id = clients[0].id;

    // 1. Unregistered client ID fails
    {
        std::string err;
        assert(!control_plane.power_action(
            999999, PowerActionKind::reboot, false, OperatorRole::technician, &err));
        assert(err.find("not authenticated") != std::string::npos);
    }

    // 2. Teacher role is unauthorized for power actions
    {
        std::string err;
        assert(!control_plane.power_action(
            registry_id, PowerActionKind::reboot, false, OperatorRole::teacher, &err));
        assert(err.find("not authorized") != std::string::npos);

        assert(!control_plane.power_action(
            registry_id, PowerActionKind::shutdown, false, OperatorRole::teacher, &err));
        assert(err.find("not authorized") != std::string::npos);
    }

    // 3. Forced power action requires Technician or Developer
    {
        std::string err;
        assert(!control_plane.power_action(
            registry_id, PowerActionKind::reboot, true, OperatorRole::teacher, &err));
        assert(err.find("not authorized") != std::string::npos);
    }

    // 4. Successful graceful reboot dispatch with Technician role
    {
        std::string err;
        assert(control_plane.power_action(
            registry_id, PowerActionKind::reboot, false, OperatorRole::technician, &err));

        const auto frame = channel.receive(&err);
        assert(frame.has_value());
        assert(frame->envelope.type == nstu::protocol::CommandType::power_action);
        const auto decoded = decode_power_action(frame->payload);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::reboot);
        assert(!decoded->force);
    }

    // 5. Successful forced shutdown dispatch with Technician role
    {
        std::string err;
        assert(control_plane.power_action(
            registry_id, PowerActionKind::shutdown, true, OperatorRole::technician, &err));

        const auto frame = channel.receive(&err);
        assert(frame.has_value());
        assert(frame->envelope.type == nstu::protocol::CommandType::power_action);
        const auto decoded = decode_power_action(frame->payload);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::shutdown);
        assert(decoded->force);
    }

    // 6. Successful forced reboot dispatch with Developer role
    {
        std::string err;
        assert(control_plane.power_action(
            registry_id, PowerActionKind::reboot, true, OperatorRole::developer, &err));

        const auto frame = channel.receive(&err);
        assert(frame.has_value());
        assert(frame->envelope.type == nstu::protocol::CommandType::power_action);
        const auto decoded = decode_power_action(frame->payload);
        assert(decoded.has_value());
        assert(decoded->action == PowerActionKind::reboot);
        assert(decoded->force);
    }

    // 7. Bulk action execution via execute_bulk_action
    {
        const std::unordered_set<std::uint64_t> targets{registry_id};

        // Execution with Teacher role fails with authorization error
        const auto teacher_result = execute_bulk_action(
            control_plane, clients, targets, BulkActionKind::reboot, 0, {},
            OperatorRole::teacher);
        assert(teacher_result.attempted == 1);
        assert(teacher_result.succeeded == 0);
        assert(teacher_result.failed == 1);
        assert(!teacher_result.error_messages.empty());
        assert(teacher_result.error_messages[0].find("not authorized") != std::string::npos);

        // Execution with Technician role succeeds
        const auto tech_result = execute_bulk_action(
            control_plane, clients, targets, BulkActionKind::reboot, 0, {},
            OperatorRole::technician);
        assert(tech_result.attempted == 1);
        assert(tech_result.succeeded == 1);
        assert(tech_result.failed == 0);

        const auto frame = channel.receive(&error);
        assert(frame.has_value());
        assert(frame->envelope.type == nstu::protocol::CommandType::power_action);
    }

    // 8. Exam active protection invariant: power actions must be blocked while exam is active
    {
        nstu::exam::ExamStartRequest exam_start;
        exam_start.package_root = "C:/ProgramData/NSTU/exams/sample";
        exam_start.web_root = "C:/ProgramData/NSTU/exams/sample/exam/web";
        exam_start.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
        exam_start.package_id = "exam-power-guard";
        exam_start.candidate_id = "student-01";
        exam_start.client_id = id;
        for (std::size_t i = 0; i < exam_start.package_digest.size(); ++i) {
            exam_start.package_digest[i] = static_cast<std::byte>(i + 1);
        }
        for (std::size_t i = 0; i < exam_start.session_id.size(); ++i) {
            exam_start.session_id[i] = static_cast<std::byte>(i + 0x20);
        }

        // Arm UWF protection status so start_exam succeeds in Release builds
        nstu::control::UwfFleetStatusReport verified;
        verified.phase = nstu::control::UwfFleetPhase::verified_protected;
        verified.probe.probe_succeeded = true;
        verified.probe.filter_current_enabled = true;
        verified.probe.system_volume_current_protected = true;
        for (std::size_t i = 0; i < verified.boot_id.size(); ++i) {
            verified.boot_id[i] = static_cast<std::byte>(0xa0 + i);
        }
        verified.detail = "UWF protects this session";
        assert(registry.set_uwf_fleet_status(registry_id, verified));
        assert(registry.snapshot()[0].uwf.proves_current_protection());

        assert(control_plane.start_exam(registry_id, exam_start, &error));
        const auto exam_cmd = channel.receive(&error);
        assert(exam_cmd.has_value());
        assert(exam_cmd->envelope.type == nstu::protocol::CommandType::exam_start);

        // Power actions must now be rejected even for Technician / Developer
        std::string exam_err;
        assert(!control_plane.power_action(
            registry_id, PowerActionKind::reboot, false, OperatorRole::technician, &exam_err));
        assert(exam_err.find("exam active on client") != std::string::npos);

        assert(!control_plane.power_action(
            registry_id, PowerActionKind::shutdown, true, OperatorRole::developer, &exam_err));
        assert(exam_err.find("exam active on client") != std::string::npos);

        // Stop the exam and verify power actions work again
        assert(control_plane.stop_exam(registry_id, &error));
        const auto stop_cmd = channel.receive(&error);
        assert(stop_cmd.has_value());
        assert(stop_cmd->envelope.type == nstu::protocol::CommandType::exam_stop);

        assert(control_plane.power_action(
            registry_id, PowerActionKind::reboot, false, OperatorRole::technician, &error));
        const auto post_exam_frame = channel.receive(&error);
        assert(post_exam_frame.has_value());
        assert(post_exam_frame->envelope.type == nstu::protocol::CommandType::power_action);
    }

    control_plane.stop();
}

} // namespace

int main() {
    test_power_action_codecs();
    test_operator_role_authorization();
    test_classroom_roster_power_integration();
    test_control_plane_power_action();
    std::cout << "All power action tests passed successfully.\n";
    return 0;
}

