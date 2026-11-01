#include "nstu/client_control.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/exam_telemetry.hpp"
#include "nstu/exam_telemetry_journal.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(0x30u + index);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x80u + index);
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

std::filesystem::path unique_temp_dir(const std::string& prefix) {
    const auto tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto p = std::filesystem::temp_directory_path() / (prefix + "-" + std::to_string(tick));
    std::filesystem::create_directories(p);
    return p;
}

nstu::exam::ExamStartRequest make_start_request(const nstu::security::ClientId& id) {
    nstu::exam::ExamStartRequest request;
    request.package_root = "C:/ProgramData/NSTU/exams/integration-test";
    request.web_root = "C:/ProgramData/NSTU/exams/integration-test/exam/web";
    request.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
    request.package_id = "integration-test-pkg";
    request.candidate_id = "candidate-integ-01";
    request.client_id = id;
    for (std::size_t index = 0; index < request.package_digest.size(); ++index) {
        request.package_digest[index] = static_cast<std::byte>(0x55u + index);
    }
    for (std::size_t index = 0; index < request.session_id.size(); ++index) {
        request.session_id[index] = static_cast<std::byte>(0xaau + index);
    }
    return request;
}

nstu::control::UwfFleetStatusReport verified_uwf_proof() {
    nstu::control::UwfFleetStatusReport proof;
    proof.phase = nstu::control::UwfFleetPhase::verified_protected;
    proof.probe.probe_succeeded = true;
    proof.probe.filter_current_enabled = true;
    proof.probe.system_volume_current_protected = true;
    proof.boot_id.fill(std::byte{0xb5});
    return proof;
}

std::optional<nstu::control::AuthenticatedControlChannel> connect_channel(
    nstu::server::ServerControlPlane& control_plane,
    const nstu::security::ClientId& id,
    std::uint32_t key_id,
    std::span<const std::byte> key,
    std::string* error) {
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
    return nstu::control::AuthenticatedControlChannel(std::move(socket), std::move(*session));
}

nstu::exam::AnswerEvent make_answer(
    const nstu::exam::ExamStartRequest& start,
    std::uint64_t sequence,
    std::string_view answer_text) {
    nstu::exam::AnswerEvent event;
    event.package_id = start.package_id;
    event.package_digest = start.package_digest;
    event.client_id = start.client_id;
    event.session_id = start.session_id;
    event.candidate_id = start.candidate_id;
    event.question_id = "question-01";
    event.question_revision = 1;
    event.sequence = sequence;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL + sequence;
    event.kind = nstu::exam::AnswerKind::upsert;
    for (char ch : answer_text) {
        event.answer.push_back(static_cast<std::byte>(ch));
    }
    return event;
}

nstu::exam::TelemetryBatch make_telemetry_batch(
    const nstu::exam::ExamStartRequest& start,
    const nstu::exam::ProducerId& producer,
    std::uint64_t sequence,
    nstu::exam::TelemetrySampleKind kind = nstu::exam::TelemetrySampleKind::question_dwell) {
    nstu::exam::TelemetryBatch batch;
    batch.package_id = start.package_id;
    batch.package_digest = start.package_digest;
    batch.client_id = start.client_id;
    batch.session_id = start.session_id;
    batch.candidate_id = start.candidate_id;
    batch.producer_id = producer;
    batch.sequence = sequence;
    batch.first_enqueued_at_unix_milliseconds = 1'700'000'000'000ULL + sequence;

    nstu::exam::TelemetrySample sample;
    sample.kind = kind;
    sample.source = nstu::exam::TelemetrySource::native;
    sample.collector_id = 1;
    sample.monotonic_elapsed_milliseconds = 100 * sequence;
    sample.duration_milliseconds = 100;
    sample.client_time_unix_milliseconds = 1'700'000'000'000ULL + sequence;
    sample.numeric_value = static_cast<std::uint32_t>(sequence);
    sample.count = 1;
    sample.reason = nstu::exam::TelemetryReason::none;
    batch.samples.push_back(sample);
    return batch;
}

bool file_contains_bytes(const std::filesystem::path& file_path, std::string_view needle) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content.find(needle) != std::string::npos;
}

bool directory_contains_bytes(const std::filesystem::path& dir_path, std::string_view needle) {
    if (!std::filesystem::exists(dir_path)) {
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
        if (entry.is_regular_file()) {
            if (file_contains_bytes(entry.path(), needle)) {
                return true;
            }
        }
    }
    return false;
}

struct TestHarness {
    nstu::security::ClientId id = client_id();
    std::vector<std::byte> key = client_key();
    constexpr static std::uint32_t kKeyId = 9;
    nstu::security::KeyStore store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane plane{registry, store};
    std::filesystem::path base_temp_dir;
    std::filesystem::path answer_journal_file;
    std::filesystem::path telemetry_journal_dir;
    nstu::exam::ExamStartRequest start = make_start_request(id);
    nstu::exam::ProducerId producer{};
    std::uint64_t registry_id = 0;
    std::string error;
    std::optional<nstu::control::AuthenticatedControlChannel> channel;

    explicit TestHarness(bool enable_telemetry = true)
        : base_temp_dir(unique_temp_dir("nstu-integ-test")),
          answer_journal_file(base_temp_dir / "answers.bin"),
          telemetry_journal_dir(base_temp_dir / "telemetry-journal") {
        producer[0] = std::byte{0x77};
        producer[1] = std::byte{0x88};
        assert(store.enroll(id, kKeyId, key, &error));

        nstu::server::ServerControlPlaneConfig config;
        config.port = 0;
        config.maximum_clients = 16;
        config.exam_journal_path = answer_journal_file;
        if (enable_telemetry) {
            config.telemetry_journal_path = telemetry_journal_dir;
        }
        assert(plane.start(std::move(config), &error));
        reconnect("INTEG-CLIENT-01");
    }

    ~TestHarness() {
        if (channel) {
            channel->close();
        }
        plane.stop();
        nstu::security::secure_zero(key);
        std::error_code ec;
        std::filesystem::remove_all(base_temp_dir, ec);
    }

    void reconnect(const std::string& label) {
        if (channel) {
            channel->close();
        }
        channel = connect_channel(plane, id, kKeyId, key, &error);
        assert(channel.has_value());
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

    void start_active_exam(bool expect_telemetry_enable = true) {
        assert(registry.set_uwf_fleet_status(registry_id, verified_uwf_proof()));
        assert(plane.start_exam(registry_id, start, &error));

        const auto start_cmd = channel->receive(&error);
        assert(start_cmd.has_value());
        assert(start_cmd->envelope.type == nstu::protocol::CommandType::exam_start);

        if (expect_telemetry_enable) {
            const auto enable_cmd = channel->receive(&error);
            assert(enable_cmd.has_value());
            assert(enable_cmd->envelope.type == nstu::protocol::CommandType::exam_telemetry_enable);
        }
    }
};

// 1. Answer Equivalence and Channel Isolation under Telemetry Flood
void test_telemetry_flood_and_answer_isolation() {
    TestHarness harness(true);
    harness.start_active_exam(true);

    const std::string canary_answer_1 = "CANARY_STUDENT_ANSWER_PAYLOAD_1";
    const std::string canary_answer_2 = "CANARY_STUDENT_ANSWER_PAYLOAD_2";

    // Submit Answer 1
    const auto ans1 = make_answer(harness.start, 1, canary_answer_1);
    assert(harness.channel->send(nstu::protocol::CommandType::exam_answer_event, 101,
                                 nstu::exam::encode_answer_event(ans1), &harness.error));
    const auto ack1 = harness.channel->receive(&harness.error);
    assert(ack1.has_value() && ack1->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
    const auto decoded_ack1 = nstu::exam::decode_answer_ack(ack1->payload);
    assert(decoded_ack1.has_value() && decoded_ack1->status == nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_ack1->sequence == 1);

    // Flood with telemetry batches (rate limit threshold is burst=4, refill=2/s)
    bool saw_telemetry_rate_limit = false;
    for (std::uint64_t seq = 1; seq <= 25; ++seq) {
        const auto batch = make_telemetry_batch(harness.start, harness.producer, seq);
        assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch,
                                     200 + seq,
                                     nstu::exam::encode_telemetry_batch(batch),
                                     &harness.error));
        const auto ack_frame = harness.channel->receive(&harness.error);
        assert(ack_frame.has_value() &&
               ack_frame->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
        const auto ack = nstu::exam::decode_telemetry_ack(ack_frame->payload);
        assert(ack.has_value());
        if (ack->status == nstu::exam::TelemetryAckStatus::unavailable) {
            saw_telemetry_rate_limit = true;
        }
    }
    assert(saw_telemetry_rate_limit);

    // Immediately submit Answer 2 while telemetry queue / rate limiter is saturated
    // Link to previous event hash to preserve chain continuity
    auto ans2 = make_answer(harness.start, 2, canary_answer_2);
    const auto h1 = nstu::exam::hash_answer_event(ans1);
    assert(h1.has_value());
    ans2.previous_event_hash = *h1;

    assert(harness.channel->send(nstu::protocol::CommandType::exam_answer_event, 102,
                                 nstu::exam::encode_answer_event(ans2), &harness.error));
    const auto ack2 = harness.channel->receive(&harness.error);
    assert(ack2.has_value() && ack2->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
    const auto decoded_ack2 = nstu::exam::decode_answer_ack(ack2->payload);
    assert(decoded_ack2.has_value() && decoded_ack2->status == nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_ack2->sequence == 2);

    // Query exam state request
    nstu::exam::StateRequest state_req;
    state_req.package_id = harness.start.package_id;
    state_req.package_digest = harness.start.package_digest;
    state_req.client_id = harness.start.client_id;
    state_req.session_id = harness.start.session_id;
    state_req.candidate_id = harness.start.candidate_id;
    assert(harness.channel->send(nstu::protocol::CommandType::exam_state_request, 103,
                                 nstu::exam::encode_state_request(state_req), &harness.error));
    const auto state_resp_frame = harness.channel->receive(&harness.error);
    assert(state_resp_frame.has_value() &&
           state_resp_frame->envelope.type == nstu::protocol::CommandType::exam_state_response);
    const auto state_resp = nstu::exam::decode_state_response(state_resp_frame->payload);
    assert(state_resp.has_value());
    assert(state_resp->highest_contiguous_sequence == 2);

    // Stop exam
    assert(harness.plane.stop_exam(harness.registry_id, &harness.error));
    const auto stop_frame = harness.channel->receive(&harness.error);
    assert(stop_frame.has_value() && stop_frame->envelope.type == nstu::protocol::CommandType::exam_stop);

    // Stop server and directly inspect the persisted AnswerJournal
    harness.channel->close();
    harness.plane.stop();

    nstu::exam::AnswerJournal direct_journal;
    assert(direct_journal.open(harness.answer_journal_file, &harness.error));
    const auto final_state = direct_journal.state(state_req, &harness.error);
    assert(final_state.has_value());
    assert(final_state->highest_contiguous_sequence == 2);
    assert(final_state->answers.size() == 1); // same question upserted
    std::string recovered_answer_text;
    for (auto b : final_state->answers[0].answer) {
        recovered_answer_text.push_back(static_cast<char>(b));
    }
    assert(recovered_answer_text == canary_answer_2);
    direct_journal.close();
}

// 2. Replay Semantics: Disconnect vs. Explicit Stop
void test_replay_semantics_disconnect_vs_stop() {
    TestHarness harness(true);
    harness.start_active_exam(true);

    // Initial batch 1 accepted
    const auto b1 = make_telemetry_batch(harness.start, harness.producer, 1);
    assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 301,
                                 nstu::exam::encode_telemetry_batch(b1), &harness.error));
    const auto ack1 = harness.channel->receive(&harness.error);
    assert(ack1.has_value() && ack1->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    const auto d_ack1 = nstu::exam::decode_telemetry_ack(ack1->payload);
    assert(d_ack1.has_value() && d_ack1->status == nstu::exam::TelemetryAckStatus::accepted);

    // Simulate unexpected network disconnect
    harness.channel->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client reconnects under the same exam session
    harness.reconnect("INTEG-CLIENT-01");

    // Server re-delivers matching exam_telemetry_enable on reconnect
    const auto re_enable = harness.channel->receive(&harness.error);
    assert(re_enable.has_value() &&
           re_enable->envelope.type == nstu::protocol::CommandType::exam_telemetry_enable);
    const auto d_enable = nstu::exam::decode_telemetry_enable(re_enable->payload);
    assert(d_enable.has_value());
    assert(d_enable->session_id == harness.start.session_id);

    // Replay batch 1 after reconnect: exact duplicate is recognized as duplicate
    assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 302,
                                 nstu::exam::encode_telemetry_batch(b1), &harness.error));
    const auto ack_replay = harness.channel->receive(&harness.error);
    assert(ack_replay.has_value() &&
           ack_replay->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    const auto d_replay = nstu::exam::decode_telemetry_ack(ack_replay->payload);
    assert(d_replay.has_value() && d_replay->status == nstu::exam::TelemetryAckStatus::duplicate);

    // Send batch 2 after reconnect: accepted
    const auto b2 = make_telemetry_batch(harness.start, harness.producer, 2);
    assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 303,
                                 nstu::exam::encode_telemetry_batch(b2), &harness.error));
    const auto ack2 = harness.channel->receive(&harness.error);
    assert(ack2.has_value() && ack2->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    const auto d_ack2 = nstu::exam::decode_telemetry_ack(ack2->payload);
    assert(d_ack2.has_value() && d_ack2->status == nstu::exam::TelemetryAckStatus::accepted);

    // Now issue explicit exam stop
    assert(harness.plane.stop_exam(harness.registry_id, &harness.error));
    const auto stop_frame = harness.channel->receive(&harness.error);
    assert(stop_frame.has_value() && stop_frame->envelope.type == nstu::protocol::CommandType::exam_stop);

    // After explicit stop: any batch (new or replay) is rejected terminal
    const auto b3 = make_telemetry_batch(harness.start, harness.producer, 3);
    assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 304,
                                 nstu::exam::encode_telemetry_batch(b3), &harness.error));
    const auto ack3 = harness.channel->receive(&harness.error);
    assert(ack3.has_value() && ack3->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    const auto d_ack3 = nstu::exam::decode_telemetry_ack(ack3->payload);
    assert(d_ack3.has_value() && d_ack3->status == nstu::exam::TelemetryAckStatus::rejected);

    // Replay of b2 after stop is also rejected
    assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 305,
                                 nstu::exam::encode_telemetry_batch(b2), &harness.error));
    const auto ack_b2_stop = harness.channel->receive(&harness.error);
    assert(ack_b2_stop.has_value() &&
           ack_b2_stop->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    const auto d_b2_stop = nstu::exam::decode_telemetry_ack(ack_b2_stop->payload);
    assert(d_b2_stop.has_value() && d_b2_stop->status == nstu::exam::TelemetryAckStatus::rejected);
}

// 3. Canary Leakage Absence (Zero Leakage Privacy)
void test_canary_leakage_absence() {
    TestHarness harness(true);
    harness.start_active_exam(true);

    const std::string kCanaryAnswer = "CANARY_CONFIDENTIAL_ANSWER_TOKEN_99999";
    const std::string kCanaryNote = "CANARY_STUDENT_PRIVATE_NOTE_TOKEN_88888";
    const std::string kCanaryClipboard = "CANARY_PASTE_CLIPBOARD_SECRET_TOKEN_77777";

    // Send answer containing canary answer token
    const auto ans = make_answer(harness.start, 1, kCanaryAnswer);
    assert(harness.channel->send(nstu::protocol::CommandType::exam_answer_event, 401,
                                 nstu::exam::encode_answer_event(ans), &harness.error));
    const auto ans_ack = harness.channel->receive(&harness.error);
    assert(ans_ack.has_value() && ans_ack->envelope.type == nstu::protocol::CommandType::exam_answer_ack);

    // Send diverse telemetry samples (timing, scroll, focus, visibility)
    const std::vector<nstu::exam::TelemetrySampleKind> sample_kinds = {
        nstu::exam::TelemetrySampleKind::collector_started,
        nstu::exam::TelemetrySampleKind::question_first_view,
        nstu::exam::TelemetrySampleKind::question_dwell,
        nstu::exam::TelemetrySampleKind::focus_changed,
        nstu::exam::TelemetrySampleKind::visibility_changed,
    };

    for (std::size_t i = 0; i < sample_kinds.size(); ++i) {
        const auto b = make_telemetry_batch(harness.start, harness.producer, i + 1, sample_kinds[i]);
        assert(harness.channel->send(nstu::protocol::CommandType::exam_telemetry_batch,
                                     500 + i,
                                     nstu::exam::encode_telemetry_batch(b),
                                     &harness.error));
        const auto ack = harness.channel->receive(&harness.error);
        assert(ack.has_value() && ack->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
    }

    // Stop exam and server to ensure all persistence workers flush and close files
    assert(harness.plane.stop_exam(harness.registry_id, &harness.error));
    const auto stop_frame = harness.channel->receive(&harness.error);
    assert(stop_frame.has_value() && stop_frame->envelope.type == nstu::protocol::CommandType::exam_stop);

    harness.channel->close();
    harness.plane.stop();

    // Verify: answer journal DOES contain the canary answer
    assert(file_contains_bytes(harness.answer_journal_file, kCanaryAnswer));

    // Verify: telemetry journal directory NEVER contains any canary strings
    assert(!directory_contains_bytes(harness.telemetry_journal_dir, kCanaryAnswer));
    assert(!directory_contains_bytes(harness.telemetry_journal_dir, kCanaryNote));
    assert(!directory_contains_bytes(harness.telemetry_journal_dir, kCanaryClipboard));

    // Verify: audit directory (if exists) NEVER contains canary strings
    const auto audit_dir = harness.answer_journal_file.parent_path() / "audit";
    if (std::filesystem::exists(audit_dir)) {
        assert(!directory_contains_bytes(audit_dir, kCanaryAnswer));
        assert(!directory_contains_bytes(audit_dir, kCanaryNote));
        assert(!directory_contains_bytes(audit_dir, kCanaryClipboard));
    }
}

// 4. Mixed-Version and Disabled Compatibility
void test_mixed_version_and_disabled_telemetry() {
    // Subcase 1: Server has telemetry disabled (telemetry_journal_path = "")
    {
        TestHarness disabled_server(false);
        // Start exam on server with telemetry disabled: receives start + enable
        disabled_server.start_active_exam(true);

        // Regular answer flows unimpeded
        const auto ans1 = make_answer(disabled_server.start, 1, "ORDINARY_ANSWER_DATA");
        assert(disabled_server.channel->send(nstu::protocol::CommandType::exam_answer_event, 601,
                                             nstu::exam::encode_answer_event(ans1),
                                             &disabled_server.error));
        const auto ans_ack1 = disabled_server.channel->receive(&disabled_server.error);
        assert(ans_ack1.has_value() && ans_ack1->envelope.type == nstu::protocol::CommandType::exam_answer_ack);

        // If client sends telemetry batch to disabled server, server returns unavailable without drop
        const auto batch = make_telemetry_batch(disabled_server.start, disabled_server.producer, 1);
        assert(disabled_server.channel->send(nstu::protocol::CommandType::exam_telemetry_batch, 602,
                                             nstu::exam::encode_telemetry_batch(batch),
                                             &disabled_server.error));
        const auto tele_ack = disabled_server.channel->receive(&disabled_server.error);
        assert(tele_ack.has_value() &&
               tele_ack->envelope.type == nstu::protocol::CommandType::exam_telemetry_ack);
        const auto decoded_ack = nstu::exam::decode_telemetry_ack(tele_ack->payload);
        assert(decoded_ack.has_value());
        assert(decoded_ack->status == nstu::exam::TelemetryAckStatus::unavailable);

        // Subsequent answer flow remains active (linked to previous event hash)
        auto ans2 = make_answer(disabled_server.start, 2, "ORDINARY_ANSWER_DATA_2");
        const auto h1 = nstu::exam::hash_answer_event(ans1);
        assert(h1.has_value());
        ans2.previous_event_hash = *h1;

        assert(disabled_server.channel->send(nstu::protocol::CommandType::exam_answer_event, 603,
                                             nstu::exam::encode_answer_event(ans2),
                                             &disabled_server.error));
        const auto ans_ack2 = disabled_server.channel->receive(&disabled_server.error);
        assert(ans_ack2.has_value() && ans_ack2->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
    }

    // Subcase 2: Old client ignores telemetry enable on upgraded server
    {
        TestHarness enabled_server(true);
        enabled_server.start_active_exam(true); // receives start + enable

        // Client never sends any telemetry, only answers
        nstu::security::Sha256Digest prev_hash{};
        for (std::uint64_t seq = 1; seq <= 5; ++seq) {
            auto ans = make_answer(enabled_server.start, seq, "OLD_CLIENT_ANSWER");
            ans.previous_event_hash = prev_hash;
            const auto current_hash = nstu::exam::hash_answer_event(ans);
            assert(current_hash.has_value());
            prev_hash = *current_hash;

            assert(enabled_server.channel->send(nstu::protocol::CommandType::exam_answer_event, 700 + seq,
                                                 nstu::exam::encode_answer_event(ans),
                                                 &enabled_server.error));
            const auto ack = enabled_server.channel->receive(&enabled_server.error);
            assert(ack.has_value() && ack->envelope.type == nstu::protocol::CommandType::exam_answer_ack);
            const auto d_ack = nstu::exam::decode_answer_ack(ack->payload);
            assert(d_ack.has_value() && d_ack->status == nstu::exam::AnswerAckStatus::accepted);
            assert(d_ack->sequence == seq);
        }

        assert(enabled_server.plane.stop_exam(enabled_server.registry_id, &enabled_server.error));
        const auto stop = enabled_server.channel->receive(&enabled_server.error);
        assert(stop.has_value() && stop->envelope.type == nstu::protocol::CommandType::exam_stop);
    }
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    test_telemetry_flood_and_answer_isolation();
    test_replay_semantics_disconnect_vs_stop();
    test_canary_leakage_absence();
    test_mixed_version_and_disabled_telemetry();

    std::puts("All exam telemetry cross-channel integration tests passed.");
    return 0;
}
