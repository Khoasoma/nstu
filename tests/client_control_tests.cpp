#include "nstu/auth.hpp"
#include "nstu/secret_store.hpp"
#include "nstu/client_control.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_telemetry.hpp"
#include "nstu/exam_telemetry_router.hpp"
#include "nstu/multicast.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

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

} // namespace

void test_exam_telemetry_router() {
    // 1. Malformed payloads
    {
        nstu::client::ExamTelemetryRouter router;
        nstu::client::ExamTelemetryRouterCallbacks callbacks;
        bool on_enable_called = false;
        callbacks.on_enable = [&](const nstu::exam::TelemetryEnable&, std::string*) {
            on_enable_called = true;
            return true;
        };
        bool agent_enable_called = false;
        callbacks.queue_agent_enable = [&](std::span<const std::byte>) {
            agent_enable_called = true;
        };
        bool ingest_called = false;
        callbacks.ingest_samples = [&](const std::vector<nstu::exam::TelemetrySample>&, std::uint64_t, std::string*) {
            ingest_called = true;
            return true;
        };
        bool ack_called = false;
        callbacks.on_ack = [&](const nstu::exam::TelemetryAck&, std::uint64_t, std::string*) {
            ack_called = true;
            return nstu::exam::TelemetryAckApplyResult::retired;
        };
        router.set_callbacks(std::move(callbacks));

        std::vector<std::byte> malformed = {std::byte{0xde}, std::byte{0xad}};
        std::string error;
        assert(!router.handle_server_enable(malformed, &error));
        assert(!error.empty());
        assert(!on_enable_called);
        assert(!agent_enable_called);

        error.clear();
        assert(!router.handle_agent_samples(malformed, 1000, &error));
        assert(!error.empty());
        assert(!ingest_called);

        error.clear();
        assert(router.handle_server_ack(malformed, 1000, &error) == nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
        assert(!error.empty());
        assert(!ack_called);
    }

    // 2. Enable handling: accepted vs stale/mismatched rejection
    {
        nstu::exam::TelemetryEnable enable;
        enable.package_id = "pkg-1";
        enable.package_digest.fill(std::byte{1});
        enable.client_id.fill(std::byte{2});
        enable.session_id.fill(std::byte{3});
        enable.candidate_id = "cand-1";
        const auto raw_enable = nstu::exam::encode_telemetry_enable(enable);
        assert(!raw_enable.empty());

        // A. Stale context rejected by coordinator callback: MUST NOT forward to agent
        {
            nstu::client::ExamTelemetryRouter router;
            bool on_enable_called = false;
            bool agent_enable_called = false;
            nstu::client::ExamTelemetryRouterCallbacks cb;
            cb.on_enable = [&](const nstu::exam::TelemetryEnable& e, std::string* err) {
                on_enable_called = true;
                assert(e.package_id == "pkg-1");
                if (err) *err = "stale context rejected";
                return false; // rejected
            };
            cb.queue_agent_enable = [&](std::span<const std::byte>) {
                agent_enable_called = true;
            };
            router.set_callbacks(std::move(cb));

            std::string err;
            assert(!router.handle_server_enable(raw_enable, &err));
            assert(on_enable_called);
            assert(!agent_enable_called); // NOT forwarded!
        }

        // B. Accepted context: coordinator accepted -> forwarded to agent
        {
            nstu::client::ExamTelemetryRouter router;
            bool on_enable_called = false;
            std::vector<std::byte> forwarded_payload;
            nstu::client::ExamTelemetryRouterCallbacks cb;
            cb.on_enable = [&](const nstu::exam::TelemetryEnable& e, std::string*) {
                on_enable_called = true;
                assert(e.candidate_id == "cand-1");
                return true; // accepted
            };
            cb.queue_agent_enable = [&](std::span<const std::byte> p) {
                forwarded_payload.assign(p.begin(), p.end());
            };
            router.set_callbacks(std::move(cb));

            std::string err;
            assert(router.handle_server_enable(raw_enable, &err));
            assert(on_enable_called);
            assert(forwarded_payload == raw_enable); // exact payload forwarded!
        }
    }

    // 3. Agent samples handling
    {
        nstu::client::ExamTelemetryRouter router;
        std::vector<nstu::exam::TelemetrySample> decoded_received;
        std::uint64_t timestamp_received = 0;
        nstu::client::ExamTelemetryRouterCallbacks cb;
        cb.ingest_samples = [&](const std::vector<nstu::exam::TelemetrySample>& samples, std::uint64_t now_ms, std::string*) {
            decoded_received = samples;
            timestamp_received = now_ms;
            return true;
        };
        router.set_callbacks(std::move(cb));

        nstu::exam::TelemetrySample s;
        s.kind = nstu::exam::TelemetrySampleKind::question_first_view;
        s.source = nstu::exam::TelemetrySource::browser;
        s.collector_id = 5ULL;
        s.question_id = "q1";
        s.question_revision = 1;
        s.monotonic_elapsed_milliseconds = 500;
        s.duration_milliseconds = 200;
        s.client_time_unix_milliseconds = 1700000000000ULL;
        s.numeric_value = 10;
        s.count = 1;
        s.reason = nstu::exam::TelemetryReason::navigation_button;

        const auto test_vec = std::vector<nstu::exam::TelemetrySample>{s};
        const auto raw_samples = nstu::client::encode_exam_telemetry_samples_payload(test_vec);
        assert(!raw_samples.empty());

        std::string err;
        assert(router.handle_agent_samples(raw_samples, 424242, &err));
        assert(timestamp_received == 424242);
        assert(decoded_received.size() == 1);
        assert(decoded_received[0].question_id == "q1");
        assert(decoded_received[0].kind == nstu::exam::TelemetrySampleKind::question_first_view);
    }

    // 4. Server ACK handling
    {
        nstu::client::ExamTelemetryRouter router;
        nstu::exam::TelemetryAck received_ack;
        std::uint64_t ack_ts = 0;
        nstu::client::ExamTelemetryRouterCallbacks cb;
        cb.on_ack = [&](const nstu::exam::TelemetryAck& ack, std::uint64_t now_ms, std::string*) {
            received_ack = ack;
            ack_ts = now_ms;
            return nstu::exam::TelemetryAckApplyResult::retired;
        };
        router.set_callbacks(std::move(cb));

        nstu::exam::TelemetryAck ack;
        ack.status = nstu::exam::TelemetryAckStatus::accepted;
        ack.session_id.fill(std::byte{7});
        ack.producer_id.fill(std::byte{8});
        ack.sequence = 99;
        ack.batch_hash.fill(std::byte{9});
        ack.server_time_unix_milliseconds = 1700000000100ULL;

        const auto raw_ack = nstu::exam::encode_telemetry_ack(ack);
        assert(!raw_ack.empty());

        std::string err;
        assert(router.handle_server_ack(raw_ack, 88888, &err) == nstu::exam::TelemetryAckApplyResult::retired);
        assert(ack_ts == 88888);
        assert(received_ack.sequence == 99);
        assert(received_ack.status == nstu::exam::TelemetryAckStatus::accepted);
    }

    // 5. Lifecycle events: explicit stop, disconnect, shutdown, restart
    {
        nstu::client::ExamTelemetryRouter router;
        bool explicit_stop_called = false;
        bool disconnect_called = false;
        bool shutdown_called = false;
        bool restart_called = false;
        const std::filesystem::path restart_path = "router-callback-telemetry.bin";
        std::filesystem::path restored_path;

        nstu::client::ExamTelemetryRouterCallbacks cb;
        cb.on_explicit_stop = [&]() { explicit_stop_called = true; };
        cb.on_disconnect = [&]() { disconnect_called = true; };
        cb.on_shutdown = [&]() { shutdown_called = true; };
        cb.on_restart = [&](const std::filesystem::path& path, std::string*) {
            restart_called = true;
            restored_path = path;
            return true;
        };
        router.set_callbacks(std::move(cb));

        router.handle_explicit_stop();
        assert(explicit_stop_called);

        router.handle_disconnect();
        assert(disconnect_called);

        router.handle_shutdown();
        assert(shutdown_called);

        assert(router.handle_restart(restart_path));
        assert(restart_called);
        assert(restored_path == restart_path);
    }

    // 6. Outbound batch polling
    {
        nstu::client::ExamTelemetryRouter router;
        nstu::exam::TelemetryBatch b;
        b.package_id = "pkg-b";
        b.package_digest.fill(std::byte{1});
        b.client_id.fill(std::byte{2});
        b.session_id.fill(std::byte{3});
        b.candidate_id = "cand-b";
        b.producer_id.fill(std::byte{4});
        b.sequence = 12;
        b.first_enqueued_at_unix_milliseconds = 1000;
        nstu::exam::TelemetrySample s;
        s.kind = nstu::exam::TelemetrySampleKind::focus_changed;
        s.source = nstu::exam::TelemetrySource::native;
        s.collector_id = 5ULL;
        s.question_revision = 0;
        s.monotonic_elapsed_milliseconds = 100;
        s.duration_milliseconds = 0;
        s.client_time_unix_milliseconds = 1000;
        s.numeric_value = 1;
        s.count = 1;
        s.reason = nstu::exam::TelemetryReason::focus_gain;
        b.samples.push_back(s);

        bool queried = false;
        nstu::client::ExamTelemetryRouterCallbacks cb;
        cb.next_batch_to_send = [&](std::uint64_t now_ms, std::uint64_t) -> std::optional<nstu::exam::TelemetryBatch> {
            queried = true;
            if (now_ms > 500) {
                return b;
            }
            return std::nullopt;
        };
        router.set_callbacks(std::move(cb));

        auto none = router.poll_outbound_batch(100);
        assert(queried);
        assert(!none.has_value());

        auto cmd = router.poll_outbound_batch(1000);
        assert(cmd.has_value());
        assert(cmd->type == nstu::protocol::CommandType::exam_telemetry_batch);
        auto decoded = nstu::exam::decode_telemetry_batch(cmd->payload);
        assert(decoded.has_value());
        assert(decoded->package_id == "pkg-b");
        assert(decoded->sequence == 12);
        assert(decoded->samples.size() == 1);
    }
}

void test_low_priority_telemetry_scheduling() {
    using namespace std::chrono_literals;
    std::string error;

    nstu::client::ClientRuntimeConfig client_config;
    client_config.server_address = "127.0.0.1";
    client_config.client_id.fill(std::byte{0x33});
    client_config.key_id = 9;
    client_config.pre_shared_key.resize(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t i = 0; i < client_config.pre_shared_key.size(); ++i) {
        client_config.pre_shared_key[i] = static_cast<std::byte>(0x70 + i);
    }

    nstu::net::TcpSocket listener;
    assert(listener.listen(0, 4, &error));
    client_config.server_port = listener.local_port(&error);

    // Test 1: Priority ordering (answers before telemetry) and no 4-batch burst
    {
        nstu::security::ReplayProtector replay;
        const nstu::control::KeyResolver resolver =
            [&](const nstu::security::ClientId& id, std::uint32_t kid)
            -> std::optional<std::vector<std::byte>> {
            if (id == client_config.client_id && kid == client_config.key_id) {
                return client_config.pre_shared_key;
            }
            return std::nullopt;
        };

        std::vector<nstu::protocol::CommandType> received_commands;
        std::mutex cmd_mutex;
        std::atomic_bool stop_server = false;

        std::thread server_th([&] {
            std::string server_err;
            auto sock = listener.accept(&server_err);
            assert(sock.is_open());
            assert(sock.set_io_timeouts(2000, 2000, &server_err));
            const auto now_sec = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            auto session = nstu::control::server_handshake(
                sock, resolver, replay, now_sec, &server_err);
            assert(session.has_value());
            nstu::control::AuthenticatedControlChannel ch(std::move(sock), std::move(*session));

            while (!stop_server) {
                auto cmd = ch.receive(&server_err);
                if (cmd.has_value()) {
                    std::scoped_lock lock(cmd_mutex);
                    received_commands.push_back(cmd->envelope.type);
                } else {
                    break;
                }
            }
        });

        std::stop_source stop_source;
        std::atomic<int> telemetry_sent_count = 0;
        std::atomic<int> answers_sent_count = 0;

        // Queue: 2 answers, then 4 telemetry batches
        std::vector<nstu::client::ClientOutboundCommand> outbound_queue;
        outbound_queue.push_back({nstu::protocol::CommandType::exam_answer_event, {std::byte{1}}});
        outbound_queue.push_back({nstu::protocol::CommandType::exam_answer_event, {std::byte{2}}});
        for (int i = 0; i < 4; ++i) {
            outbound_queue.push_back({nstu::protocol::CommandType::exam_telemetry_batch, {static_cast<std::byte>(10 + i)}});
        }
        std::size_t queue_index = 0;
        std::mutex queue_mutex;

        auto outbound_provider = [&]() -> std::optional<nstu::client::ClientOutboundCommand> {
            std::scoped_lock lock(queue_mutex);
            if (queue_index < outbound_queue.size()) {
                auto item = outbound_queue[queue_index++];
                if (item.type == nstu::protocol::CommandType::exam_telemetry_batch) {
                    telemetry_sent_count++;
                } else if (item.type == nstu::protocol::CommandType::exam_answer_event) {
                    answers_sent_count++;
                }
                return item;
            }
            return std::nullopt;
        };

        nstu::client::ClientConnectionOptions conn_options;
        conn_options.enable_authenticated_discovery = false;

        std::thread client_th([&] {
            std::string client_err;
            (void)nstu::client::run_client_control_session(
                client_config, stop_source.get_token(),
                [] {
                    nstu::control::ClientStatusReport status;
                    status.hostname = "PRIO-PC";
                    return status;
                },
                [](const nstu::control::AuthenticatedCommand&) {},
                outbound_provider,
                &client_err,
                {},
                conn_options);
        });

        assert(wait_until([&] {
            std::scoped_lock lock(cmd_mutex);
            return received_commands.size() >= 6;
        }));

        stop_source.request_stop();
        client_th.join();
        stop_server = true;
        server_th.join();

        // Verify ordering: answer events received BEFORE any telemetry batches!
        {
            std::scoped_lock lock(cmd_mutex);
            int first_answer = -1;
            int second_answer = -1;
            int first_telemetry = -1;
            for (std::size_t i = 0; i < received_commands.size(); ++i) {
                if (received_commands[i] == nstu::protocol::CommandType::exam_answer_event) {
                    if (first_answer == -1) first_answer = static_cast<int>(i);
                    else if (second_answer == -1) second_answer = static_cast<int>(i);
                } else if (received_commands[i] == nstu::protocol::CommandType::exam_telemetry_batch) {
                    if (first_telemetry == -1) first_telemetry = static_cast<int>(i);
                }
            }
            assert(first_answer != -1);
            assert(second_answer != -1);
            assert(first_telemetry != -1);
            assert(first_answer < first_telemetry);
            assert(second_answer < first_telemetry);
        }
    }

    // Test 2: Slow-progress socket on telemetry batch causes 500ms deadline timeout and disconnect
    {
        nstu::net::TcpSocket slow_listener;
        assert(slow_listener.listen(0, 4, &error));
        client_config.server_port = slow_listener.local_port(&error);

        nstu::security::ReplayProtector replay;
        const nstu::control::KeyResolver resolver =
            [&](const nstu::security::ClientId& id, std::uint32_t kid)
            -> std::optional<std::vector<std::byte>> {
            if (id == client_config.client_id && kid == client_config.key_id) {
                return client_config.pre_shared_key;
            }
            return std::nullopt;
        };

        std::atomic_bool stop_server = false;
        std::thread server_th([&] {
            std::string server_err;
            auto sock = slow_listener.accept(&server_err);
            assert(sock.is_open());
            const auto now_sec = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            auto session = nstu::control::server_handshake(
                sock, resolver, replay, now_sec, &server_err);
            assert(session.has_value());
            nstu::control::AuthenticatedControlChannel ch(std::move(sock), std::move(*session));
            auto hello = ch.receive(&server_err);
            assert(hello.has_value());
            // Stop reading so large batch blocks
            while (!stop_server) {
                std::this_thread::sleep_for(20ms);
            }
        });

        std::stop_source stop_source;
        std::vector<std::byte> large_telemetry(1024 * 1024, std::byte{0x77});
        bool offered = false;
        auto slow_outbound = [&]() -> std::optional<nstu::client::ClientOutboundCommand> {
            if (!offered) {
                offered = true;
                return nstu::client::ClientOutboundCommand{
                    nstu::protocol::CommandType::exam_telemetry_batch,
                    large_telemetry};
            }
            return std::nullopt;
        };

        nstu::client::ClientConnectionOptions conn_options;
        conn_options.enable_authenticated_discovery = false;

        const auto start_time = std::chrono::steady_clock::now();
        std::string client_err;
        const bool session_result = nstu::client::run_client_control_session(
            client_config, stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "SLOW-PC";
                return status;
            },
            [](const nstu::control::AuthenticatedCommand&) {},
            slow_outbound,
            &client_err,
            {},
            conn_options);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        assert(!session_result);
        assert(!client_err.empty());
        assert(elapsed.count() < 2500);

        stop_server = true;
        server_th.join();
    }

    nstu::client::clear_client_runtime_config(client_config);
}

namespace {

std::uint64_t task9_now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

struct Task9Storage {
    std::filesystem::path root;
    std::filesystem::path outbox;
    explicit Task9Storage(std::string_view tag) {
        root = std::filesystem::temp_directory_path() /
            ("nstu-task9-" + std::string(tag) + "-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::create_directories(root));
        outbox = root / "telemetry.bin";
    }
    ~Task9Storage() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void test_task9_real_restart_restores_storage() {
    Task9Storage storage("restart");
    nstu::client::ExamTelemetryServiceCoordinator coordinator;
    nstu::client::ExamTelemetryRouter router(coordinator, [](std::span<const std::byte>) {});
    assert(!std::filesystem::exists(storage.outbox));
    std::string error;
    assert(router.handle_restart(storage.outbox, &error));
    // The production constructor now restores actual protected storage.
    assert(coordinator.is_open());
    assert(!router.poll_outbound_batch(task9_now_ms()));
    router.handle_shutdown();
    assert(!coordinator.is_open());
}

void test_task9_decoder_preserves_specific_error() {
    const std::vector<std::byte> truncated{std::byte{1}, std::byte{0}};
    std::string decoder_error;
    assert(!nstu::client::decode_exam_telemetry_samples_payload(truncated, &decoder_error));
    assert(!decoder_error.empty());
    nstu::client::ExamTelemetryRouter router;
    std::string router_error;
    assert(!router.handle_agent_samples(truncated, task9_now_ms(), &router_error));
    assert(router_error == decoder_error);
}

nstu::exam::TelemetryEnable task9_enable() {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = "task9-package";
    enable.package_digest[0] = std::byte{1};
    enable.client_id[0] = std::byte{2};
    enable.session_id[0] = std::byte{3};
    enable.candidate_id = "task9-candidate";
    enable.server_time_unix_milliseconds = task9_now_ms();
    return enable;
}

nstu::exam::TelemetrySample task9_sample() {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_dwell;
    sample.source = nstu::exam::TelemetrySource::browser;
    sample.collector_id = 11;
    sample.question_id = "q1";
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 2000;
    sample.duration_milliseconds = 1000;
    sample.client_time_unix_milliseconds = task9_now_ms();
    sample.count = 1;
    sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}

void test_task9_real_router_lifecycle_and_exact_ack() {
    Task9Storage storage("lifecycle");
    auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([&] { return now; });
    std::vector<std::vector<std::byte>> forwarded;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte> bytes) {
        forwarded.emplace_back(bytes.begin(), bytes.end());
    });
    std::string error;
    assert(router.handle_restart(storage.outbox, &error));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::disabled);
    const auto samples = nstu::client::encode_exam_telemetry_samples_payload(
        std::vector<nstu::exam::TelemetrySample>{task9_sample()});
    assert(!samples.empty());
    assert(!router.handle_agent_samples(samples, now, &error));
    assert(coordinator.outbox_size() == 0 && forwarded.empty());
    const auto enable = task9_enable();
    const auto raw_enable = nstu::exam::encode_telemetry_enable(enable);
    error.clear();
    assert(router.handle_server_enable(raw_enable, &error));
    assert(forwarded.size() == 1 && forwarded.back() == raw_enable);
    const auto producer = coordinator.active_producer_id();
    assert(router.handle_agent_samples(samples, now, &error));
    const auto command = router.poll_outbound_batch(now);
    assert(command && command->type == nstu::protocol::CommandType::exam_telemetry_batch);
    const auto batch = nstu::exam::decode_telemetry_batch(command->payload);
    assert(batch && batch->producer_id == producer && batch->sequence == 1);
    assert(coordinator.next_sequence() == 2);
    router.handle_disconnect();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(!router.poll_outbound_batch(now));
    assert(coordinator.outbox_size() == 1);
    assert(router.handle_server_enable(raw_enable, &error));
    assert(coordinator.active_producer_id() == producer && forwarded.size() == 2);
    assert(router.poll_outbound_batch(now)->payload == command->payload);
    router.handle_shutdown();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(!coordinator.is_open());
    assert(router.handle_restart(storage.outbox, &error));
    assert(coordinator.active_producer_id() == producer && coordinator.next_sequence() == 2);
    assert(coordinator.outbox_size() == 1 && !router.poll_outbound_batch(now));
    assert(router.handle_server_enable(raw_enable, &error));
    nstu::exam::TelemetryAck ack;
    ack.session_id = batch->session_id;
    ack.producer_id = batch->producer_id;
    ack.sequence = batch->sequence;
    ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(*batch);
    ack.status = nstu::exam::TelemetryAckStatus::accepted;
    ack.server_time_unix_milliseconds = now;
    auto wrong = ack;
    wrong.batch_hash[0] ^= std::byte{1};
    const auto wrong_ack_bytes = nstu::exam::encode_telemetry_ack(wrong);
    const auto ack_bytes = nstu::exam::encode_telemetry_ack(ack);
    assert(!wrong_ack_bytes.empty());
    assert(!ack_bytes.empty());
    assert(router.handle_server_ack(wrong_ack_bytes, now, &error) ==
           nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(coordinator.outbox_size() == 1);
    assert(router.handle_server_ack(ack_bytes, now, &error) ==
           nstu::exam::TelemetryAckApplyResult::retired);
    assert(coordinator.outbox_size() == 0 && coordinator.next_sequence() == 2);
    assert(router.handle_agent_samples(samples, now, &error));
    const auto second = router.poll_outbound_batch(now);
    assert(second);
    assert(nstu::exam::decode_telemetry_batch(second->payload)->sequence == 2);
    router.handle_explicit_stop();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(coordinator.outbox_size() == 0 && !router.poll_outbound_batch(now));
    assert(!router.handle_server_enable(raw_enable, &error));
    assert(forwarded.size() == 3);
    router.handle_shutdown();
    assert(router.handle_restart(storage.outbox, &error));
    assert(!router.handle_server_enable(raw_enable, &error));
    router.handle_shutdown();
}

void test_task9_real_router_restore_failure_is_closed() {
    Task9Storage storage("corrupt");
    nstu::client::ExamTelemetryServiceCoordinator coordinator;
    std::size_t forwards = 0;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
    std::string error;
    assert(router.handle_restart(storage.outbox, &error));
    router.handle_shutdown();
    const auto state_path = std::filesystem::path(storage.outbox.wstring() + L".coord_state");
    {
        std::ofstream state(state_path, std::ios::binary | std::ios::trunc);
        assert(state);
        state << "corrupt coordinator state";
    }
    error.clear();
    assert(!router.handle_restart(storage.outbox, &error));
    assert(!error.empty() && !coordinator.is_open());
    assert(!router.handle_server_enable(nstu::exam::encode_telemetry_enable(task9_enable()), &error));
    assert(forwards == 0 && !router.poll_outbound_batch(task9_now_ms()));
    std::ifstream state(state_path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(state), std::istreambuf_iterator<char>()};
    assert(saved == "corrupt coordinator state");
}

void test_task9_real_router_offline_maintenance() {
    Task9Storage storage("maintenance");
    auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([&] { return now; });
    nstu::client::ExamTelemetryRouter router(coordinator, [](std::span<const std::byte>) {});
    std::string error;
    assert(router.handle_restart(storage.outbox, &error));
    assert(router.handle_server_enable(nstu::exam::encode_telemetry_enable(task9_enable()), &error));
    assert(router.handle_agent_samples(std::vector<nstu::exam::TelemetrySample>{task9_sample()}, now, &error));
    const auto producer = coordinator.active_producer_id();
    router.handle_disconnect();
    now += nstu::exam::kTelemetryRetentionMilliseconds;
    const HANDLE locked = CreateFileW(storage.outbox.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    error.clear();
    assert(!router.handle_maintenance(0, &error));
    assert(!error.empty() && !router.poll_outbound_batch(now));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(CloseHandle(locked));
    error.clear();
    assert(router.handle_maintenance(0, &error));
    assert(coordinator.outbox_size() == 0);
    assert(coordinator.aggregate_loss().expired_batches == 1);
    assert(coordinator.active_producer_id() == producer && coordinator.next_sequence() == 2);
    assert(!router.poll_outbound_batch(now));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    router.handle_shutdown();
}


using RouterStopOutcome = nstu::client::TelemetryStopOutcome;
using RouterStopApplicability = nstu::client::TelemetryStopApplicability;
using RouterStopRevocation = nstu::client::TelemetryStopMemoryRevocation;
using RouterStopWitness = nstu::client::TelemetryStopDurableWitness;
using RouterStopCleanup = nstu::client::TelemetryStopCleanup;
using RouterStopError = nstu::client::TelemetryStopError;
using RouterStopDispatch = nstu::client::TelemetryStopDispatch;
static_assert(!noexcept(std::declval<nstu::client::ExamTelemetryRouter&>().handle_explicit_stop_with_outcome()));
static_assert(!noexcept(std::declval<nstu::client::ExamTelemetryRouter&>().handle_explicit_stop()));

void assert_router_stop_outcome(const nstu::client::TelemetryStopDispatchResult& result,
                                const RouterStopOutcome& expected) {
    assert(result.dispatch == RouterStopDispatch::outcome_callback_invoked);
    assert(result.outcome.has_value());
    assert(result.outcome->applicability == expected.applicability);
    assert(result.outcome->memory_revocation == expected.memory_revocation);
    assert(result.outcome->durable_witness == expected.durable_witness);
    assert(result.outcome->cleanup == expected.cleanup);
    assert(result.outcome->error == expected.error);
}

void test_router_stop_dispatch_and_forwarding() {
    nstu::client::TelemetryStopDispatchResult default_result;
    assert(default_result.dispatch == RouterStopDispatch::not_configured);
    assert(default_result.outcome == std::nullopt);
    nstu::client::ExamTelemetryRouter absent;
    const auto missing = absent.handle_explicit_stop_with_outcome();
    assert(missing.dispatch == RouterStopDispatch::not_configured);
    assert(missing.outcome == std::nullopt);
    absent.handle_explicit_stop();

    int legacy_calls = 0;
    nstu::client::ExamTelemetryRouterCallbacks legacy;
    legacy.on_explicit_stop = [&] { ++legacy_calls; };
    nstu::client::ExamTelemetryRouter legacy_router(std::move(legacy));
    const auto legacy_result = legacy_router.handle_explicit_stop_with_outcome();
    assert(legacy_result.dispatch == RouterStopDispatch::legacy_callback_invoked);
    assert(legacy_result.outcome == std::nullopt && legacy_calls == 1);
    legacy_router.handle_explicit_stop();
    assert(legacy_calls == 2);

    // Supplied tuples establish forwarding only; native coordinator tests below establish storage facts.
    const std::array<RouterStopOutcome, 5> supplied{{
        {RouterStopApplicability::closed, RouterStopRevocation::not_applicable,
         RouterStopWitness::not_attempted, RouterStopCleanup::not_needed, RouterStopError::none},
        {RouterStopApplicability::applicable, RouterStopRevocation::revoked_now,
         RouterStopWitness::refusal_marker_confirmed, RouterStopCleanup::pending, RouterStopError::protected_state_failed},
        {RouterStopApplicability::applicable, RouterStopRevocation::already_revoked,
         RouterStopWitness::unconfirmed, RouterStopCleanup::pending, RouterStopError::revocation_witness_unconfirmed},
        {RouterStopApplicability::applicable, RouterStopRevocation::revoked_now,
         RouterStopWitness::protected_state_confirmed, RouterStopCleanup::pending, RouterStopError::protected_state_failed},
        {RouterStopApplicability::applicable, RouterStopRevocation::already_revoked,
         RouterStopWitness::protected_state_confirmed, RouterStopCleanup::pending, RouterStopError::cleanup_failed}
    }};
    for (const bool configure_legacy : {false, true}) {
        for (const auto& outcome : supplied) {
            int typed_calls = 0;
            int fallback_calls = 0;
            nstu::client::ExamTelemetryRouterCallbacks callbacks;
            callbacks.on_explicit_stop_with_outcome = [&] { ++typed_calls; return outcome; };
            if (configure_legacy) callbacks.on_explicit_stop = [&] { ++fallback_calls; };
            nstu::client::ExamTelemetryRouter router(std::move(callbacks));
            assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(), outcome);
            assert(typed_calls == 1 && fallback_calls == 0);
            router.handle_explicit_stop();
            assert(typed_calls == 2 && fallback_calls == 0);
        }
    }
}

void test_router_stop_ten_field_aggregate_compatibility() {
    std::array<int, 10> calls{};
    const auto enable = task9_enable();
    const auto raw = nstu::exam::encode_telemetry_enable(enable);
    const auto sample = task9_sample();
    const std::filesystem::path sentinel_path = L"old-ten-field-position";
    // Deliberately only the ten original positional fields, with distinct observable sentinels.
    nstu::client::ExamTelemetryRouterCallbacks old_callbacks{
        [&](const nstu::exam::TelemetryEnable& value, std::string*) {
            ++calls[0]; assert(value.package_id == enable.package_id); return true;
        },
        [&](const std::vector<nstu::exam::TelemetrySample>& values, std::uint64_t now, std::string*) {
            ++calls[1]; assert(values.size() == 1 && values[0].question_id == sample.question_id);
            assert(now == 123); return true;
        },
        [&](const nstu::exam::TelemetryAck& value, std::uint64_t now, std::string*) {
            ++calls[2]; assert(value.sequence == 71 && now == 124);
            return nstu::exam::TelemetryAckApplyResult::retired;
        },
        [&] { ++calls[3]; },
        [&] { ++calls[4]; },
        [&] { ++calls[5]; },
        [&](const std::filesystem::path& value, std::string*) {
            ++calls[6]; assert(value == sentinel_path); return true;
        },
        [&](std::uint64_t now, std::string*) { ++calls[7]; assert(now == 125); return true; },
        [&](std::span<const std::byte> value) {
            ++calls[8]; assert(std::vector<std::byte>(value.begin(), value.end()) == raw);
        },
        [&](std::uint64_t now, std::uint64_t timeout) -> std::optional<nstu::exam::TelemetryBatch> {
            ++calls[9]; assert(now == 126 && timeout == 127); return std::nullopt;
        }
    };
    assert(!old_callbacks.on_explicit_stop_with_outcome);
    nstu::client::ExamTelemetryRouter router(std::move(old_callbacks));
    assert(router.handle_server_enable(enable, raw));
    assert(router.handle_agent_samples(std::vector{sample}, 123));
    nstu::exam::TelemetryAck ack;
    ack.sequence = 71;
    assert(router.handle_server_ack(ack, 124) == nstu::exam::TelemetryAckApplyResult::retired);
    const auto stop = router.handle_explicit_stop_with_outcome();
    assert(stop.dispatch == RouterStopDispatch::legacy_callback_invoked && !stop.outcome);
    assert(calls[3] == 1 && calls[4] == 0 && calls[5] == 0);
    router.handle_explicit_stop();
    router.handle_disconnect();
    router.handle_shutdown();
    assert(router.handle_restart(sentinel_path));
    assert(router.handle_maintenance(125));
    assert(!router.poll_outbound_batch(126, 127));
    assert((calls == std::array<int, 10>{1, 1, 1, 2, 1, 1, 1, 1, 1, 1}));
}

void test_router_stop_exceptions_propagate_without_fallback() {
    struct Sentinel { int value = 91; } sentinel;
    for (const bool typed : {false, true}) {
        for (const bool void_entry : {false, true}) {
            int legacy_calls = 0;
            int typed_calls = 0;
            nstu::client::ExamTelemetryRouterCallbacks callbacks;
            callbacks.on_explicit_stop = [&] {
                ++legacy_calls;
                if (!typed) throw &sentinel;
            };
            if (typed) callbacks.on_explicit_stop_with_outcome = [&]() -> RouterStopOutcome {
                ++typed_calls; throw &sentinel;
            };
            nstu::client::ExamTelemetryRouter router(std::move(callbacks));
            bool caught = false;
            try {
                if (void_entry) router.handle_explicit_stop();
                else (void)router.handle_explicit_stop_with_outcome();
            } catch (const Sentinel* actual) {
                assert(actual == &sentinel && actual->value == 91);
                caught = true;
            }
            assert(caught);
            assert(typed_calls == (typed ? 1 : 0));
            assert(legacy_calls == (typed ? 0 : 1));
        }
    }
}

class RouterStopSharingLock {
public:
    explicit RouterStopSharingLock(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        assert(handle_ != INVALID_HANDLE_VALUE);
    }
    ~RouterStopSharingLock() { assert(CloseHandle(handle_)); }
    RouterStopSharingLock(const RouterStopSharingLock&) = delete;
    RouterStopSharingLock& operator=(const RouterStopSharingLock&) = delete;
private:
    HANDLE handle_;
};

std::string router_stop_file_bytes(const std::filesystem::path& path) {
    assert(std::filesystem::is_regular_file(path));
    std::ifstream file(path, std::ios::binary);
    assert(file);
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    assert(!file.bad());
    assert(bytes.size() == std::filesystem::file_size(path));
    return bytes;
}

void assert_router_stop_protected_lifecycle(const std::filesystem::path& outbox,
                                           const nstu::exam::TelemetryEnable& enable,
                                           nstu::client::TelemetryLifecycleState expected) {
    constexpr std::string_view entropy = "NSTU-Exam-Telemetry-Service-Coordinator-v1";
    const auto bytes = nstu::security::load_machine_secret(outbox.wstring() + L".coord_state",
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    const auto lifecycle_offset = 108 + enable.package_id.size() + enable.candidate_id.size();
    assert(bytes.size() >= 32 && lifecycle_offset < bytes.size() - 32);
    const auto payload = std::span<const std::byte>(bytes).first(bytes.size() - 32);
    const auto digest = nstu::security::sha256(payload);
    assert(digest && std::equal(digest->begin(), digest->end(), bytes.end() - 32));
    const auto u32 = [&](std::size_t offset) {
        assert(offset <= payload.size() && payload.size() - offset >= 4);
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value |= std::to_integer<std::uint32_t>(payload[offset + index]) << (8 * index);
        return value;
    };
    assert(u32(0) == 0x544c4d53);
    assert(payload[4] == std::byte{3} && payload[5] == std::byte{0});
    assert(payload[6] == std::byte{0} && payload[7] == std::byte{0});
    assert(u32(8) == enable.package_id.size());
    assert(u32(76 + enable.package_id.size()) == enable.candidate_id.size());
    assert(payload[lifecycle_offset] == static_cast<std::byte>(expected));
}

void assert_router_stop_reconstruction_refuses_old(const Task9Storage& storage,
                                                   const nstu::exam::TelemetryEnable& enable,
                                                   std::uint64_t now) {
    nstu::client::ExamTelemetryServiceCoordinator reconstructed([now] { return now; });
    int forwards = 0;
    nstu::client::ExamTelemetryRouter router(reconstructed, [&](std::span<const std::byte>) { ++forwards; });
    assert(router.handle_restart(storage.outbox));
    assert(reconstructed.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(reconstructed.outbox_size() == 0);
    assert(!router.poll_outbound_batch(now));
    assert(!router.handle_server_enable(nstu::exam::encode_telemetry_enable(enable)));
    assert(forwards == 0 && !router.poll_outbound_batch(now));
    router.handle_shutdown();
}

void test_router_stop_real_normal_repeated_and_nonapplicable() {
    Task9Storage storage("router-stop-normal");
    const auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([now] { return now; });
    int forwards = 0;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
    assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
        {RouterStopApplicability::closed, RouterStopRevocation::not_applicable,
         RouterStopWitness::not_attempted, RouterStopCleanup::not_needed, RouterStopError::none});
    assert(!coordinator.is_open() && !std::filesystem::exists(storage.outbox));
    assert(router.handle_restart(storage.outbox));
    const std::filesystem::path state = storage.outbox.wstring() + L".coord_state";
    const std::filesystem::path marker = state.wstring() + L".blocked";
    const auto unbound_state = router_stop_file_bytes(state);
    const auto unbound_payload = router_stop_file_bytes(storage.outbox);
    assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
        {RouterStopApplicability::unbound, RouterStopRevocation::not_applicable,
         RouterStopWitness::not_attempted, RouterStopCleanup::not_needed, RouterStopError::none});
    assert(router_stop_file_bytes(state) == unbound_state);
    assert(router_stop_file_bytes(storage.outbox) == unbound_payload && !std::filesystem::exists(marker));
    assert(forwards == 0 && !router.poll_outbound_batch(now));
    const auto enable = task9_enable();
    const auto raw_enable = nstu::exam::encode_telemetry_enable(enable);
    assert(router.handle_server_enable(raw_enable));
    assert(router.handle_agent_samples(std::vector{task9_sample()}, now));
    assert(forwards == 1 && coordinator.outbox_size() == 1 && router.poll_outbound_batch(now));
    assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
        {RouterStopApplicability::applicable, RouterStopRevocation::revoked_now,
         RouterStopWitness::protected_state_confirmed, RouterStopCleanup::complete, RouterStopError::none});
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::revoked);
    nstu::exam::TelemetryOutbox inspect;
    assert(inspect.open(storage.outbox, now, nullptr) && inspect.size() == 0);
    inspect.close();
    assert(coordinator.outbox_size() == 0 && !router.poll_outbound_batch(now));
    assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
        {RouterStopApplicability::applicable, RouterStopRevocation::already_revoked,
         RouterStopWitness::protected_state_confirmed, RouterStopCleanup::complete, RouterStopError::none});
    assert(!router.handle_server_enable(raw_enable) && forwards == 1);
    assert(!std::filesystem::exists(marker) && !router.poll_outbound_batch(now));
    router.handle_shutdown();
    assert_router_stop_reconstruction_refuses_old(storage, enable, now);
}

void test_router_stop_native_failures_and_maintenance_recovery() {
    // State replacement failure, existing empty/nonempty markers, and payload replacement failure.
    for (int variant = 0; variant < 4; ++variant) {
        Task9Storage storage("router-stop-native-" + std::to_string(variant));
        const auto now = task9_now_ms();
        nstu::client::ExamTelemetryServiceCoordinator coordinator([now] { return now; });
        int forwards = 0;
        nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
        const auto enable = task9_enable();
        const auto raw_enable = nstu::exam::encode_telemetry_enable(enable);
        assert(router.handle_restart(storage.outbox));
        assert(router.handle_server_enable(raw_enable));
        assert(router.handle_agent_samples(std::vector{task9_sample()}, now));
        assert(forwards == 1 && coordinator.outbox_size() == 1);
        const std::filesystem::path state = storage.outbox.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto original_state = router_stop_file_bytes(state);
        const auto original_payload = router_stop_file_bytes(storage.outbox);
        assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::enabled);
        const bool existing_marker = variant == 1 || variant == 2;
        const std::string marker_sentinel = variant == 1 ? std::string{} : std::string("\x24\x68\xac", 3);
        if (existing_marker) {
            const HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            assert(file != INVALID_HANDLE_VALUE);
            DWORD written = 0;
            assert(WriteFile(file, marker_sentinel.data(), static_cast<DWORD>(marker_sentinel.size()), &written, nullptr));
            assert(written == marker_sentinel.size() && FlushFileBuffers(file));
            assert(CloseHandle(file));
            assert(router_stop_file_bytes(marker) == marker_sentinel);
        }
        {
            RouterStopSharingLock lock(variant == 3 ? storage.outbox : state);
            const auto witness = variant == 3 ? RouterStopWitness::protected_state_confirmed :
                (existing_marker ? RouterStopWitness::unconfirmed : RouterStopWitness::refusal_marker_confirmed);
            const auto stop_error = variant == 3 ? RouterStopError::cleanup_failed :
                (existing_marker ? RouterStopError::revocation_witness_unconfirmed : RouterStopError::protected_state_failed);
            assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
                {RouterStopApplicability::applicable, RouterStopRevocation::revoked_now,
                 witness, RouterStopCleanup::pending, stop_error});
            assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
            assert(!router.poll_outbound_batch(now));
            assert(!router.handle_server_enable(raw_enable) && forwards == 1);
            if (variant == 3) {
                assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::revoked);
                assert(router_stop_file_bytes(storage.outbox) == original_payload);
                assert(!std::filesystem::exists(marker));
            } else {
                assert(router_stop_file_bytes(state) == original_state);
                assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::enabled);
                assert(router_stop_file_bytes(marker) == (existing_marker ? marker_sentinel : std::string(1, '\x01')));
                nstu::exam::TelemetryOutbox inspect;
                assert(inspect.open(storage.outbox, now, nullptr) && inspect.size() == 0);
                inspect.close();
                nstu::client::ExamTelemetryServiceCoordinator blocked([now] { return now; });
                int blocked_forwards = 0;
                nstu::client::ExamTelemetryRouter blocked_router(blocked,
                    [&](std::span<const std::byte>) { ++blocked_forwards; });
                assert(!blocked_router.handle_restart(storage.outbox) && !blocked.is_open());
                assert(!blocked_router.handle_server_enable(raw_enable));
                assert(!blocked_router.poll_outbound_batch(now) && blocked_forwards == 0);
                assert(router_stop_file_bytes(state) == original_state);
                assert(router_stop_file_bytes(marker) == (existing_marker ? marker_sentinel : std::string(1, '\x01')));
            }
        } // Release the native sharing lock before maintenance or teardown.
        assert(router.handle_maintenance(now));
        assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(coordinator.outbox_size() == 0 && !router.poll_outbound_batch(now));
        assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::revoked);
        assert(!std::filesystem::exists(marker));
        nstu::exam::TelemetryOutbox inspect;
        assert(inspect.open(storage.outbox, now, nullptr) && inspect.size() == 0);
        inspect.close();
        assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(),
            {RouterStopApplicability::applicable, RouterStopRevocation::already_revoked,
             RouterStopWitness::protected_state_confirmed, RouterStopCleanup::complete, RouterStopError::none});
        assert(!router.handle_server_enable(raw_enable) && forwards == 1 && !router.poll_outbound_batch(now));
        router.handle_shutdown();
        assert_router_stop_reconstruction_refuses_old(storage, enable, now);
    }
}

void run_router_stop_outcome_tests() {
    test_router_stop_dispatch_and_forwarding();
    test_router_stop_ten_field_aggregate_compatibility();
    test_router_stop_exceptions_propagate_without_fallback();
    test_router_stop_real_normal_repeated_and_nonapplicable();
    test_router_stop_native_failures_and_maintenance_recovery();
}

using DisconnectOutcome = nstu::client::TelemetryDisconnectOutcome;
using DisconnectState = nstu::client::TelemetryDisconnectState;
using DisconnectPending = nstu::client::TelemetryDisconnectPending;
using DisconnectPublication = nstu::client::TelemetryDisconnectPublication;
using DisconnectCleanup = nstu::client::TelemetryDisconnectCleanup;
using DisconnectError = nstu::client::TelemetryDisconnectError;
using DisconnectDispatch = nstu::client::TelemetryDisconnectDispatch;
static_assert(!noexcept(std::declval<nstu::client::ExamTelemetryRouter&>().handle_disconnect_with_outcome()));
static_assert(!noexcept(std::declval<nstu::client::ExamTelemetryRouter&>().handle_disconnect()));

void assert_router_disconnect(const nstu::client::TelemetryDisconnectDispatchResult& result,
                              const DisconnectOutcome& expected) {
    assert(result.dispatch == DisconnectDispatch::outcome_callback_invoked && result.outcome);
    assert(result.outcome->before == expected.before);
    assert(result.outcome->after == expected.after);
    assert(result.outcome->pending == expected.pending);
    assert(result.outcome->publication == expected.publication);
    assert(result.outcome->cleanup == expected.cleanup);
    assert(result.outcome->current_failure == expected.current_failure);
}

void test_router_disconnect_dispatch_compatibility_and_exceptions() {
    nstu::client::TelemetryDisconnectDispatchResult defaults;
    assert(defaults.dispatch == DisconnectDispatch::not_configured && !defaults.outcome);
    nstu::client::ExamTelemetryRouter router;
    const auto absent = router.handle_disconnect_with_outcome();
    assert(absent.dispatch == DisconnectDispatch::not_configured && !absent.outcome);
    router.handle_disconnect();
    int legacy_calls = 0;
    nstu::client::ExamTelemetryRouterCallbacks legacy;
    legacy.on_disconnect = [&] { ++legacy_calls; };
    router.set_callbacks(legacy);
    const auto legacy_result = router.handle_disconnect_with_outcome();
    assert(legacy_result.dispatch == DisconnectDispatch::legacy_callback_invoked && !legacy_result.outcome);
    router.handle_disconnect();
    assert(legacy_calls == 2);

    // Supplied tuples prove forwarding only, including earlier publication versus current failure.
    const std::array<DisconnectOutcome, 4> supplied{{
        {},
        {DisconnectState::enabled, DisconnectState::suspended, DisconnectPending::retained,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed,
         DisconnectError::pending_enqueue_unfinished},
        {DisconnectState::enabled, DisconnectState::suspended, DisconnectPending::completed,
         DisconnectPublication::earlier_protected_snapshot_confirmed, DisconnectCleanup::not_needed,
         DisconnectError::protected_state_failed},
        {DisconnectState::enabled, DisconnectState::revoked, DisconnectPending::revoked_age_uncertain,
         DisconnectPublication::earlier_protected_snapshot_confirmed, DisconnectCleanup::pending,
         DisconnectError::revocation_witness_unconfirmed}
    }};
    for (const bool both : {false, true}) {
        for (const auto& outcome : supplied) {
            int typed_calls = 0;
            nstu::client::ExamTelemetryRouterCallbacks callbacks;
            callbacks.on_disconnect_with_outcome = [&] { ++typed_calls; return outcome; };
            if (both) callbacks.on_disconnect = [&] { ++legacy_calls; };
            router.set_callbacks(std::move(callbacks));
            assert_router_disconnect(router.handle_disconnect_with_outcome(), outcome);
            router.handle_disconnect();
            assert(typed_calls == 2 && legacy_calls == 2);
        }
    }
    router.set_callbacks(legacy); // Replacing callbacks removes typed precedence.
    const auto replaced = router.handle_disconnect_with_outcome();
    assert(replaced.dispatch == DisconnectDispatch::legacy_callback_invoked && !replaced.outcome);
    assert(legacy_calls == 3);
    router.set_callbacks({});
    assert(router.handle_disconnect_with_outcome().dispatch == DisconnectDispatch::not_configured);

    int stop_calls = 0;
    // Eleven positional fields retain the typed stop position; the appended disconnect defaults empty.
    nstu::client::ExamTelemetryRouterCallbacks eleven{
        {}, {}, {}, {}, [&] { ++legacy_calls; }, {}, {}, {}, {}, {},
        [&] { ++stop_calls; return RouterStopOutcome{}; }
    };
    assert(eleven.on_explicit_stop_with_outcome && !eleven.on_disconnect_with_outcome);
    router.set_callbacks(std::move(eleven));
    assert_router_stop_outcome(router.handle_explicit_stop_with_outcome(), {});
    assert(router.handle_disconnect_with_outcome().dispatch == DisconnectDispatch::legacy_callback_invoked);
    assert(stop_calls == 1 && legacy_calls == 4);

    struct Sentinel {} sentinel;
    for (const bool typed : {false, true}) {
        for (const bool void_entry : {false, true}) {
            int legacy_count = 0;
            int typed_count = 0;
            nstu::client::ExamTelemetryRouterCallbacks callbacks;
            callbacks.on_disconnect = [&] { ++legacy_count; if (!typed) throw &sentinel; };
            if (typed) callbacks.on_disconnect_with_outcome = [&]() -> DisconnectOutcome {
                ++typed_count; throw &sentinel;
            };
            router.set_callbacks(std::move(callbacks));
            bool caught = false;
            try {
                if (void_entry) router.handle_disconnect();
                else (void)router.handle_disconnect_with_outcome();
            } catch (const Sentinel* actual) { assert(actual == &sentinel); caught = true; }
            assert(caught && typed_count == (typed ? 1 : 0) && legacy_count == (typed ? 0 : 1));
        }
    }
}

std::vector<std::byte> router_disconnect_state_payload(const std::filesystem::path& outbox) {
    constexpr std::string_view entropy = "NSTU-Exam-Telemetry-Service-Coordinator-v1";
    auto bytes = nstu::security::load_machine_secret(outbox.wstring() + L".coord_state",
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    assert(bytes.size() >= 32);
    const auto payload = std::span<const std::byte>(bytes).first(bytes.size() - 32);
    const auto digest = nstu::security::sha256(payload);
    assert(digest && std::equal(digest->begin(), digest->end(), bytes.end() - 32));
    bytes.resize(bytes.size() - 32);
    return bytes;
}

void assert_router_disconnect_batch(const std::filesystem::path& outbox,
                                    const nstu::exam::TelemetryBatch& expected, std::uint64_t now) {
    constexpr std::string_view entropy = "NSTU-Exam-Telemetry-Outbox-v1";
    const auto physical = nstu::security::load_machine_secret(outbox.wstring(),
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    const auto encoded = nstu::exam::encode_telemetry_batch(expected);
    assert(!encoded.empty() && std::search(physical.begin(), physical.end(), encoded.begin(), encoded.end()) != physical.end());
    nstu::exam::TelemetryOutbox inspect;
    assert(inspect.open(outbox, now, nullptr));
    const auto batches = inspect.pending_batches();
    assert(batches.size() == 1 && nstu::exam::encode_telemetry_batch(batches.front()) == encoded);
}

void assert_router_disconnect_physical_empty_outbox(const std::filesystem::path& outbox) {
    constexpr std::string_view entropy = "NSTU-Exam-Telemetry-Outbox-v1";
    const auto bytes = nstu::security::load_machine_secret(outbox.wstring(),
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    assert(bytes.size() >= 64);
    const auto payload = std::span<const std::byte>(bytes).first(bytes.size() - 32);
    const auto digest = nstu::security::sha256(payload);
    assert(digest && std::equal(digest->begin(), digest->end(), bytes.end() - 32));
    const auto u32 = [&](std::size_t offset) {
        assert(offset <= payload.size() && payload.size() - offset >= 4);
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value |= std::to_integer<std::uint32_t>(payload[offset + index]) << (8 * index);
        return value;
    };
    assert(u32(0) == 0x544c4d4f);
    assert(payload[4] == std::byte{2} && payload[5] == std::byte{0});
    assert(payload[6] == std::byte{0} && payload[7] == std::byte{0});
    const auto loss_count = u32(24);
    assert(loss_count <= 64);
    const auto records_offset = std::size_t{28} + std::size_t{56} * loss_count;
    assert(u32(records_offset) == 0 && payload.size() == records_offset + 4);
    // Read protected bytes directly: no open/prune operation can hide a stale batch.
}

void test_router_disconnect_real_retention_and_reconstruction() {
    Task9Storage storage("disconnect-retention");
    const auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([now] { return now; });
    int forwards = 0;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
    assert_router_disconnect(router.handle_disconnect_with_outcome(), {});
    assert(router.handle_restart(storage.outbox));
    const auto unbound_payload = router_stop_file_bytes(storage.outbox);
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::disabled, DisconnectState::disabled, DisconnectPending::not_present,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert(router_stop_file_bytes(storage.outbox) == unbound_payload && forwards == 0);
    const auto enable = task9_enable();
    const auto raw = nstu::exam::encode_telemetry_enable(enable);
    assert(router.handle_server_enable(raw));
    assert(router.handle_agent_samples(std::vector{task9_sample()}, now));
    constexpr std::uint64_t long_timeout = 600000;
    const auto offered = router.poll_outbound_batch(now, long_timeout);
    assert(offered);
    const auto expected = nstu::exam::decode_telemetry_batch(offered->payload);
    assert(expected && expected->sequence == 1 && expected->producer_id == coordinator.active_producer_id());
    const auto payload = router_stop_file_bytes(storage.outbox);
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::enabled, DisconnectState::suspended, DisconnectPending::not_present,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::suspended);
    assert(router_stop_file_bytes(storage.outbox) == payload && forwards == 1);
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::suspended, DisconnectState::suspended, DisconnectPending::not_present,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert(!router.poll_outbound_batch(now + 1, long_timeout));
    auto mismatch = enable;
    mismatch.candidate_id = "wrong-candidate";
    assert(!router.handle_server_enable(nstu::exam::encode_telemetry_enable(mismatch)));
    assert(forwards == 1 && !router.poll_outbound_batch(now + 1, long_timeout));
    assert(router.handle_server_enable(raw) && forwards == 2);
    const auto reoffered = router.poll_outbound_batch(now + 1, long_timeout);
    assert(reoffered && reoffered->payload == offered->payload);
    assert_router_disconnect_batch(storage.outbox, *expected, now);
    router.handle_disconnect();
    coordinator.close();
    nstu::client::ExamTelemetryServiceCoordinator rebuilt([now] { return now; });
    nstu::client::ExamTelemetryRouter restored(rebuilt, [&](std::span<const std::byte>) { ++forwards; });
    assert(restored.handle_restart(storage.outbox));
    assert(rebuilt.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(rebuilt.next_sequence() == 2 && rebuilt.active_producer_id() == expected->producer_id);
    assert(rebuilt.outbox_size() == 1 && !restored.poll_outbound_batch(now + 2, long_timeout));
    assert(restored.handle_server_enable(raw) && forwards == 3);
    const auto recovered = restored.poll_outbound_batch(now + 2, long_timeout);
    assert(recovered && recovered->payload == offered->payload);
    restored.handle_shutdown();
}

void test_router_disconnect_real_state_failure_retry() {
    Task9Storage storage("disconnect-state-failure");
    const auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([now] { return now; });
    int forwards = 0;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
    const auto enable = task9_enable();
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(nstu::exam::encode_telemetry_enable(enable)));
    const std::filesystem::path state = storage.outbox.wstring() + L".coord_state";
    const auto original = router_stop_file_bytes(state);
    {
        RouterStopSharingLock held(state);
        assert_router_disconnect(router.handle_disconnect_with_outcome(),
            {DisconnectState::enabled, DisconnectState::suspended, DisconnectPending::not_present,
             DisconnectPublication::unconfirmed, DisconnectCleanup::not_needed, DisconnectError::protected_state_failed});
        assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
        assert(router_stop_file_bytes(state) == original && forwards == 1);
        assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::enabled);
    }
    const auto history = coordinator.last_error();
    assert(!history.empty());
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::suspended, DisconnectState::suspended, DisconnectPending::not_present,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert(coordinator.last_error() == history && forwards == 1 && !router.poll_outbound_batch(now));
    assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::suspended);
    router.handle_shutdown();
}

void test_router_disconnect_real_pending_retry() {
    Task9Storage storage("disconnect-pending");
    const auto now = task9_now_ms();
    nstu::client::ExamTelemetryServiceCoordinator coordinator([now] { return now; });
    int forwards = 0;
    nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
    const auto enable = task9_enable();
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(nstu::exam::encode_telemetry_enable(enable)));
    const nstu::exam::TelemetryBatch expected{enable.package_id, enable.package_digest, enable.client_id,
        enable.session_id, enable.candidate_id, coordinator.active_producer_id(), 1, now, std::vector{task9_sample()}};
    const auto encoded = nstu::exam::encode_telemetry_batch(expected);
    const auto original_payload = router_stop_file_bytes(storage.outbox);
    {
        RouterStopSharingLock held(storage.outbox);
        assert(!router.handle_agent_samples(expected.samples, now));
        const auto wal = router_disconnect_state_payload(storage.outbox);
        assert(!encoded.empty() && wal.size() >= encoded.size());
        assert(std::equal(encoded.rbegin(), encoded.rend(), wal.rbegin()));
        assert_router_disconnect(router.handle_disconnect_with_outcome(),
            {DisconnectState::enabled, DisconnectState::suspended, DisconnectPending::retained,
             DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed,
             DisconnectError::pending_enqueue_unfinished});
        assert(coordinator.next_sequence() == 1 && coordinator.outbox_size() == 0 && forwards == 1);
        assert(router_stop_file_bytes(storage.outbox) == original_payload);
        const auto suspended_wal = router_disconnect_state_payload(storage.outbox);
        assert(suspended_wal.size() >= encoded.size());
        assert(std::equal(encoded.rbegin(), encoded.rend(), suspended_wal.rbegin()));
        assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::suspended);
    }
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::suspended, DisconnectState::suspended, DisconnectPending::completed,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert(coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1 && forwards == 1);
    assert_router_disconnect_batch(storage.outbox, expected, now);
    assert(!router.poll_outbound_batch(now));
    assert_router_disconnect(router.handle_disconnect_with_outcome(),
        {DisconnectState::suspended, DisconnectState::suspended, DisconnectPending::not_present,
         DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
    assert(coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1);
    router.handle_shutdown();
}

void test_router_disconnect_real_expiry_and_cleanup() {
    for (const bool block_payload : {false, true}) {
        Task9Storage storage(block_payload ? "disconnect-expiry-cleanup" : "disconnect-expiry-marker");
        auto now = task9_now_ms();
        nstu::client::ExamTelemetryServiceCoordinator coordinator([&] { return now; });
        int forwards = 0;
        nstu::client::ExamTelemetryRouter router(coordinator, [&](std::span<const std::byte>) { ++forwards; });
        const auto enable = task9_enable();
        const auto raw = nstu::exam::encode_telemetry_enable(enable);
        assert(router.handle_restart(storage.outbox) && router.handle_server_enable(raw));
        assert(router.handle_agent_samples(std::vector{task9_sample()}, now)); // Real payload to retain during failed purge.
        {
            RouterStopSharingLock held(storage.outbox);
            auto sample = task9_sample();
            sample.collector_id = 12;
            assert(!router.handle_agent_samples(std::vector{sample}, now));
        }
        const std::filesystem::path state = storage.outbox.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto original_state = router_stop_file_bytes(state);
        const auto original_payload = router_stop_file_bytes(storage.outbox);
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        {
            RouterStopSharingLock held(block_payload ? storage.outbox : state);
            assert_router_disconnect(router.handle_disconnect_with_outcome(),
                {DisconnectState::enabled, DisconnectState::revoked, DisconnectPending::revoked_expired,
                 block_payload ? DisconnectPublication::current_protected_state_confirmed : DisconnectPublication::refusal_marker_confirmed,
                 DisconnectCleanup::pending, block_payload ? DisconnectError::cleanup_failed : DisconnectError::protected_state_failed});
            assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked && forwards == 1);
            if (block_payload) {
                assert(router_stop_file_bytes(storage.outbox) == original_payload);
                assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::revoked);
                assert(!std::filesystem::exists(marker));
            } else {
                assert(router_stop_file_bytes(state) == original_state);
                assert(router_stop_file_bytes(marker) == std::string(1, '\x01'));
                assert_router_disconnect_physical_empty_outbox(storage.outbox);
                nstu::exam::TelemetryOutbox inspect;
                assert(inspect.open(storage.outbox, now, nullptr) && inspect.size() == 0);
                inspect.close();
                nstu::client::ExamTelemetryServiceCoordinator blocked([&] { return now; });
                nstu::client::ExamTelemetryRouter blocked_router(blocked, [](std::span<const std::byte>) { assert(false); });
                assert(!blocked_router.handle_restart(storage.outbox) && !blocked.is_open());
                assert(!blocked_router.handle_server_enable(raw) && !blocked_router.poll_outbound_batch(now));
                assert(router_stop_file_bytes(state) == original_state && router_stop_file_bytes(marker) == std::string(1, '\x01'));
            }
        }
        const auto history = coordinator.last_error();
        assert(router.handle_maintenance(now));
        assert_router_disconnect_physical_empty_outbox(storage.outbox);
        assert_router_disconnect(router.handle_disconnect_with_outcome(),
            {DisconnectState::revoked, DisconnectState::revoked, DisconnectPending::not_present,
             DisconnectPublication::current_protected_state_confirmed, DisconnectCleanup::not_needed, DisconnectError::none});
        assert(coordinator.last_error() == history && coordinator.outbox_size() == 0);
        assert_router_stop_protected_lifecycle(storage.outbox, enable, nstu::client::TelemetryLifecycleState::revoked);
        assert(!std::filesystem::exists(marker) && !router.handle_server_enable(raw));
        assert(!router.poll_outbound_batch(now) && forwards == 1);
        router.handle_shutdown();
        assert_router_stop_reconstruction_refuses_old(storage, enable, now);
    }
}

void run_router_disconnect_outcome_tests() {
    test_router_disconnect_dispatch_compatibility_and_exceptions();
    test_router_disconnect_real_retention_and_reconstruction();
    test_router_disconnect_real_state_failure_retry();
    test_router_disconnect_real_pending_retry();
    test_router_disconnect_real_expiry_and_cleanup();
}

} // namespace

int main(int argc, char** argv) {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    if (argc == 2 && std::string_view(argv[1]) == "router-disconnect-outcomes") {
        run_router_disconnect_outcome_tests();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "router-stop-outcomes") {
        run_router_stop_outcome_tests();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "task9-baseline") {
        test_task9_real_restart_restores_storage();
        test_task9_decoder_preserves_specific_error();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "task9-decoder") {
        test_task9_decoder_preserves_specific_error();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "task9-scheduling") {
        test_low_priority_telemetry_scheduling();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "task9-real") {
        test_task9_real_restart_restores_storage();
        test_task9_decoder_preserves_specific_error();
        test_task9_real_router_lifecycle_and_exact_ack();
        test_task9_real_router_restore_failure_is_closed();
        test_task9_real_router_offline_maintenance();
        return 0;
    }
    nstu::client::ClientRuntimeConfig client_config;
    client_config.server_address = "192.0.2.1";
    client_config.client_id.fill(std::byte{0});
    for (std::size_t index = 0; index < client_config.client_id.size(); ++index) {
        client_config.client_id[index] = static_cast<std::byte>(index + 21);
    }
    client_config.key_id = 8;
    client_config.pre_shared_key.resize(
        nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < client_config.pre_shared_key.size();
         ++index) {
        client_config.pre_shared_key[index] =
            static_cast<std::byte>(0x60 + index);
    }

    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(client_config.client_id, client_config.key_id,
                            client_config.pre_shared_key, &error));
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig server_config;
    server_config.port = 0;
    assert(server.start(std::move(server_config), &error));
    client_config.server_port = server.local_port();

    std::stop_source stop_source;
    std::atomic_bool lock_received = false;
    std::atomic_bool snapshot_sent = false;
    std::atomic_bool session_succeeded = false;
    std::atomic_bool discovered_loopback = false;
    std::atomic<std::uint16_t> discovered_port = 0;
    nstu::client::ClientConnectionOptions connection_options;
    connection_options.connect_timeout_ms = 100;
    connection_options.discovery.timeout = std::chrono::milliseconds(500);
    connection_options.discovery.target_addresses = {"127.0.0.1"};
    std::thread client([&] {
        std::string client_error;
        const bool result = nstu::client::run_client_control_session(
            client_config, stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "SERVICE-PC";
                status.session_id = 2;
                return status;
            },
            [&](const nstu::control::AuthenticatedCommand& command) {
                if (command.envelope.type ==
                    nstu::protocol::CommandType::lock) {
                    lock_received = true;
                    stop_source.request_stop();
                }
            },
            [&]() -> std::optional<nstu::client::ClientOutboundCommand> {
                if (snapshot_sent.exchange(true)) {
                    return std::nullopt;
                }
                nstu::control::SnapshotFrame frame;
                frame.width = 320;
                frame.height = 180;
                frame.captured_at_unix_milliseconds = 42;
                frame.jpeg = {std::byte{0xff}, std::byte{0xd8},
                              std::byte{0xff}, std::byte{0xd9}};
                return nstu::client::ClientOutboundCommand{
                    nstu::protocol::CommandType::snapshot_frame,
                    nstu::control::encode_snapshot_frame(frame)};
            },
            &client_error,
            [&](const nstu::discovery::ServerEndpoint& endpoint) {
                discovered_loopback = endpoint.address == "127.0.0.1";
                discovered_port = endpoint.port;
            },
            connection_options);
        session_succeeded = result;
    });
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return clients.size() == 1 && clients[0].hostname == "SERVICE-PC";
    }));
    const auto registry_id = registry.snapshot()[0].id;
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return !clients.empty() && clients[0].snapshot_generation == 1 &&
               clients[0].snapshot_width == 320;
    }));
    assert(server.set_locked(registry_id, true, &error));
    assert(wait_until([&] { return lock_received.load(); }));
    client.join();
    assert(session_succeeded.load());
    assert(discovered_loopback.load());
    assert(discovered_port.load() == server.local_port());

    auto rejected_config = client_config;
    rejected_config.server_address = "127.0.0.1";
    rejected_config.pre_shared_key[0] ^= static_cast<std::byte>(0x5a);
    nstu::client::ClientConnectionOptions rejected_options;
    rejected_options.connect_timeout_ms = 100;
    rejected_options.discovery.timeout = std::chrono::milliseconds(250);
    rejected_options.discovery.target_addresses = {"127.0.0.1"};
    std::atomic_bool unverified_endpoint_observed = false;
    std::stop_source rejected_stop_source;
    std::string rejected_error;
    const bool rejected = nstu::client::run_client_control_session(
        rejected_config, rejected_stop_source.get_token(),
        [] {
            nstu::control::ClientStatusReport status;
            status.hostname = "REJECTED-PC";
            return status;
        },
        [](const nstu::control::AuthenticatedCommand&) {}, {}, &rejected_error,
        [&](const nstu::discovery::ServerEndpoint&) {
            unverified_endpoint_observed = true;
        },
        rejected_options);
    assert(!rejected);
    assert(!rejected_error.empty());
    assert(!unverified_endpoint_observed.load());
    nstu::client::clear_client_runtime_config(rejected_config);

    // Direct-hit reconnect: the cached endpoint is correct, so the direct
    // branch of the parallel race wins on loopback without needing discovery.
    // The pre-shared key is untouched from the first session - reconnect never
    // re-exchanges the secret.
    auto direct_config = client_config;
    direct_config.server_address = "127.0.0.1";
    nstu::client::ClientConnectionOptions direct_options;
    direct_options.connect_timeout_ms = 1000;
    direct_options.direct_connect_timeout_ms = 500;
    // No discovery targets: if the direct branch did not carry this, the race
    // would have nothing to fall back to and the case would fail.
    direct_options.discovery.timeout = std::chrono::milliseconds(250);
    std::stop_source direct_stop_source;
    std::atomic_bool direct_lock_received = false;
    std::atomic_bool direct_session_succeeded = false;
    std::atomic_bool direct_endpoint_observed = false;
    std::thread direct_client([&] {
        std::string direct_error;
        const bool result = nstu::client::run_client_control_session(
            direct_config, direct_stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "DIRECT-PC";
                return status;
            },
            [&](const nstu::control::AuthenticatedCommand& command) {
                if (command.envelope.type ==
                    nstu::protocol::CommandType::lock) {
                    direct_lock_received = true;
                    direct_stop_source.request_stop();
                }
            },
            {}, &direct_error,
            [&](const nstu::discovery::ServerEndpoint& endpoint) {
                if (endpoint.address == "127.0.0.1" &&
                    endpoint.port == server.local_port()) {
                    direct_endpoint_observed = true;
                }
            },
            direct_options);
        direct_session_succeeded = result;
    });
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return !clients.empty() && clients[0].hostname == "DIRECT-PC";
    }));
    const auto direct_registry_id = registry.snapshot()[0].id;
    assert(server.set_locked(direct_registry_id, true, &error));
    assert(wait_until([&] { return direct_lock_received.load(); }));
    direct_client.join();
    assert(direct_session_succeeded.load());
    assert(direct_endpoint_observed.load());
    // The reconnect cache still holds the same secret it started with.
    assert(direct_config.pre_shared_key == client_config.pre_shared_key);
    nstu::client::clear_client_runtime_config(direct_config);

    server.stop();

    // Server-absent reconnect: with the server stopped, neither the cached
    // endpoint nor authenticated discovery can produce a channel, so the race
    // fails cleanly with a reported error and observes no endpoint.
    auto absent_config = client_config;
    absent_config.server_address = "192.0.2.1";
    nstu::client::ClientConnectionOptions absent_options;
    absent_options.connect_timeout_ms = 200;
    absent_options.direct_connect_timeout_ms = 150;
    absent_options.discovery.timeout = std::chrono::milliseconds(250);
    absent_options.discovery.target_addresses = {"127.0.0.1"};
    std::atomic_bool absent_endpoint_observed = false;
    std::stop_source absent_stop_source;
    std::string absent_error;
    const bool absent = nstu::client::run_client_control_session(
        absent_config, absent_stop_source.get_token(),
        [] {
            nstu::control::ClientStatusReport status;
            status.hostname = "ABSENT-PC";
            return status;
        },
        [](const nstu::control::AuthenticatedCommand&) {}, {}, &absent_error,
        [&](const nstu::discovery::ServerEndpoint&) {
            absent_endpoint_observed = true;
        },
        absent_options);
    assert(!absent);
    assert(!absent_error.empty());
    assert(!absent_endpoint_observed.load());
    nstu::client::clear_client_runtime_config(absent_config);

    nstu::client::clear_client_runtime_config(client_config);

    run_router_stop_outcome_tests();
    run_router_disconnect_outcome_tests();
    test_exam_telemetry_router();
    test_task9_real_restart_restores_storage();
    test_task9_decoder_preserves_specific_error();
    test_task9_real_router_lifecycle_and_exact_ack();
    test_task9_real_router_restore_failure_is_closed();
    test_task9_real_router_offline_maintenance();
    test_low_priority_telemetry_scheduling();
    return 0;
}
