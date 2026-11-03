#include "nstu/classroom_roster.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/multicast.hpp"
#include "nstu/operator_role.hpp"
#include "nstu/protocol.hpp"
#include "nstu/wake_on_lan.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId test_client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 33);
    }
    return id;
}

std::vector<std::byte> test_client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x70 + index);
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

void test_mac_address_parsing_and_formatting() {
    using namespace nstu::net;

    // 1. Standard colon format
    const auto mac1 = parse_mac_address("00:11:22:33:44:55");
    assert(mac1.has_value());
    assert(mac1->octets[0] == 0x00);
    assert(mac1->octets[1] == 0x11);
    assert(mac1->octets[2] == 0x22);
    assert(mac1->octets[3] == 0x33);
    assert(mac1->octets[4] == 0x44);
    assert(mac1->octets[5] == 0x55);
    assert(!mac1->is_zero());
    assert(!mac1->is_broadcast());

    // 2. Standard hyphen format
    const auto mac2 = parse_mac_address("AA-BB-CC-DD-EE-11");
    assert(mac2.has_value());
    assert(mac2->octets[0] == 0xAA);
    assert(mac2->octets[5] == 0x11);
    assert(!mac2->is_broadcast());

    // 2b. Broadcast MAC
    const auto mac_bcast = parse_mac_address("FF-FF-FF-FF-FF-FF");
    assert(mac_bcast.has_value());
    assert(mac_bcast->is_broadcast());

    // 3. Lowercase hex
    const auto mac3 = parse_mac_address("0a:1b:2c:3d:4e:5f");
    assert(mac3.has_value());
    assert(mac3->octets[0] == 0x0A);
    assert(mac3->octets[5] == 0x5F);

    // 4. Zero MAC
    const auto mac_zero = parse_mac_address("00:00:00:00:00:00");
    assert(mac_zero.has_value());
    assert(mac_zero->is_zero());

    // 5. Formatting tests
    const std::string formatted_colon_up = format_mac_address(*mac1, ':', true);
    assert(formatted_colon_up == "00:11:22:33:44:55");

    const std::string formatted_hyphen_low = format_mac_address(*mac2, '-', false);
    assert(formatted_hyphen_low == "aa-bb-cc-dd-ee-11");

    // Round-trip
    assert(parse_mac_address(formatted_colon_up) == mac1);
    assert(parse_mac_address(formatted_hyphen_low) == mac2);

    // 6. Invalid / malformed inputs
    assert(!parse_mac_address("").has_value());
    assert(!parse_mac_address("00:11:22:33:44").has_value()); // too short
    assert(!parse_mac_address("00:11:22:33:44:55:66").has_value()); // too long
    assert(!parse_mac_address("00:11:22:33:44:GG").has_value()); // invalid hex
    assert(!parse_mac_address("00:11-22:33-44:55").has_value()); // mixed separators
    assert(!parse_mac_address("00.11.22.33.44.55").has_value()); // dot separator
    assert(!parse_mac_address("001122334455").has_value()); // no separator
}

void test_magic_packet_creation_and_verification() {
    using namespace nstu::net;

    const auto mac = parse_mac_address("12:34:56:78:9A:BC");
    assert(mac.has_value());

    const auto packet = create_magic_packet(*mac);
    assert(packet.size() == kWolMagicPacketSize);

    // First 6 bytes must be 0xFF
    for (std::size_t i = 0; i < 6; ++i) {
        assert(packet[i] == 0xFF);
    }

    // Next 16 repetitions of MAC
    for (std::size_t rep = 0; rep < 16; ++rep) {
        for (std::size_t i = 0; i < 6; ++i) {
            assert(packet[6 + rep * 6 + i] == mac->octets[i]);
        }
    }

    // Verify valid packet
    assert(verify_magic_packet(packet, *mac));

    // Verify rejection on wrong MAC
    const auto other_mac = parse_mac_address("12:34:56:78:9A:BD");
    assert(!verify_magic_packet(packet, *other_mac));

    // Verify rejection on corrupted preamble
    auto corrupted_packet = packet;
    corrupted_packet[2] = 0xFE;
    assert(!verify_magic_packet(corrupted_packet, *mac));

    // Verify rejection on invalid size
    std::vector<std::uint8_t> short_packet(packet.begin(), packet.begin() + 101);
    assert(!verify_magic_packet(short_packet, *mac));
}

void test_wol_support_state_and_labels() {
    using namespace nstu::net;

    assert(std::string(to_string(WolSupportState::unknown)) == "unknown");
    assert(std::string(to_string(WolSupportState::supported)) == "supported");
    assert(std::string(to_string(WolSupportState::unsupported)) == "unsupported");
    assert(std::string(to_string(WolSupportState::missing_mac)) == "missing_mac");

    // Bilingual labels
    assert(std::string(wol_support_label(WolSupportState::supported, false)) == "Supported");
    assert(std::string(wol_support_label(WolSupportState::supported, true)) == "Được hỗ trợ");

    assert(std::string(wol_support_label(WolSupportState::unsupported, false)) == "Unsupported");
    assert(std::string(wol_support_label(WolSupportState::unsupported, true)) == "Không hỗ trợ");

    assert(std::string(wol_support_label(WolSupportState::missing_mac, false)) == "Missing MAC");
    assert(std::string(wol_support_label(WolSupportState::missing_mac, true)) == "Thiếu địa chỉ MAC");
}

void test_wol_report_codecs() {
    using namespace nstu::control;
    using namespace nstu::net;

    const auto mac = parse_mac_address("AA:BB:CC:11:22:33");
    assert(mac.has_value());

    // 1. Valid report: supported
    {
        const WolReportMessage msg{
            .mac = *mac,
            .support = WolSupportState::supported,
        };
        const auto encoded = encode_wol_report(msg);
        assert(encoded.size() == 8);
        assert(encoded[6] == std::byte{1}); // support
        assert(encoded[7] == std::byte{0}); // reserved

        const auto decoded = decode_wol_report(encoded);
        assert(decoded.has_value());
        assert(decoded->mac == *mac);
        assert(decoded->support == WolSupportState::supported);
    }

    // 2. Valid report: unsupported
    {
        const WolReportMessage msg{
            .mac = *mac,
            .support = WolSupportState::unsupported,
        };
        const auto encoded = encode_wol_report(msg);
        const auto decoded = decode_wol_report(encoded);
        assert(decoded.has_value());
        assert(decoded->support == WolSupportState::unsupported);
    }

    // 3. Rejections
    // Invalid size
    std::vector<std::byte> short_payload(7, std::byte{0});
    assert(!decode_wol_report(short_payload).has_value());

    std::vector<std::byte> long_payload(9, std::byte{0});
    assert(!decode_wol_report(long_payload).has_value());

    // Non-zero reserved byte
    std::vector<std::byte> bad_reserved(8, std::byte{0});
    bad_reserved[7] = std::byte{1};
    assert(!decode_wol_report(bad_reserved).has_value());

    // Out of range enum
    std::vector<std::byte> bad_enum(8, std::byte{0});
    bad_enum[6] = std::byte{4};
    assert(!decode_wol_report(bad_enum).has_value());
}

void test_operator_role_wake_on_lan() {
    using namespace nstu::server;

    // Wake-on-LAN is permitted for Teacher (pre-class lab startup), Technician (maintenance), and Developer
    assert(is_action_permitted(OperatorRole::teacher, OperatorAction::wake_on_lan));
    assert(is_action_permitted(OperatorRole::technician, OperatorAction::wake_on_lan));
    assert(is_action_permitted(OperatorRole::developer, OperatorAction::wake_on_lan));
}

void test_client_registry_wol_state() {
    using namespace nstu::net;
    using namespace nstu::server;

    ClientRegistry registry;

    ClientRecord client;
    client.id = 42;
    client.hostname = "PC-LAB-42";
    client.status = ClientStatus::offline;
    registry.upsert(client);

    // Initial state
    auto snapshot = registry.snapshot();
    assert(snapshot.size() == 1);
    assert(snapshot[0].mac_address.empty());
    assert(snapshot[0].wol_support == WolSupportState::unknown);

    // Report WOL support
    const auto mac = parse_mac_address("00:25:90:AB:CD:EF");
    assert(mac.has_value());
    assert(registry.set_wol_report(42, *mac, WolSupportState::supported));

    snapshot = registry.snapshot();
    assert(snapshot[0].mac_address == "00:25:90:AB:CD:EF");
    assert(snapshot[0].wol_support == WolSupportState::supported);

    // Upsert preserving MAC and WOL status on reconnect
    ClientRecord update;
    update.id = 42;
    update.hostname = "PC-LAB-42";
    update.status = ClientStatus::online;
    registry.upsert(update);

    snapshot = registry.snapshot();
    assert(snapshot[0].status == ClientStatus::online);
    assert(snapshot[0].mac_address == "00:25:90:AB:CD:EF");
    assert(snapshot[0].wol_support == WolSupportState::supported);

    // Unknown client returns false
    assert(!registry.set_wol_report(999, *mac, WolSupportState::supported));
}

void test_control_plane_wake_client() {
    using namespace nstu::control;
    using namespace nstu::net;
    using namespace nstu::server;

    WinsockRuntime winsock;
    assert(winsock.ready());

    const auto id = test_client_id();
    auto key = test_client_key();
    constexpr std::uint32_t key_id = 15;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));

    ClientRegistry registry;
    ServerControlPlane control_plane(registry, key_store);
    ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 16;
    assert(control_plane.start(std::move(config), &error));

    // 1. Unknown client
    assert(!control_plane.wake_client(999, OperatorRole::teacher, &error));
    assert(error.find("not found") != std::string::npos);

    // 2. Setup offline client with missing MAC
    ClientRecord client1;
    client1.id = 1001;
    client1.hostname = "PC-OFFLINE-NO-MAC";
    client1.status = ClientStatus::offline;
    registry.upsert(client1);

    error.clear();
    assert(!control_plane.wake_client(1001, OperatorRole::teacher, &error));
    assert(error.find("MAC address is unknown") != std::string::npos);

    // 3. Client with unsupported hardware
    const auto mac = parse_mac_address("00:11:22:33:44:55");
    assert(mac.has_value());
    assert(registry.set_wol_report(1001, *mac, WolSupportState::unsupported));

    error.clear();
    assert(!control_plane.wake_client(1001, OperatorRole::teacher, &error));
    assert(error.find("not supported") != std::string::npos);

    // 4. Online client (already awake)
    ClientRecord client2;
    client2.id = 1002;
    client2.hostname = "PC-ONLINE";
    client2.status = ClientStatus::online;
    registry.upsert(client2);
    assert(registry.set_wol_report(1002, *mac, WolSupportState::supported));

    error.clear();
    assert(!control_plane.wake_client(1002, OperatorRole::teacher, &error));
    assert(error.find("already online") != std::string::npos);

    // 5. Valid offline client with known MAC & supported WOL
    ClientRecord client3;
    client3.id = 1003;
    client3.hostname = "PC-READY-TO-WAKE";
    client3.status = ClientStatus::offline;
    registry.upsert(client3);
    assert(registry.set_wol_report(1003, *mac, WolSupportState::supported));

    error.clear();
    const bool wake_ok = control_plane.wake_client(1003, OperatorRole::teacher, &error);
    assert(wake_ok);
    assert(error.empty());
}

void test_classroom_roster_bulk_wake() {
    using namespace nstu::net;
    using namespace nstu::server;

    // 1. Bulk target evaluation
    std::vector<ClientRecord> roster_clients;
    ClientRecord online1;
    online1.id = 201;
    online1.hostname = "PC-201";
    online1.status = ClientStatus::online;
    roster_clients.push_back(online1);

    ClientRecord offline2;
    offline2.id = 202;
    offline2.hostname = "PC-202";
    offline2.status = ClientStatus::offline;
    roster_clients.push_back(offline2);

    ClientRecord offline3;
    offline3.id = 203;
    offline3.hostname = "PC-203";
    offline3.status = ClientStatus::offline;
    roster_clients.push_back(offline3);

    const std::unordered_set<std::uint64_t> selected{201, 202, 203};
    const auto target_summary = evaluate_bulk_targets(roster_clients, selected);
    assert(target_summary.total_selected == 3);
    assert(target_summary.eligible_online == 1);
    assert(target_summary.skipped_offline == 2);

    // 2. Bilingual confirmation titles and messages
    const std::string title_en = format_bulk_confirmation_title(BulkActionKind::wake_on_lan, false);
    assert(title_en.find("Wake-on-LAN") != std::string::npos);

    const std::string title_vi = format_bulk_confirmation_title(BulkActionKind::wake_on_lan, true);
    assert(title_vi.find("Wake-on-LAN") != std::string::npos);

    const std::string msg_en = format_bulk_confirmation_message(BulkActionKind::wake_on_lan, 2, "Lab 101", false);
    assert(msg_en.find("WAKE 2 selected offline computer(s)") != std::string::npos);
    assert(msg_en.find("Lab 101") != std::string::npos);

    const std::string msg_vi = format_bulk_confirmation_message(BulkActionKind::wake_on_lan, 2, "Phòng 101", true);
    assert(msg_vi.find("BẬT 2 máy tính ngoại tuyến") != std::string::npos);
    assert(msg_vi.find("Phòng 101") != std::string::npos);

    // 3. Bilingual result status
    BulkActionResult result;
    result.attempted = 2;
    result.succeeded = 2;
    result.failed = 0;

    const std::string status_en = format_bulk_action_result_status(BulkActionKind::wake_on_lan, result, false);
    assert(status_en.find("Wake-on-LAN") != std::string::npos);
    assert(status_en.find("2 succeeded") != std::string::npos);

    const std::string status_vi = format_bulk_action_result_status(BulkActionKind::wake_on_lan, result, true);
    assert(status_vi.find("bật máy qua mạng") != std::string::npos);
    assert(status_vi.find("2 thành công") != std::string::npos);
}

void test_detect_local_hardware_wol() {
    using namespace nstu::net;

    const auto info1 = detect_local_hardware_wol();
    // Repeating detection must be stable and leak-free
    const auto info2 = detect_local_hardware_wol();

    assert(info1.wol_support == info2.wol_support);
    assert(info1.mac == info2.mac);

    if (info1.wol_support == WolSupportState::supported ||
        info1.wol_support == WolSupportState::unsupported) {
        assert(!info1.mac.is_zero());
    } else if (info1.wol_support == WolSupportState::missing_mac) {
        assert(info1.mac.is_zero());
    }
}

} // namespace

int main() {
    std::cout << "Running Wake-on-LAN tests...\n";

    test_mac_address_parsing_and_formatting();
    std::cout << "PASS: test_mac_address_parsing_and_formatting\n";

    test_magic_packet_creation_and_verification();
    std::cout << "PASS: test_magic_packet_creation_and_verification\n";

    test_wol_support_state_and_labels();
    std::cout << "PASS: test_wol_support_state_and_labels\n";

    test_wol_report_codecs();
    std::cout << "PASS: test_wol_report_codecs\n";

    test_operator_role_wake_on_lan();
    std::cout << "PASS: test_operator_role_wake_on_lan\n";

    test_client_registry_wol_state();
    std::cout << "PASS: test_client_registry_wol_state\n";

    test_control_plane_wake_client();
    std::cout << "PASS: test_control_plane_wake_client\n";

    test_classroom_roster_bulk_wake();
    std::cout << "PASS: test_classroom_roster_bulk_wake\n";

    test_detect_local_hardware_wol();
    std::cout << "PASS: test_detect_local_hardware_wol\n";

    std::cout << "All Wake-on-LAN tests passed successfully!\n";
    return 0;
}

