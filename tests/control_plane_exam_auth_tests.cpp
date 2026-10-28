// Compile the exact implementation in this test TU to isolate its test hooks.
// The production static library compiles without this guard or mutable clocks.
#define NSTU_CONTROL_PLANE_AUTH_TESTING
#include "../server/src/control_plane.cpp"

#include "nstu/control_channel.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/exam_telemetry.hpp"
#include "nstu/multicast.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(0x20u + index);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x70u + index);
    }
    return key;
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::filesystem::path journal_path() {
    const auto tick = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
    return std::filesystem::temp_directory_path() /
           (std::string("nstu-control-plane-auth-") + std::to_string(tick) +
            ".bin");
}

nstu::exam::ExamStartRequest make_start(
    const nstu::security::ClientId& id) {
    nstu::exam::ExamStartRequest request;
    request.package_root = "C:/ProgramData/NSTU/exams/auth-test";
    request.web_root =
        "C:/ProgramData/NSTU/exams/auth-test/exam/web";
    request.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
    request.package_id = "auth-test-package";
    request.candidate_id = "candidate-auth-01";
    request.client_id = id;
    for (std::size_t index = 0; index < request.package_digest.size();
         ++index) {
        request.package_digest[index] =
            static_cast<std::byte>(0x40u + index);
    }
    for (std::size_t index = 0; index < request.session_id.size(); ++index) {
        request.session_id[index] = static_cast<std::byte>(0x90u + index);
    }
    return request;
}

nstu::exam::AnswerEvent make_event(
    const nstu::exam::ExamStartRequest& start, std::uint64_t sequence) {
    nstu::exam::AnswerEvent event;
    event.package_id = start.package_id;
    event.package_digest = start.package_digest;
    event.client_id = start.client_id;
    event.session_id = start.session_id;
    event.candidate_id = start.candidate_id;
    event.question_id = "reading-01";
    event.question_revision = 1;
    event.sequence = sequence;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL + sequence;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {static_cast<std::byte>('B')};
    return event;
}

nstu::exam::StateRequest make_state_request(
    const nstu::exam::ExamStartRequest& start) {
    nstu::exam::StateRequest request;
    request.package_id = start.package_id;
    request.package_digest = start.package_digest;
    request.client_id = start.client_id;
    request.session_id = start.session_id;
    request.candidate_id = start.candidate_id;
    return request;
}

std::optional<nstu::control::AuthenticatedControlChannel> connect_channel(
    nstu::server::ServerControlPlane& control_plane,
    const nstu::security::ClientId& id, std::uint32_t key_id,
    std::span<const std::byte> key, std::string* error) {
    nstu::net::TcpSocket socket;
    if (!socket.connect("127.0.0.1", control_plane.local_port(), error) ||
        !socket.set_io_timeouts(5000, 5000, error)) {
        return std::nullopt;
    }
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto session = nstu::control::client_handshake(
        socket, id, key_id, key, now, std::chrono::seconds(120), error);
    if (!session) {
        return std::nullopt;
    }
    return nstu::control::AuthenticatedControlChannel(std::move(socket),
                                                       std::move(*session));
}

nstu::control::UwfFleetStatusReport verified_protection() {
    nstu::control::UwfFleetStatusReport proof;
    proof.phase = nstu::control::UwfFleetPhase::verified_protected;
    proof.probe.probe_succeeded = true;
    proof.probe.filter_current_enabled = true;
    proof.probe.system_volume_current_protected = true;
    proof.boot_id.fill(std::byte{0xb1});
    return proof;
}

// Each barrier observes one real callback. Every park has a bounded release and
// completion rendezvous before the plane or captured test state is destroyed.
struct OwnershipRendezvous {
    std::mutex mutex;
    std::condition_variable condition;
    bool park = false;
    bool entered = false;
    bool released = false;
    bool completed = false;
    nstu::net::ConnectionId connection = 0;
    std::function<void()> close_server_socket;

    void arrive(bool before, nstu::net::ConnectionId id,
                std::function<void()> close = {}) {
        std::unique_lock lock(mutex);
        if (before) {
            connection = id;
            close_server_socket = std::move(close);
            entered = true;
            condition.notify_all();
            if (park) {
                assert(condition.wait_for(lock, std::chrono::seconds(5),
                                          [&] { return released; }));
            }
        } else {
            completed = true;
            condition.notify_all();
        }
    }
    void wait_for_entry() {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return entered; }));
    }
    void wait_for_completion() {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return completed; }));
    }
    void release_and_wait() {
        {
            std::scoped_lock lock(mutex);
            released = true;
        }
        condition.notify_all();
        wait_for_completion();
    }
    void watch(nstu::protocol::CommandType type, std::uint64_t request) {
        nstu::server::set_registry_mutation_hook_for_testing(
            [this, type, request](bool before, nstu::net::ConnectionId id,
                                 nstu::protocol::CommandType actual_type,
                                 std::uint64_t actual_request) {
                if (actual_type == type && actual_request == request) {
                    arrive(before, id);
                }
            });
    }
};

struct OwnershipFixture {
    nstu::security::ClientId id = client_id();
    std::vector<std::byte> key = client_key();
    nstu::security::KeyStore store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane plane{registry, store};
    std::optional<nstu::control::AuthenticatedControlChannel> a;
    std::optional<nstu::control::AuthenticatedControlChannel> b;
    std::function<void()> close_b_server_socket;
    std::string error;
    std::uint64_t request = 1000;

    OwnershipFixture() {
        assert(store.enroll(id, 9, key, &error));
        nstu::server::ServerControlPlaneConfig config;
        config.port = 0;
        config.maximum_clients = 16;
        assert(plane.start(std::move(config), &error));
        a = connect_channel(plane, id, 9, key, &error);
        assert(a);
        status(*a, "OWNER-A");
    }
    ~OwnershipFixture() {
        nstu::server::set_registry_mutation_hook_for_testing({});
        nstu::server::set_auth_publication_hook_for_testing({});
        nstu::server::set_disconnect_hold_for_testing(false);
        (void)nstu::server::release_held_disconnects_for_testing();
        if (b) b->close();
        if (a) a->close();
        plane.stop();
        nstu::security::secure_zero(key);
    }
    nstu::server::ClientRecord record() const {
        const auto records = registry.snapshot();
        assert(records.size() == 1);
        return records.front();
    }
    void replace() {
        OwnershipRendezvous publication;
        nstu::server::set_auth_publication_hook_for_testing(
            [&](bool before, nstu::net::ConnectionId connection,
                std::function<void()> close) {
                publication.arrive(before, connection, std::move(close));
            });
        b = connect_channel(plane, id, 9, key, &error);
        assert(b);
        publication.wait_for_completion();
        close_b_server_socket = std::move(publication.close_server_socket);
        nstu::server::set_auth_publication_hook_for_testing({});
        status(*b, "OWNER-B");
    }
    void send_and_wait(nstu::control::AuthenticatedControlChannel& channel,
                       nstu::protocol::CommandType type,
                       std::span<const std::byte> payload) {
        OwnershipRendezvous completion;
        completion.watch(type, ++request);
        assert(channel.send(type, request, payload, &error));
        completion.wait_for_completion();
        nstu::server::set_registry_mutation_hook_for_testing({});
    }
    void status(nstu::control::AuthenticatedControlChannel& channel,
                const std::string& hostname) {
        nstu::control::ClientStatusReport report;
        report.hostname = hostname;
        send_and_wait(channel, nstu::protocol::CommandType::status_report,
                      nstu::control::encode_status_report(report));
    }
    void proof(nstu::control::AuthenticatedControlChannel& channel,
               std::byte boot) {
        auto report = verified_protection();
        report.boot_id.fill(boot);
        report.detail = "current carrier wire probe";
        send_and_wait(channel, nstu::protocol::CommandType::uwf_fleet_status,
                      nstu::control::encode_uwf_fleet_status_report(report));
    }
};

void test_stale_authenticated_registry_callback(
    nstu::protocol::CommandType type) {
    OwnershipFixture fixture;
    fixture.proof(*fixture.a, std::byte{0xa1});
    assert(fixture.record().uwf.proves_current_protection());
    std::vector<std::byte> payload;
    if (type == nstu::protocol::CommandType::uwf_fleet_status) {
        auto report = verified_protection();
        report.boot_id.fill(std::byte{0xa1});
        report.detail = "parked old carrier wire probe";
        payload = nstu::control::encode_uwf_fleet_status_report(report);
    } else if (type == nstu::protocol::CommandType::status_report) {
        nstu::control::ClientStatusReport report;
        report.hostname = "STALE-OWNER-A";
        payload = nstu::control::encode_status_report(report);
    }
    OwnershipRendezvous stale;
    stale.park = true;
    stale.watch(type, ++fixture.request);
    assert(fixture.a->send(type, fixture.request, payload, &fixture.error));
    stale.wait_for_entry();
    nstu::server::set_disconnect_hold_for_testing(true);
    fixture.replace();
    const auto before = fixture.record();
    assert(!before.uwf.proves_current_protection());
    assert(before.hostname == "OWNER-B");
    assert(nstu::server::held_disconnect_count_for_testing() == 1);
    stale.release_and_wait();
    const auto after = fixture.record();
    assert(!after.uwf.proves_current_protection());
    assert(after.uwf.observed_boot_id == before.uwf.observed_boot_id);
    assert(after.hostname == before.hostname);
    assert(after.last_seen == before.last_seen);

    // Current B wire updates still work, and late A cleanup preserves B proof.
    fixture.proof(*fixture.b, std::byte{0xb2});
    fixture.status(*fixture.b, "OWNER-B-CURRENT");
    assert(fixture.record().uwf.proves_current_protection());
    const auto current = fixture.record();
    fixture.send_and_wait(*fixture.b, nstu::protocol::CommandType::heartbeat, {});
    assert(fixture.record().last_seen >= current.last_seen);
    nstu::server::set_disconnect_hold_for_testing(false);
    assert(nstu::server::release_held_disconnects_for_testing() == 1);
    assert(fixture.record().hostname == "OWNER-B-CURRENT");
    assert(fixture.record().status == nstu::server::ClientStatus::online);
    assert(fixture.record().uwf.proves_current_protection());
    // Server close runs removal synchronously while B's peer is still open.
    // Closing the peer first could start an independent disconnect callback.
    fixture.close_b_server_socket();
    fixture.b->close();
    assert(fixture.record().status == nstu::server::ClientStatus::offline);
    assert(!fixture.record().uwf.proves_current_protection());
}

void test_removed_authenticated_state_cannot_publish() {
    OwnershipFixture fixture;
    fixture.proof(*fixture.a, std::byte{0xa1});
    const auto before = fixture.record();
    OwnershipRendezvous publication;
    publication.park = true;
    nstu::server::set_auth_publication_hook_for_testing(
        [&](bool entering, nstu::net::ConnectionId id,
            std::function<void()> close) {
            publication.arrive(entering, id, std::move(close));
        });
    fixture.b = connect_channel(fixture.plane, fixture.id, 9, fixture.key,
                                &fixture.error);
    assert(fixture.b);
    publication.wait_for_entry();
    const auto disconnects = nstu::server::completed_disconnect_count_for_testing();
    fixture.b->close();
    // There is no posted receive while auth on_bytes is parked. Close its real
    // server socket too, invoking the actual dispatcher disconnect/removal.
    publication.close_server_socket();
    assert(nstu::server::completed_disconnect_count_for_testing() == disconnects + 1);
    publication.release_and_wait();
    nstu::server::set_auth_publication_hook_for_testing({});
    const auto after = fixture.record();
    assert(after.hostname == before.hostname);
    assert(after.last_seen == before.last_seen);
    assert(after.uwf.proves_current_protection());
    assert(after.uwf.verified_boot_id == before.uwf.verified_boot_id);
    assert(fixture.plane.authenticated_client_count() == 1);
    fixture.status(*fixture.a, "OWNER-A-PRESERVED");
    assert(fixture.record().uwf.proves_current_protection());
}

void test_authenticated_ownership_regressions() {
    // Selection permits separate real RED receipts; the ordinary gate executes
    // every new case as well as every original assertion in this binary.
    const auto* selected = std::getenv("NSTU_AUTH_OWNERSHIP_CASE");
    const auto run = [&](const char* name) {
        return selected == nullptr || std::string_view(selected) == name;
    };
    if (run("fleet")) test_stale_authenticated_registry_callback(
        nstu::protocol::CommandType::uwf_fleet_status);
    if (run("status")) test_stale_authenticated_registry_callback(
        nstu::protocol::CommandType::status_report);
    if (run("heartbeat")) test_stale_authenticated_registry_callback(
        nstu::protocol::CommandType::heartbeat);
    if (run("removed")) test_removed_authenticated_state_cannot_publish();
}

struct DeadlineFixture {
    nstu::security::ClientId id = client_id();
    std::vector<std::byte> key = client_key();
    nstu::security::KeyStore store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane plane{registry, store};
    std::filesystem::path path = journal_path();
    std::optional<nstu::control::AuthenticatedControlChannel> channel;
    nstu::exam::ExamStartRequest start = make_start(id);
    nstu::exam::AnswerEvent event = make_event(start, 1);
    std::uint64_t registry_id = 0;
    std::string error;

    DeadlineFixture() {
        assert(store.enroll(id, 9, key, &error));
        nstu::server::ServerControlPlaneConfig config;
        config.port = 0;
        config.maximum_clients = 16;
        config.exam_journal_path = path;
        assert(plane.start(std::move(config), &error));
        reconnect("CLOCK-INITIAL");
        assert(registry.set_uwf_fleet_status(registry_id, verified_protection()));
        assert(plane.start_exam(registry_id, start, &error));
        const auto command = channel->receive(&error);
        assert(command && command->envelope.type == nstu::protocol::CommandType::exam_start);
        receive_enable();
        const auto wire = nstu::exam::encode_answer_event(event);
        assert(channel->send(nstu::protocol::CommandType::exam_answer_event, 100, wire, &error));
        const auto ack_frame = channel->receive(&error);
        assert(ack_frame && ack_frame->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
        const auto ack = nstu::exam::decode_answer_ack(ack_frame->payload);
        assert(ack && ack->status == nstu::exam::AnswerAckStatus::accepted);
    }
    ~DeadlineFixture() {
        if (channel) channel->close();
        plane.stop();
        nstu::security::secure_zero(key);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto lock_path = path;
        lock_path += L".lock";
        std::filesystem::remove(lock_path, ec);
    }
    void reconnect(const std::string& label) {
        if (channel) channel->close();
        channel = connect_channel(plane, id, 9, key, &error);
        assert(channel);
        nstu::control::ClientStatusReport status;
        status.hostname = label;
        assert(channel->send(nstu::protocol::CommandType::status_report, 1,
            nstu::control::encode_status_report(status), &error));
        assert(wait_until([&] {
            const auto peers = registry.snapshot();
            const auto peer = std::find_if(peers.begin(), peers.end(), [&](const auto& p) {
                return p.hostname == label && p.status == nstu::server::ClientStatus::online;
            });
            if (peer == peers.end()) return false;
            registry_id = peer->id;
            return true;
        }));
    }
    void receive_enable() {
        const auto frame = channel->receive(&error);
        assert(frame && frame->envelope.type == nstu::protocol::CommandType::exam_telemetry_enable);
        const auto enable = nstu::exam::decode_telemetry_enable(frame->payload);
        assert(enable && enable->package_id == start.package_id &&
            enable->package_digest == start.package_digest && enable->client_id == start.client_id &&
            enable->session_id == start.session_id && enable->candidate_id == start.candidate_id);
    }
    void receive_answer_state() {
        assert(channel->send(nstu::protocol::CommandType::exam_state_request, 200,
            nstu::exam::encode_state_request(make_state_request(start)), &error));
        const auto frame = channel->receive(&error);
        assert(frame && frame->envelope.type == nstu::protocol::CommandType::exam_state_response);
        const auto state = nstu::exam::decode_state_response(frame->payload);
        assert(state && state->answers.size() == 1 && state->session_id == start.session_id);
        assert(state->answers.front().answer == event.answer);
        assert(state->answers.front().event_hash == *nstu::exam::hash_answer_event(event));
        assert(state->highest_contiguous_sequence == 1);
    }
};

void test_wall_expiry_then_rollback_preserves_answers() {
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::milliseconds(0));
    DeadlineFixture fixture;
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::hours(26));
    fixture.reconnect("CLOCK-EXPIRED");
    fixture.receive_answer_state();
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::hours(24));
    fixture.reconnect("CLOCK-ROLLED-BACK");
    fixture.receive_answer_state();
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::milliseconds(0));
}

void test_fixed_deadline_boundaries_and_wrong_client() {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    const auto wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto steady = Clock::now();
    const auto hours_ms = [](std::chrono::hours hours) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(hours).count());
    };
    nstu::server::set_telemetry_clock_for_testing(nstu::server::telemetry_detail::ClockSnapshot{wall, steady});
    {
        DeadlineFixture fixture;
        auto other_id = fixture.id;
        other_id.front() ^= std::byte{0x80};
        assert(fixture.store.enroll(other_id, 9, fixture.key, &fixture.error));
        auto other = connect_channel(fixture.plane, other_id, 9, fixture.key, &fixture.error);
        assert(other);
        nstu::control::ClientStatusReport status;
        status.hostname = "CLOCK-WRONG-CLIENT";
        assert(other->send(nstu::protocol::CommandType::status_report, 1,
            nstu::control::encode_status_report(status), &fixture.error));
        std::uint64_t other_registry_id = 0;
        assert(wait_until([&] {
            for (const auto& p : fixture.registry.snapshot()) {
                if (p.hostname == status.hostname && p.status == nstu::server::ClientStatus::online) {
                    other_registry_id = p.id;
                    return true;
                }
            }
            return false;
        }));
        assert(fixture.plane.send_command(other_registry_id, nstu::protocol::CommandType::heartbeat,
            {}, &fixture.error));
        const auto other_frame = other->receive(&fixture.error);
        assert(other_frame && other_frame->envelope.type == nstu::protocol::CommandType::heartbeat);
        other->close();

        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(24h), steady + 24h});
        fixture.reconnect("CLOCK-24-HOURS");
        fixture.receive_enable();
        fixture.receive_answer_state();
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(25h) - 1, steady + 24h});
        fixture.reconnect("CLOCK-JUST-BEFORE");
        fixture.receive_enable();
        fixture.receive_answer_state();
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(25h), steady + 24h});
        fixture.reconnect("CLOCK-EXACT-WALL-EXPIRY");
        fixture.receive_answer_state();
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(24h), steady + 24h});
        fixture.reconnect("CLOCK-LATCH-AFTER-ROLLBACK");
        fixture.receive_answer_state();
    }
    nstu::server::set_telemetry_clock_for_testing(nstu::server::telemetry_detail::ClockSnapshot{wall, steady});
    {
        DeadlineFixture fixture;
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(24h), steady + 24h});
        fixture.reconnect("CLOCK-BEFORE-WALL-ROLLBACK");
        fixture.receive_enable();
        fixture.receive_answer_state();
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(1h), steady + 25h - 1ms});
        fixture.reconnect("CLOCK-STEADY-JUST-BEFORE-WALL-ROLLED-BACK");
        fixture.receive_enable();
        fixture.receive_answer_state();
        nstu::server::set_telemetry_clock_for_testing(
            nstu::server::telemetry_detail::ClockSnapshot{wall + hours_ms(1h), steady + 25h});
        fixture.reconnect("CLOCK-STEADY-EXACT-WALL-WITHIN-WINDOW");
        fixture.receive_answer_state();
    }
    nstu::server::set_telemetry_clock_for_testing(std::nullopt);
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const auto id = client_id();
    auto key = client_key();
    constexpr std::uint32_t key_id = 9;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));

    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 16;
    const auto path = journal_path();
    config.exam_journal_path = path;
    assert(control_plane.start(std::move(config), &error));

    auto channel = connect_channel(control_plane, id, key_id, key, &error);
    assert(channel.has_value());

    nstu::control::ClientStatusReport status;
    status.hostname = "AUTH-TEST-PC";
    const auto status_payload = nstu::control::encode_status_report(status);
    assert(channel->send(nstu::protocol::CommandType::hello, 1,
                         status_payload, &error));
    assert(wait_until([&] { return registry.snapshot().size() == 1; }));
    const auto registry_snapshot = registry.snapshot();
    assert(registry_snapshot.size() == 1);
    const auto registry_id = registry_snapshot.front().id;

    // Handshake registration precedes UWF proof retirement. Authenticated hello
    // runs afterward under the same connection mutex, so observe it before
    // installing the current-session proof.
    assert(wait_until([&] {
        const auto peers = registry.snapshot();
        return peers.size() == 1 && peers.front().id == registry_id &&
               peers.front().hostname == status.hostname &&
               peers.front().status == nstu::server::ClientStatus::online;
    }));

    // The exam gate is server-side and fails closed. Prove current-session UWF
    // protection before any start_exam can succeed.
    nstu::control::UwfFleetStatusReport uwf_verified;
    uwf_verified.phase = nstu::control::UwfFleetPhase::verified_protected;
    uwf_verified.probe.probe_succeeded = true;
    uwf_verified.probe.filter_current_enabled = true;
    uwf_verified.probe.system_volume_current_protected = true;
    for (std::size_t index = 0; index < uwf_verified.boot_id.size(); ++index) {
        uwf_verified.boot_id[index] = static_cast<std::byte>(0xb0 + index);
    }
    uwf_verified.detail = "UWF protects this session";
    assert(registry.set_uwf_fleet_status(registry_id, uwf_verified));

    // The teacher UI supplies package/candidate fields only. The control plane
    // must bind the authenticated client identity and mint a fresh session so
    // UI code cannot accidentally authorize another client or reuse a session.
    nstu::server::ExamLaunchRequest launch;
    launch.package_root = "C:/ProgramData/NSTU/exams/packages/auth-test/";
    launch.web_root = launch.package_root + "exam/web";
    launch.user_data_root =
        "C:/ProgramData/NSTU/exam-user-data/candidate-auth-ui";
    launch.package_id = "auth-test-package";
    launch.candidate_id = "candidate-auth-ui";
    for (std::size_t index = 0; index < launch.package_digest.size(); ++index) {
        launch.package_digest[index] = static_cast<std::byte>(0x60u + index);
    }
    assert(control_plane.launch_exam(registry_id, launch, &error));
    const auto launched_command = channel->receive(&error);
    assert(launched_command.has_value());
    const auto launched =
        nstu::exam::decode_exam_start_request(launched_command->payload);
    assert(launched.has_value());
    assert(launched->client_id == id);
    assert(launched->package_id == launch.package_id);
    assert(launched->candidate_id == launch.candidate_id);
    assert(std::any_of(launched->session_id.begin(), launched->session_id.end(),
                       [](std::byte value) { return value != std::byte{0}; }));
    const auto launched_states = control_plane.exam_client_states();
    assert(launched_states.size() == 1);
    assert(launched_states.front().client_id == registry_id);
    assert(!launched_states.front().ready);
    assert(!launched_states.front().begun);

    const auto launched_enable = channel->receive(&error);
    assert(launched_enable.has_value());
    assert(launched_enable->envelope.type ==
           nstu::protocol::CommandType::exam_telemetry_enable);
    const auto decoded_launched_enable =
        nstu::exam::decode_telemetry_enable(launched_enable->payload);
    assert(decoded_launched_enable.has_value());
    assert(decoded_launched_enable->client_id == id);
    assert(decoded_launched_enable->package_digest == launch.package_digest);
    assert(decoded_launched_enable->session_id == launched->session_id);
    assert(decoded_launched_enable->package_id == launch.package_id);
    assert(decoded_launched_enable->candidate_id == launch.candidate_id);
    assert(decoded_launched_enable->server_time_unix_milliseconds != 0);

    assert(control_plane.stop_exam(registry_id, &error));
    const auto launched_stop = channel->receive(&error);
    assert(launched_stop.has_value());
    assert(launched_stop->envelope.type ==
           nstu::protocol::CommandType::exam_stop);
    assert(control_plane.exam_client_states().empty());

    const auto start = make_start(id);
    assert(control_plane.start_exam(registry_id, start, &error));
    const auto start_command = channel->receive(&error);
    assert(start_command.has_value());
    assert(start_command->envelope.type ==
           nstu::protocol::CommandType::exam_start);

    const auto start_enable = channel->receive(&error);
    assert(start_enable.has_value());
    assert(start_enable->envelope.type ==
           nstu::protocol::CommandType::exam_telemetry_enable);
    const auto decoded_start_enable =
        nstu::exam::decode_telemetry_enable(start_enable->payload);
    assert(decoded_start_enable.has_value());
    assert(decoded_start_enable->client_id == id);
    assert(decoded_start_enable->package_id == start.package_id);
    assert(decoded_start_enable->candidate_id == start.candidate_id);
    assert(decoded_start_enable->session_id == start.session_id);
    assert(decoded_start_enable->server_time_unix_milliseconds != 0);

    // A second start cannot replace the server-issued context while the
    // current session is active.
    assert(!control_plane.start_exam(registry_id, start, &error));

    auto accepted_event = make_event(start, 1);
    const auto accepted_wire = nstu::exam::encode_answer_event(accepted_event);
    assert(!accepted_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_answer_event, 10,
                         accepted_wire, &error));
    const auto accepted_ack = channel->receive(&error);
    assert(accepted_ack.has_value());
    assert(accepted_ack->envelope.type ==
           nstu::protocol::CommandType::exam_answer_ack);
    const auto decoded_ack =
        nstu::exam::decode_answer_ack(accepted_ack->payload);
    assert(decoded_ack.has_value());
    assert(decoded_ack->status == nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_ack->sequence == 1);

    const auto state_request = make_state_request(start);
    const auto state_wire = nstu::exam::encode_state_request(state_request);
    assert(!state_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_state_request, 11,
                         state_wire, &error));
    const auto state_response = channel->receive(&error);
    assert(state_response.has_value());
    assert(state_response->envelope.type ==
           nstu::protocol::CommandType::exam_state_response);
    const auto decoded_state =
        nstu::exam::decode_state_response(state_response->payload);
    assert(decoded_state.has_value());
    assert(decoded_state->session_id == start.session_id);
    assert(decoded_state->answers.size() == 1);

    // The synchronized start is gated on readiness. Releasing a client that has
    // not reported exam_ready would hand it a running clock while its package is
    // still staging, so begin_exam must refuse until the report arrives.
    error.clear();
    assert(!control_plane.begin_exam(registry_id, 3600, &error));
    assert(!error.empty());

    // Synchronized-start barrier: a matching exam_ready marks the client ready,
    // and begin_exam then sends it an authoritative exam_begin.
    nstu::exam::ExamReadyReport ready;
    ready.package_id = start.package_id;
    ready.candidate_id = start.candidate_id;
    ready.package_digest = start.package_digest;
    ready.client_id = start.client_id;
    ready.session_id = start.session_id;
    const auto ready_wire = nstu::exam::encode_exam_ready_report(ready);
    assert(!ready_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_ready, 12, ready_wire,
                         &error));
    assert(wait_until([&] {
        const auto clients = control_plane.ready_exam_clients();
        return std::find(clients.begin(), clients.end(), registry_id) !=
               clients.end();
    }));
    assert(control_plane.begin_exam(registry_id, 3600, &error));
    const auto begin_command = channel->receive(&error);
    assert(begin_command.has_value());
    assert(begin_command->envelope.type ==
           nstu::protocol::CommandType::exam_begin);
    const auto decoded_begin =
        nstu::exam::decode_exam_begin_command(begin_command->payload);
    assert(decoded_begin.has_value());
    assert(decoded_begin->package_id == start.package_id);
    assert(decoded_begin->session_id == start.session_id);
    assert(decoded_begin->duration_seconds == 3600);
    assert(decoded_begin->server_start_unix_milliseconds != 0);

    // The release is single-use. A repeated begin_exam would mint a later start
    // timestamp and silently restart the candidate's clock, desynchronizing the
    // cohort the barrier exists to keep together, so it must be refused.
    error.clear();
    assert(!control_plane.begin_exam(registry_id, 3600, &error));
    assert(!error.empty());

    const auto accepted_hash = nstu::exam::hash_answer_event(accepted_event);
    assert(accepted_hash.has_value());

    // A mismatched package ID is not allowed to reach the journal, even when
    // the embedded digest, client identity, session, and candidate fields are
    // otherwise valid. The server closes the authenticated channel on this
    // violation.
    auto mismatched_event = make_event(start, 2);
    mismatched_event.package_id = "another-package";
    const auto mismatched_wire =
        nstu::exam::encode_answer_event(mismatched_event);
    assert(!mismatched_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_answer_event, 13,
                         mismatched_wire, &error));
    error.clear();
    assert(!channel->receive(&error).has_value());
    channel->close();

    // The active context survives an ordinary reconnect. A valid event for
    // the original tuple is still accepted on the new authenticated channel.
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::offline;
    }));
    auto reconnected = connect_channel(control_plane, id, key_id, key, &error);
    assert(reconnected.has_value());
    assert(wait_until([&] { return registry.snapshot().size() == 1; }));

    const auto reconnected_enable = reconnected->receive(&error);
    assert(reconnected_enable.has_value());
    assert(reconnected_enable->envelope.type ==
           nstu::protocol::CommandType::exam_telemetry_enable);
    const auto decoded_reconnected_enable =
        nstu::exam::decode_telemetry_enable(reconnected_enable->payload);
    assert(decoded_reconnected_enable.has_value());
    assert(decoded_reconnected_enable->client_id == id);
    assert(decoded_reconnected_enable->package_id == start.package_id);
    assert(decoded_reconnected_enable->session_id == start.session_id);
    assert(decoded_reconnected_enable->candidate_id == start.candidate_id);
    assert(decoded_reconnected_enable->server_time_unix_milliseconds != 0);

    auto recovered_event = make_event(start, 2);
    recovered_event.previous_event_hash = *accepted_hash;
    const auto recovered_wire = nstu::exam::encode_answer_event(recovered_event);
    assert(!recovered_wire.empty());
    assert(reconnected->send(nstu::protocol::CommandType::exam_answer_event,
                             20, recovered_wire, &error));
    const auto recovered_ack = reconnected->receive(&error);
    assert(recovered_ack.has_value());
    const auto decoded_recovered_ack =
        nstu::exam::decode_answer_ack(recovered_ack->payload);
    assert(decoded_recovered_ack.has_value());
    assert(decoded_recovered_ack->status ==
           nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_recovered_ack->sequence == 2);

    assert(control_plane.stop_exam(registry_id, &error));
    const auto stop_command = reconnected->receive(&error);
    assert(stop_command.has_value());
    assert(stop_command->envelope.type ==
           nstu::protocol::CommandType::exam_stop);

    // Reconnecting after explicit stop receives no telemetry enable
    reconnected->close();
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::offline;
    }));
    reconnected = connect_channel(control_plane, id, key_id, key, &error);
    assert(reconnected.has_value());
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::online;
    }));

    // Once explicitly stopped, even a correctly shaped event is rejected.
    auto stopped_event = make_event(start, 3);
    const auto stopped_wire = nstu::exam::encode_answer_event(stopped_event);
    assert(!stopped_wire.empty());
    assert(reconnected->send(nstu::protocol::CommandType::exam_answer_event,
                             21, stopped_wire, &error));
    error.clear();
    assert(!reconnected->receive(&error).has_value());
    reconnected->close();

    // --- 25-Hour Expiry Without Deadline Renewal and Preserved Answer Recovery ---
    auto start2 = make_start(id);
    for (std::size_t i = 0; i < start2.session_id.size(); ++i) {
        start2.session_id[i] = static_cast<std::byte>(0xa5 + i);
    }

    auto fresh_ch = connect_channel(control_plane, id, key_id, key, &error);
    assert(fresh_ch.has_value());
    // A report from this connection is processed after its handshake demotes
    // the previous session's UWF proof; online alone is published too early.
    nstu::control::ClientStatusReport fresh_status;
    fresh_status.hostname = "AUTH-TEST-FRESH-PC";
    assert(fresh_ch->send(nstu::protocol::CommandType::status_report, 1,
                          nstu::control::encode_status_report(fresh_status),
                          &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::online &&
               snapshot.front().hostname == fresh_status.hostname;
    }));
    assert(registry.set_uwf_fleet_status(registry_id, uwf_verified));

    assert(control_plane.start_exam(registry_id, start2, &error));
    const auto s2_cmd = fresh_ch->receive(&error);
    assert(s2_cmd.has_value());
    assert(s2_cmd->envelope.type == nstu::protocol::CommandType::exam_start);

    const auto fresh_enable = fresh_ch->receive(&error);
    assert(fresh_enable.has_value());
    assert(fresh_enable->envelope.type == nstu::protocol::CommandType::exam_telemetry_enable);

    fresh_ch->close();
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::offline;
    }));

    // Advance clock past 25 hours (26 hours)
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::hours(26));

    // Reconnect after 25-hour telemetry deadline
    auto expired_ch = connect_channel(control_plane, id, key_id, key, &error);
    assert(expired_ch.has_value());
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::online;
    }));

    // Submit answer event
    auto answer_event_exp = make_event(start2, 1);
    const auto answer_wire_exp = nstu::exam::encode_answer_event(answer_event_exp);
    assert(!answer_wire_exp.empty());
    assert(expired_ch->send(nstu::protocol::CommandType::exam_answer_event, 40,
                            answer_wire_exp, &error));

    // The FIRST message received MUST be the answer ACK, NOT telemetry enable (telemetry expired)!
    const auto ack_exp = expired_ch->receive(&error);
    assert(ack_exp.has_value());
    assert(ack_exp->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
    const auto dec_ack_exp = nstu::exam::decode_answer_ack(ack_exp->payload);
    assert(dec_ack_exp.has_value());
    assert(dec_ack_exp->status == nstu::exam::AnswerAckStatus::accepted);

    // Answer recovery via state request still works!
    const auto state_req_exp = make_state_request(start2);
    const auto state_wire_exp = nstu::exam::encode_state_request(state_req_exp);
    assert(expired_ch->send(nstu::protocol::CommandType::exam_state_request, 41,
                            state_wire_exp, &error));
    const auto state_resp_exp = expired_ch->receive(&error);
    assert(state_resp_exp.has_value());
    assert(state_resp_exp->envelope.type == nstu::protocol::CommandType::exam_state_response);
    const auto dec_state_exp = nstu::exam::decode_state_response(state_resp_exp->payload);
    assert(dec_state_exp.has_value());
    assert(dec_state_exp->session_id == start2.session_id);
    assert(dec_state_exp->answers.size() == 1);
    assert(dec_state_exp->answers.front().answer == answer_event_exp.answer);
    assert(dec_state_exp->answers.front().event_hash == *nstu::exam::hash_answer_event(answer_event_exp));

    // Extreme offsets saturate instead of overflowing: INT64_MIN must not be
    // negated, and a large forward offset must not wrap past zero.
    nstu::server::set_telemetry_clock_offset_for_testing(
        std::chrono::milliseconds::min());
    assert(nstu::server::telemetry_server_time_ms_for_testing() == 0);
    nstu::server::set_telemetry_clock_offset_for_testing(
        std::chrono::milliseconds::max());
    assert(nstu::server::telemetry_server_time_ms_for_testing() >
           static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()));
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::hours(-1));
    const auto behind_ms = nstu::server::telemetry_server_time_ms_for_testing();
    nstu::server::set_telemetry_clock_offset_for_testing(std::chrono::milliseconds(0));
    const auto real_ms = nstu::server::telemetry_server_time_ms_for_testing();
    assert(behind_ms != 0 && behind_ms < real_ms &&
           real_ms - behind_ms >= 3'600'000ULL);

    expired_ch->close();

    // Every connection opened so far (channel, two reconnected, fresh_ch,
    // expired_ch) has closed. Wait for all five on_disconnected callbacks to
    // return so the churn below can count its own disconnects exactly.
    assert(wait_until([&] {
        return nstu::server::completed_disconnect_count_for_testing() == 5;
    }));

    // Immediate reconnects: the superseded connection's disconnect races the
    // replacement handshake. Each round waits until the old connection's
    // on_disconnected has returned, then asserts the replacement is online with
    // its proof intact, whichever order the two landed in.
    std::optional<nstu::control::AuthenticatedControlChannel> previous_ch;
    for (int round = 0; round < 20; ++round) {
        const auto disconnects_before =
            nstu::server::completed_disconnect_count_for_testing();
        if (previous_ch) {
            previous_ch->close();
        }
        auto round_ch = connect_channel(control_plane, id, key_id, key, &error);
        assert(round_ch.has_value());
        nstu::control::ClientStatusReport round_status;
        round_status.hostname = "AUTH-TEST-ROUND-" + std::to_string(round);
        assert(round_ch->send(nstu::protocol::CommandType::status_report, 1,
                              nstu::control::encode_status_report(round_status),
                              &error));
        assert(wait_until([&] {
            const auto snapshot = registry.snapshot();
            return !snapshot.empty() &&
                   snapshot.front().status == nstu::server::ClientStatus::online &&
                   snapshot.front().hostname == round_status.hostname;
        }));
        assert(registry.set_uwf_fleet_status(registry_id, uwf_verified));

        // A second report on the same connection is a server round trip after
        // the proof was recorded; the proof and online status must survive it.
        round_status.hostname += "-CONFIRMED";
        assert(round_ch->send(nstu::protocol::CommandType::status_report, 2,
                              nstu::control::encode_status_report(round_status),
                              &error));
        assert(wait_until([&] {
            const auto snapshot = registry.snapshot();
            return !snapshot.empty() &&
                   snapshot.front().hostname == round_status.hostname;
        }));
        // Barrier: the superseded connection's on_disconnected has returned.
        // Exactly one disconnect per round after the first; none in round 0.
        const auto expected_disconnects =
            disconnects_before + (round == 0 ? 0u : 1u);
        assert(wait_until([&] {
            return nstu::server::completed_disconnect_count_for_testing() ==
                   expected_disconnects;
        }));
        const auto confirmed = registry.snapshot();
        assert(confirmed.size() == 1);
        assert(confirmed.front().status == nstu::server::ClientStatus::online);
        assert(confirmed.front().uwf.proves_current_protection());
        previous_ch = std::move(round_ch);
    }

    // Forced late disconnect: hold disconnect callbacks, let a replacement
    // supersede the still-open previous connection and record fresh proof, and
    // only then let the old connection's on_disconnected run.
    nstu::server::set_disconnect_hold_for_testing(true);
    const auto disconnects_before_late =
        nstu::server::completed_disconnect_count_for_testing();
    auto late_ch = connect_channel(control_plane, id, key_id, key, &error);
    assert(late_ch.has_value());
    nstu::control::ClientStatusReport late_status;
    late_status.hostname = "AUTH-TEST-LATE-DISCONNECT";
    assert(late_ch->send(nstu::protocol::CommandType::status_report, 1,
                         nstu::control::encode_status_report(late_status),
                         &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::online &&
               snapshot.front().hostname == late_status.hostname;
    }));
    assert(registry.set_uwf_fleet_status(registry_id, uwf_verified));
    // The supersede closed the old connection, but its callback is parked.
    assert(wait_until([] {
        return nstu::server::held_disconnect_count_for_testing() == 1;
    }));
    assert(nstu::server::completed_disconnect_count_for_testing() ==
           disconnects_before_late);
    const auto before_late = registry.snapshot();
    assert(before_late.size() == 1);
    assert(before_late.front().uwf.proves_current_protection());

    nstu::server::set_disconnect_hold_for_testing(false);
    assert(nstu::server::release_held_disconnects_for_testing() == 1);
    assert(nstu::server::completed_disconnect_count_for_testing() ==
           disconnects_before_late + 1);
    const auto after_late = registry.snapshot();
    assert(after_late.size() == 1);
    assert(after_late.front().status == nstu::server::ClientStatus::online);
    assert(after_late.front().uwf.proves_current_protection());
    assert(control_plane.authenticated_client_count() == 1);

    // Control: the live connection's own disconnect still demotes the proof.
    late_ch->close();
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::offline &&
               !snapshot.front().uwf.proves_current_protection();
    }));
    previous_ch->close();

    control_plane.stop();
    nstu::security::secure_zero(key);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    auto lock_path = path;
    lock_path += L".lock";
    std::filesystem::remove(lock_path, cleanup_error);
    test_wall_expiry_then_rollback_preserves_answers();
    test_fixed_deadline_boundaries_and_wrong_client();
    test_authenticated_ownership_regressions();
    return 0;
}
