#include "nstu/exam_telemetry_service.hpp"
#include "nstu/secret_store.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Coordinator = nstu::client::ExamTelemetryServiceCoordinator;
static_assert(!std::is_reference_v<decltype(std::declval<const Coordinator&>().context())>);
static_assert(!std::is_reference_v<decltype(std::declval<const Coordinator&>().active_producer_id())>);
static_assert(!std::is_reference_v<decltype(std::declval<const Coordinator&>().aggregate_loss())>);
static_assert(!std::is_move_constructible_v<Coordinator>);
static_assert(!std::is_move_assignable_v<Coordinator>);

void test_unauthorized_samples_are_separate_from_loss() {
    Coordinator coordinator;
    const auto path = std::filesystem::temp_directory_path() / "nstu_telemetry_service_tests" / "coordinator_unauthorized.bin";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.wstring() + L".coord_state", ec);
    assert(coordinator.open(path));
    const std::vector<nstu::exam::TelemetrySample> samples(65);
    assert(!coordinator.ingest_samples(samples));
    assert(coordinator.dropped_sample_count() == 0);
    assert(coordinator.dropped_batch_count() == 0);
    assert(coordinator.not_authorized_sample_count() == 65);
}

nstu::exam::TelemetryEnable make_enable(const std::string& package_id = "math-pkg",
                                        const std::string& candidate_id = "cand-01") {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = package_id;
    enable.package_digest = nstu::security::Sha256Digest{std::byte{0x12}, std::byte{0x34}};
    enable.client_id = nstu::security::ClientId{std::byte{0x56}, std::byte{0x78}};
    enable.session_id = nstu::exam::SessionId{std::byte{0x9a}, std::byte{0xbc}};
    enable.candidate_id = candidate_id;
    enable.server_time_unix_milliseconds = 1700000000000ULL;
    return enable;
}

nstu::exam::TelemetrySample make_sample(std::uint64_t collector_id,
                                        std::uint32_t count = 1) {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_dwell;
    sample.source = nstu::exam::TelemetrySource::browser;
    sample.collector_id = collector_id;
    sample.question_id = "math-q1";
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 2000;
    sample.duration_milliseconds = 1500;
    sample.client_time_unix_milliseconds = 1700000001000ULL;
    sample.numeric_value = 0;
    sample.count = count;
    sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}

std::filesystem::path temp_outbox_path(const std::string& tag) {
    const auto dir = std::filesystem::temp_directory_path() / "nstu_telemetry_service_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / ("coordinator_" + tag + ".bin");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.wstring() + L".coord_state", ec);
    std::filesystem::remove(path.wstring() + L".coord_state.blocked", ec);
    std::filesystem::remove(path.wstring() + L".revoked", ec);
    return path;
}

std::uint64_t test_now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::span<const std::byte> entropy_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

void append_number(std::vector<std::byte>& bytes, std::uint64_t number, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) bytes.push_back(static_cast<std::byte>((number >> (8u * i)) & 0xffu));
}

void put_number(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t number, std::size_t size) {
    assert(offset + size <= bytes.size());
    for (std::size_t i = 0; i < size; ++i) bytes[offset + i] = static_cast<std::byte>((number >> (8u * i)) & 0xffu);
}

void save_fixture(const std::filesystem::path& path, std::vector<std::byte> payload, std::string_view entropy) {
    const auto digest = nstu::security::sha256(payload);
    assert(digest);
    payload.insert(payload.end(), digest->begin(), digest->end());
    assert(nstu::security::save_machine_secret(path.wstring(), payload, entropy_bytes(entropy)));
}

// Independently construct a supported v1 outbox fixture; avoids thousands of
// durable rewrites just to exercise the coordinator's quota boundary.
void write_outbox_fixture(const std::filesystem::path& path,
                          const std::vector<nstu::exam::TelemetryBatch>& batches) {
    std::vector<std::byte> payload;
    append_number(payload, 0x544c4d4f, 4);
    append_number(payload, 1, 2);
    append_number(payload, 0, 2);
    for (int i = 0; i < 5; ++i) append_number(payload, 0, 8);
    append_number(payload, batches.size(), 4);
    for (const auto& batch : batches) {
        const auto encoded = nstu::exam::encode_telemetry_batch(batch);
        assert(!encoded.empty());
        append_number(payload, encoded.size(), 4);
        payload.insert(payload.end(), encoded.begin(), encoded.end());
        append_number(payload, 0, 2);
        append_number(payload, 0, 8);
    }
    save_fixture(path, std::move(payload), "NSTU-Exam-Telemetry-Outbox-v1");
}

std::vector<std::byte> coordinator_fixture_payload(const std::filesystem::path& path) {
    auto payload = nstu::security::load_machine_secret(path.wstring() + L".coord_state",
        entropy_bytes("NSTU-Exam-Telemetry-Service-Coordinator-v1"));
    assert(payload.size() >= 32);
    payload.resize(payload.size() - 32);
    return payload;
}

std::size_t sequence_fixture_offset(const nstu::exam::TelemetryEnable& enable) {
    return 96 + enable.package_id.size() + enable.candidate_id.size();
}

void test_full_outbox_counts_every_sample_and_saturates() {
    const auto path = temp_outbox_path("full_quota");
    const auto enable = make_enable();
    const auto now = test_now_ms();
    nstu::exam::TelemetryBatch exemplar;
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        const std::vector samples{make_sample(1)};
        assert(coordinator.ingest_samples(samples, now));
        exemplar = *coordinator.next_batch_to_send(now);
        coordinator.close();
    }
    std::vector<nstu::exam::TelemetryBatch> batches;
    for (std::uint64_t seq = 1; seq <= nstu::exam::kMaximumTelemetryOutboxBatches; ++seq) {
        exemplar.sequence = seq;
        batches.push_back(exemplar);
    }
    write_outbox_fixture(path, batches);
    auto payload = coordinator_fixture_payload(path);
    put_number(payload, sequence_fixture_offset(enable), 4097, 8);
    save_fixture(path.wstring() + L".coord_state", payload, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        const std::vector<nstu::exam::TelemetrySample> samples(65, make_sample(2));
        assert(!coordinator.ingest_samples(samples, now));
        assert(coordinator.outbox_size() == 4096);
        assert(coordinator.next_sequence() == 4097);
        assert(coordinator.dropped_sample_count() == 65);
        assert(coordinator.dropped_batch_count() == 3);
        coordinator.close();
    }
    payload = coordinator_fixture_payload(path);
    const auto loss_offset = sequence_fixture_offset(enable) + 8 + 4 + 1;
    put_number(payload, loss_offset, std::numeric_limits<std::uint64_t>::max() - 1, 8);
    put_number(payload, loss_offset + 8, std::numeric_limits<std::uint64_t>::max() - 1, 8);
    save_fixture(path.wstring() + L".coord_state", payload, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    Coordinator restarted;
    assert(restarted.open(path));
    assert(restarted.on_server_enable(enable));
    const std::vector<nstu::exam::TelemetrySample> samples(65, make_sample(2));
    assert(!restarted.ingest_samples(samples, now));
    assert(restarted.dropped_sample_count() == std::numeric_limits<std::uint64_t>::max());
    assert(restarted.dropped_batch_count() == std::numeric_limits<std::uint64_t>::max());
}

void test_producer_and_sequence_assignment() {
    const auto time_base = test_now_ms();
    const auto path = temp_outbox_path("producer_seq");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::client::ExamTelemetryServiceCoordinator coordinator;
    assert(coordinator.open(path));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::disabled);

    // 1. Samples rejected before enable
    std::vector<nstu::exam::TelemetrySample> samples = {make_sample(42, 1)};
    std::string error;
    assert(!coordinator.ingest_samples(samples, time_base, &error));
    assert(coordinator.outbox_size() == 0);

    // 2. Enable binds context, generates random nonzero producer ID, sequence starts at 1
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::enabled);
    assert(coordinator.generation_count() == 1);
    assert(coordinator.next_sequence() == 1);

    const auto prod_id = coordinator.active_producer_id();
    bool is_zero = true;
    for (const auto b : prod_id) {
        if (b != std::byte{0}) is_zero = false;
    }
    assert(!is_zero);

    // 3. Ingesting samples batches them and assigns sequence 1
    assert(coordinator.ingest_samples(samples, time_base + 1000));
    assert(coordinator.outbox_size() == 1);
    assert(coordinator.next_sequence() == 2);

    auto batch1 = coordinator.next_batch_to_send(time_base + 1000);
    assert(batch1.has_value());
    assert(batch1->sequence == 1);
    assert(batch1->producer_id == prod_id);
    assert(batch1->package_id == enable.package_id);
    assert(batch1->candidate_id == enable.candidate_id);
    assert(batch1->samples.size() == 1);

    // 4. Ingesting 35 samples splits into 2 batches (32 + 3) with monotonic sequences 2 and 3
    std::vector<nstu::exam::TelemetrySample> large_samples;
    for (std::uint32_t i = 0; i < 35; ++i) {
        large_samples.push_back(make_sample(42, i + 1));
    }
    assert(coordinator.ingest_samples(large_samples, time_base + 2000));
    assert(coordinator.outbox_size() == 3);
    assert(coordinator.next_sequence() == 4);

    auto batch2 = coordinator.next_batch_to_send(time_base + 2000);
    assert(batch2.has_value());
    assert(batch2->sequence == 2);
    assert(batch2->samples.size() == 32);

    auto batch3 = coordinator.next_batch_to_send(time_base + 2000);
    assert(batch3.has_value());
    assert(batch3->sequence == 3);
    assert(batch3->samples.size() == 3);
}

void test_lifecycle_and_reconnect_semantics() {
    const auto time_base = test_now_ms();
    const auto path = temp_outbox_path("lifecycle_reconnect");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::client::ExamTelemetryServiceCoordinator coordinator;
    assert(coordinator.open(path));

    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));

    std::vector<nstu::exam::TelemetrySample> s1 = {make_sample(10, 1)};
    assert(coordinator.ingest_samples(s1, time_base + 1000));
    assert(coordinator.outbox_size() == 1);

    // Send batch 1 (marks in-flight)
    auto b1 = coordinator.next_batch_to_send(time_base + 1000);
    assert(b1.has_value());

    // 1. Disconnect suspends collection, clears in-flight, forbids upload
    coordinator.on_disconnect();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    // Ingesting while suspended rejected
    assert(!coordinator.ingest_samples(s1, time_base + 2000));
    // Uploading while suspended forbidden
    assert(!coordinator.next_batch_to_send(time_base + 2000).has_value());
    assert(coordinator.outbox_size() == 1);

    // 2. Re-enable with matching context resumes collection and upload, keeps active producer ID
    const auto prod_id_before = coordinator.active_producer_id();
    assert(coordinator.on_server_enable(enable));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::enabled);
    assert(coordinator.active_producer_id() == prod_id_before);

    // Batch 1 is eligible again since in-flight was cleared on disconnect
    auto b1_resumed = coordinator.next_batch_to_send(time_base + 3000);
    assert(b1_resumed.has_value());
    assert(b1_resumed->sequence == 1);

    // 3. ACK retires batch 1
    nstu::exam::TelemetryAck ack;
    ack.status = nstu::exam::TelemetryAckStatus::accepted;
    ack.session_id = b1->session_id;
    ack.producer_id = b1->producer_id;
    ack.sequence = b1->sequence;
    ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(*b1);
    assert(coordinator.on_server_ack(ack, time_base + 3500) == nstu::exam::TelemetryAckApplyResult::retired);
    assert(coordinator.outbox_size() == 0);

    // 4. Explicit stop revokes context, purges outbox, forbids later samples and enable
    coordinator.on_explicit_stop();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(coordinator.outbox_size() == 0);
    assert(!coordinator.ingest_samples(s1, time_base + 4000));
    assert(!coordinator.on_server_enable(enable));
}

void test_stale_context_and_restart() {
    const auto time_base = test_now_ms();
    const auto path = temp_outbox_path("stale_restart");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        nstu::client::ExamTelemetryServiceCoordinator coordinator;
        assert(coordinator.open(path));

        const auto enable = make_enable("pkg-A", "cand-A");
        assert(coordinator.on_server_enable(enable));

        std::vector<nstu::exam::TelemetrySample> s = {make_sample(1)};
        assert(coordinator.ingest_samples(s, time_base + 1000));
        assert(coordinator.outbox_size() == 1);
        coordinator.close();
    }

    // Reopening (restart):
    nstu::client::ExamTelemetryServiceCoordinator restarted;
    assert(restarted.open(path));
    // Cannot upload before matching enable
    assert(!restarted.next_batch_to_send(time_base + 2000).has_value());

    // Mismatched context enable is rejected
    const auto wrong_enable = make_enable("pkg-WRONG", "cand-A");
    assert(!restarted.on_server_enable(wrong_enable));
    assert(!restarted.next_batch_to_send(time_base + 2000).has_value());

    // Matching enable resumes
    const auto matching = make_enable("pkg-A", "cand-A");
    assert(restarted.on_server_enable(matching));
    assert(restarted.outbox_size() == 1);
    auto b = restarted.next_batch_to_send(time_base + 3000);
    assert(b.has_value());
    assert(b->sequence == 1);
}

void test_reopen_preserves_active_state() {
    const auto time_base = test_now_ms();
    const auto path = temp_outbox_path("reopen");
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    const auto producer = coordinator.active_producer_id();
    const std::vector samples{make_sample(1)};
    assert(coordinator.ingest_samples(samples, time_base + 1000));
    assert(coordinator.open(path));
    assert(!coordinator.next_batch_to_send(time_base + 2000));
    assert(coordinator.on_server_enable(enable));
    assert(coordinator.active_producer_id() == producer);
    assert(coordinator.next_sequence() == 2);
    assert(coordinator.outbox_size() == 1);
}

void test_revoked_identities_survive_new_exam_and_restart() {
    const auto path = temp_outbox_path("revoked_history");
    const auto a = make_enable("pkg-A");
    const auto b = make_enable("pkg-B");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(a));
        coordinator.on_explicit_stop();
        assert(!coordinator.on_server_enable(a));
        assert(coordinator.on_server_enable(b));
        assert(coordinator.generation_count() == 1);
        coordinator.on_explicit_stop();
        coordinator.close();
    }
    Coordinator restarted;
    assert(restarted.open(path));
    assert(!restarted.on_server_enable(a));
    assert(!restarted.on_server_enable(b));
    assert(restarted.on_server_enable(make_enable("pkg-C")));
}

void test_corrupt_state_is_preserved_and_refused() {
    const auto path = temp_outbox_path("corrupt_state");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(make_enable()));
        coordinator.on_explicit_stop();
        coordinator.close();
    }
    const std::filesystem::path state(path.wstring() + L".coord_state");
    { std::ofstream stream(state, std::ios::binary | std::ios::trunc); stream << "CORRUPT"; }
    Coordinator restarted;
    std::string error;
    assert(!restarted.open(path, &error));
    assert(!error.empty());
    assert(!restarted.on_server_enable(make_enable("another-exam")));
    restarted.close();
    assert(std::filesystem::file_size(state) == 7);
}

void test_missing_state_refuses_orphan_batches() {
    const auto time_base = test_now_ms();
    const auto path = temp_outbox_path("orphan");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(make_enable()));
        const std::vector samples{make_sample(1)};
        assert(coordinator.ingest_samples(samples, time_base + 1000));
        coordinator.close();
    }
    assert(std::filesystem::remove(path.wstring() + L".coord_state"));
    Coordinator restarted;
    std::string error;
    assert(!restarted.open(path, &error));
    assert(!error.empty());
    assert(!restarted.next_batch_to_send(time_base + 2000));
    assert(!restarted.on_server_enable(make_enable("different-exam")));
}

class LockedFile {
public:
    explicit LockedFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        assert(handle_ != INVALID_HANDLE_VALUE);
    }
    ~LockedFile() { assert(CloseHandle(handle_)); }
    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;
private:
    HANDLE handle_;
};

void test_failed_enqueue_intent_recovers_after_restart() {
    const auto path = temp_outbox_path("enqueue_intent");
    const auto enable = make_enable();
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    nstu::exam::ProducerId producer;
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        producer = coordinator.active_producer_id();
        const std::vector samples{make_sample(77)};
        LockedFile locked(path);
        std::string error;
        assert(!coordinator.ingest_samples(samples, now, &error));
        assert(!error.empty());
        assert(!coordinator.next_batch_to_send(now));
        coordinator.close(); // intent must survive even when enqueue cannot finish
    }
    Coordinator restarted;
    assert(restarted.open(path));
    assert(restarted.on_server_enable(enable));
    const auto batch = restarted.next_batch_to_send(now);
    assert(batch);
    assert(batch->sequence == 1 && batch->producer_id == producer);
    assert(batch->samples.front().collector_id == 77);
    assert(restarted.next_sequence() == 2);
    assert(restarted.dropped_sample_count() == 0);
}

void test_full_revocation_archive_is_reported() {
    const auto path = temp_outbox_path("full_archive");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        for (std::size_t i = 0; i <= nstu::client::kMaximumRevokedTelemetryIdentities; ++i) {
            assert(coordinator.on_server_enable(make_enable("pkg-" + std::to_string(i))));
            coordinator.on_explicit_stop();
        }
        coordinator.close();
    }
    Coordinator restarted;
    assert(restarted.open(path));
    std::string error;
    assert(!restarted.on_server_enable(make_enable("pkg-next"), &error));
    assert(error.find("archive full") != std::string::npos);
    for (std::size_t i = 0; i <= nstu::client::kMaximumRevokedTelemetryIdentities; ++i) {
        assert(!restarted.on_server_enable(make_enable("pkg-" + std::to_string(i))));
    }
}

void test_stop_before_first_enable_does_not_poison_restart() {
    const auto path = temp_outbox_path("unbound_stop");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        coordinator.on_explicit_stop();
        coordinator.close();
    }
    Coordinator restarted;
    assert(restarted.open(path));
    assert(restarted.on_server_enable(make_enable()));
}

void test_new_packets_dropped_during_pending_failure_are_counted() {
    const auto path = temp_outbox_path("pending_drop");
    Coordinator coordinator;
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    {
        LockedFile locked(path);
        const std::vector pending{make_sample(1)};
        assert(!coordinator.ingest_samples(pending, now));
        const std::vector<nstu::exam::TelemetrySample> incoming(65, make_sample(2));
        assert(!coordinator.ingest_samples(incoming, now));
        assert(coordinator.dropped_sample_count() == 65);
        assert(coordinator.dropped_batch_count() == 3);
    }
    const auto batch = coordinator.next_batch_to_send(now);
    assert(batch && batch->samples.front().collector_id == 1);
    assert(coordinator.next_sequence() == 2);
}

void test_stop_storage_failures_block_restart_and_retry_cleanup() {
    const auto path = temp_outbox_path("stop_state_failure");
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    const std::vector samples{make_sample(1)};
    assert(coordinator.ingest_samples(samples, test_now_ms()));
    {
        LockedFile locked(path.wstring() + L".coord_state");
        coordinator.on_explicit_stop();
        assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(!coordinator.next_batch_to_send());
        assert(!coordinator.last_error().empty());
        assert(std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
        Coordinator restarted;
        assert(!restarted.open(path));
        assert(!coordinator.on_server_enable(make_enable("pkg-B")));
    }
    // Retry persists revocation and cleanup before accepting another identity.
    assert(coordinator.on_server_enable(make_enable("pkg-B")));
    assert(!std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
    assert(!coordinator.on_server_enable(enable));
    coordinator.on_explicit_stop();
    coordinator.close();
    Coordinator restarted;
    assert(restarted.open(path));
    assert(!restarted.on_server_enable(enable));

    const auto payload_path = temp_outbox_path("stop_payload_failure");
    Coordinator payload;
    assert(payload.open(payload_path));
    assert(payload.on_server_enable(enable));
    assert(payload.ingest_samples(samples, test_now_ms()));
    {
        LockedFile locked(payload_path);
        payload.on_explicit_stop();
        assert(!payload.next_batch_to_send());
        assert(!payload.last_error().empty());
    }
    assert(payload.on_server_enable(make_enable("pkg-C")));
    assert(payload.outbox_size() == 0);
}

nstu::exam::TelemetryBatch make_pending_fixture(const std::filesystem::path& path) {
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    nstu::exam::TelemetryBatch batch{enable.package_id, enable.package_digest,
        enable.client_id, enable.session_id, enable.candidate_id, coordinator.active_producer_id(),
        1, test_now_ms(), {make_sample(31)}};
    LockedFile locked(path);
    assert(!coordinator.ingest_samples(batch.samples, batch.first_enqueued_at_unix_milliseconds));
    coordinator.close();
    return batch;
}

void test_pending_duplicate_conflict_and_expiry_recovery() {
    const auto path = temp_outbox_path("pending_exact");
    const auto expected = make_pending_fixture(path);
    write_outbox_fixture(path, {expected});
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.open(path)); // repeated recovery never duplicates
        assert(coordinator.outbox_size() == 1);
        assert(coordinator.next_sequence() == 2);
        assert(coordinator.on_server_enable(make_enable()));
        const auto batch = coordinator.next_batch_to_send();
        assert(batch && nstu::exam::encode_telemetry_batch(*batch) == nstu::exam::encode_telemetry_batch(expected));
        nstu::exam::TelemetryAck ack{expected.session_id, expected.producer_id, 1,
            *nstu::exam::compute_telemetry_batch_hash(expected), test_now_ms(), nstu::exam::TelemetryAckStatus::accepted};
        assert(coordinator.on_server_ack(ack) == nstu::exam::TelemetryAckApplyResult::retired);
        coordinator.close();
    }
    Coordinator restarted;
    assert(restarted.open(path));
    assert(restarted.on_server_enable(make_enable()));
    assert(restarted.outbox_size() == 0 && restarted.next_sequence() == 2);
    assert(restarted.active_producer_id() == expected.producer_id);

    const auto conflict_path = temp_outbox_path("pending_conflict");
    const auto pending = make_pending_fixture(conflict_path);
    auto conflict = pending;
    conflict.samples.front().collector_id = 32;
    write_outbox_fixture(conflict_path, {conflict});
    Coordinator conflicted;
    assert(!conflicted.open(conflict_path));
    assert(!conflicted.last_error().empty());

    const auto duplicate_path = temp_outbox_path("pending_duplicate_conflict");
    const auto duplicate = make_pending_fixture(duplicate_path);
    conflict = duplicate;
    conflict.samples.front().collector_id = 32;
    write_outbox_fixture(duplicate_path, {duplicate, conflict});
    Coordinator duplicate_conflict;
    assert(!duplicate_conflict.open(duplicate_path));

    const auto expired_path = temp_outbox_path("pending_expired");
    auto expired = make_pending_fixture(expired_path);
    const auto original_bytes = nstu::exam::encode_telemetry_batch(expired);
    auto payload = coordinator_fixture_payload(expired_path);
    assert(std::equal(original_bytes.begin(), original_bytes.end(), payload.end() - original_bytes.size()));
    expired.first_enqueued_at_unix_milliseconds = test_now_ms() - nstu::exam::kTelemetryRetentionMilliseconds;
    const auto expired_bytes = nstu::exam::encode_telemetry_batch(expired);
    assert(expired_bytes.size() == original_bytes.size());
    std::copy(expired_bytes.begin(), expired_bytes.end(), payload.end() - expired_bytes.size());
    save_fixture(expired_path.wstring() + L".coord_state", payload, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    Coordinator expired_coordinator;
    assert(!expired_coordinator.open(expired_path));
    assert(!expired_coordinator.next_batch_to_send());
    Coordinator expired_restart;
    assert(expired_restart.open(expired_path));
    assert(!expired_restart.on_server_enable(make_enable()));
    assert(expired_restart.outbox_size() == 0);
}

void test_coordinator_loss_counters_expire_after_thirty_days() {
    const auto path = temp_outbox_path("loss_expiry");
    Coordinator coordinator;
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
    const auto now = test_now_ms();
    const std::vector<nstu::exam::TelemetrySample> invalid(65);
    assert(!coordinator.ingest_samples(invalid, now));
    assert(coordinator.dropped_sample_count() == 65);
    coordinator.on_disconnect();
    assert(!coordinator.ingest_samples(invalid, now));
    assert(coordinator.not_authorized_sample_count() == 65);
    (void)coordinator.prune_expired(now + nstu::exam::kTelemetryRetentionMilliseconds);
    assert(coordinator.dropped_sample_count() == 0);
    assert(coordinator.dropped_batch_count() == 0);
    assert(coordinator.not_authorized_sample_count() == 0);
}

void test_first_unauthorized_packet_after_expiry_is_counted() {
    const auto path = temp_outbox_path("loss_expiry_packet");
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto old_time = test_now_ms() - nstu::exam::kTelemetryRetentionMilliseconds - 1;
    const std::vector<nstu::exam::TelemetrySample> invalid(65);
    assert(!coordinator.ingest_samples(invalid, old_time));
    assert(coordinator.not_authorized_sample_count() == 65);
    const std::vector<nstu::exam::TelemetrySample> incoming(1);
    assert(!coordinator.ingest_samples(incoming, test_now_ms()));
    assert(coordinator.not_authorized_sample_count() == 1);
}

void test_future_saved_loss_timestamp_cannot_extend_retention() {
    const auto path = temp_outbox_path("loss_future");
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        const std::vector<nstu::exam::TelemetrySample> invalid(65);
        assert(!coordinator.ingest_samples(invalid, test_now_ms()));
        coordinator.close();
    }
    auto payload = coordinator_fixture_payload(path);
    const auto first_loss_offset = sequence_fixture_offset({}) + 8 + 4 + 1 + 8 + 8 + 8;
    put_number(payload, first_loss_offset, test_now_ms() + 2 * nstu::exam::kTelemetryRetentionMilliseconds, 8);
    save_fixture(path.wstring() + L".coord_state", payload, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    Coordinator restarted;
    assert(restarted.open(path));
    assert(restarted.not_authorized_sample_count() == 65);
    (void)restarted.prune_expired(test_now_ms() + nstu::exam::kTelemetryRetentionMilliseconds + 1);
    assert(restarted.not_authorized_sample_count() == 0);
}

void test_state_write_failure_blocks_mutations_until_recovery() {
    const auto path = temp_outbox_path("state_failure");
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto enable = make_enable();
    {
        LockedFile locked(path.wstring() + L".coord_state");
        assert(!coordinator.on_server_enable(enable));
        assert(!coordinator.next_batch_to_send());
        assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    }
    assert(coordinator.on_server_enable(enable));
    const auto now = test_now_ms();
    const std::vector samples{make_sample(1)};
    {
        LockedFile locked(path.wstring() + L".coord_state");
        assert(!coordinator.ingest_samples(samples, now));
        assert(!coordinator.next_batch_to_send(now));
        assert(coordinator.outbox_size() == 0 && coordinator.next_sequence() == 1);
        assert(coordinator.on_server_ack({}) == nstu::exam::TelemetryAckApplyResult::storage_failed);
        assert(coordinator.prune_expired(now) == 0);
        assert(!coordinator.on_server_enable(make_enable("pkg-wrong")));
        coordinator.close();
        assert(coordinator.is_open());
    }
    const auto batch = coordinator.next_batch_to_send(now);
    assert(batch && batch->sequence == 1);
    assert(coordinator.next_sequence() == 2);
}

void test_ack_variants_and_shutdown_retention() {
    const auto path = temp_outbox_path("ack_shutdown");
    Coordinator coordinator;
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    const auto now = test_now_ms();
    const std::vector samples{make_sample(1)};
    assert(coordinator.ingest_samples(samples, now));
    const auto batch = coordinator.next_batch_to_send(now);
    assert(batch);
    nstu::exam::TelemetryAck ack{batch->session_id, batch->producer_id, batch->sequence,
        *nstu::exam::compute_telemetry_batch_hash(*batch), now, nstu::exam::TelemetryAckStatus::unavailable};
    assert(coordinator.on_server_ack(ack, now) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!coordinator.next_batch_to_send(now + 1000));
    assert(coordinator.next_batch_to_send(now + 2000));
    auto wrong = ack;
    wrong.batch_hash.front() ^= std::byte{1};
    wrong.status = nstu::exam::TelemetryAckStatus::accepted;
    assert(coordinator.on_server_ack(wrong, now) == nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(coordinator.outbox_size() == 1);
    coordinator.on_service_shutdown();
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(!coordinator.next_batch_to_send(now + 3000));
    assert(coordinator.outbox_size() == 1);
    assert(coordinator.on_server_enable(enable));
    assert(coordinator.next_batch_to_send(now + 3000));
    ack.status = nstu::exam::TelemetryAckStatus::duplicate;
    assert(coordinator.on_server_ack(ack, now + 3000) == nstu::exam::TelemetryAckApplyResult::retired);
    for (const auto status : {nstu::exam::TelemetryAckStatus::rejected, nstu::exam::TelemetryAckStatus::conflict}) {
        assert(coordinator.ingest_samples(samples, now + 4000));
        const auto terminal_batch = coordinator.next_batch_to_send(now + 4000);
        assert(terminal_batch);
        ack.sequence = terminal_batch->sequence;
        ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(*terminal_batch);
        ack.status = status;
        assert(coordinator.on_server_ack(ack, now + 4000) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
        assert(coordinator.outbox_size() == 0);
    }
    const auto loss = coordinator.aggregate_loss();
    assert(loss.rejected_batches == 1 && loss.conflict_batches == 1);
}

void test_idle_provider_expires_suspended_batches_and_disabled_loss() {
    const auto now = test_now_ms();
    Coordinator suspended;
    assert(suspended.open(temp_outbox_path("idle_suspended")));
    assert(suspended.on_server_enable(make_enable()));
    const std::vector samples{make_sample(1)};
    assert(suspended.ingest_samples(samples, now));
    suspended.on_disconnect();
    assert(!suspended.next_batch_to_send(now + nstu::exam::kTelemetryRetentionMilliseconds));
    assert(suspended.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(suspended.outbox_size() == 0);
    assert(suspended.aggregate_loss().expired_batches == 1);

    Coordinator disabled;
    assert(disabled.open(temp_outbox_path("idle_disabled")));
    assert(!disabled.ingest_samples(samples, now));
    assert(disabled.not_authorized_sample_count() == 1);
    assert(!disabled.next_batch_to_send(now + nstu::exam::kTelemetryRetentionMilliseconds));
    assert(disabled.not_authorized_sample_count() == 0);
}

void write_aged_arrival_fixture(const std::filesystem::path& path,
                                const nstu::exam::TelemetryBatch& batch,
                                std::uint64_t arrival) {
    std::vector<std::byte> payload;
    append_number(payload, 0x544c4d4f, 4);
    append_number(payload, 2, 2);
    append_number(payload, 0, 2);
    payload.insert(payload.end(), batch.session_id.begin(), batch.session_id.end());
    append_number(payload, 0, 4); // loss contexts
    append_number(payload, 1, 4); // records
    const auto encoded = nstu::exam::encode_telemetry_batch(batch);
    assert(!encoded.empty());
    append_number(payload, encoded.size(), 4);
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    append_number(payload, 0, 2);
    append_number(payload, 0, 8);
    append_number(payload, arrival, 8);
    save_fixture(path, std::move(payload), "NSTU-Exam-Telemetry-Outbox-v1");
}

void test_pending_future_wire_cannot_renew_an_expired_arrival() {
    const auto path = temp_outbox_path("wal_aged_arrival");
    auto pending = make_pending_fixture(path);
    const auto old_bytes = nstu::exam::encode_telemetry_batch(pending);
    auto state = coordinator_fixture_payload(path);
    assert(std::equal(old_bytes.begin(), old_bytes.end(), state.end() - old_bytes.size()));
    const auto now = test_now_ms();
    pending.first_enqueued_at_unix_milliseconds = now + 90ULL * 86400ULL * 1000ULL;
    const auto future_bytes = nstu::exam::encode_telemetry_batch(pending);
    assert(future_bytes.size() == old_bytes.size());
    std::copy(future_bytes.begin(), future_bytes.end(), state.end() - future_bytes.size());
    save_fixture(path.wstring() + L".coord_state", state, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    write_aged_arrival_fixture(path, pending, now - nstu::exam::kTelemetryRetentionMilliseconds);

    Coordinator recovered;
    assert(!recovered.open(path));
    assert(!recovered.next_batch_to_send(now));
    Coordinator restarted;
    assert(restarted.open(path));
    assert(!restarted.on_server_enable(make_enable()));
    assert(restarted.outbox_size() == 0);
}

void test_maintenance_reports_failure_and_preserves_pending_intent() {
    const auto path = temp_outbox_path("maintenance_pending");
    auto now = test_now_ms();
    Coordinator coordinator([&] { return now; });
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    const auto producer = coordinator.active_producer_id();
    const std::vector samples{make_sample(91)};
    {
        LockedFile locked(path);
        assert(!coordinator.ingest_samples(samples));
        coordinator.on_disconnect();
        const auto intent = coordinator_fixture_payload(path);
        std::string error;
        assert(!coordinator.maintain(0, &error));
        assert(!error.empty());
        assert(coordinator_fixture_payload(path) == intent);
        assert(coordinator.next_sequence() == 1 && coordinator.outbox_size() == 0);
        assert(!coordinator.next_batch_to_send());
    }
    assert(coordinator.maintain());
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1);
    assert(!coordinator.next_batch_to_send());
    coordinator.close();
    now += 1000;
    Coordinator restarted([&] { return now; });
    assert(restarted.open(path));
    assert(restarted.on_server_enable(enable));
    const auto batch = restarted.next_batch_to_send();
    assert(batch && batch->producer_id == producer && batch->sequence == 1);
    assert(batch->samples.front().collector_id == 91);
    assert(restarted.outbox_size() == 1);
}

void test_service_owned_intent_age_expires_future_wire() {
    const auto path = temp_outbox_path("intent_age");
    auto now = test_now_ms();
    nstu::exam::TelemetryBatch pending;
    {
        Coordinator coordinator([&] { return now; });
        assert(coordinator.open(path));
        const auto enable = make_enable();
        assert(coordinator.on_server_enable(enable));
        pending = {enable.package_id, enable.package_digest, enable.client_id, enable.session_id,
                   enable.candidate_id, coordinator.active_producer_id(), 1, now, {make_sample(72)}};
        LockedFile locked(path);
        assert(!coordinator.ingest_samples(pending.samples));
        coordinator.close();
    }
    const auto original = nstu::exam::encode_telemetry_batch(pending);
    auto state = coordinator_fixture_payload(path);
    assert(std::equal(original.begin(), original.end(), state.end() - original.size()));
    pending.first_enqueued_at_unix_milliseconds += 90ULL * 86400ULL * 1000ULL;
    const auto future = nstu::exam::encode_telemetry_batch(pending);
    assert(future.size() == original.size());
    std::copy(future.begin(), future.end(), state.end() - future.size());
    save_fixture(path.wstring() + L".coord_state", state, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    now += nstu::exam::kTelemetryRetentionMilliseconds;
    Coordinator expired([&] { return now; });
    assert(!expired.open(path));
    assert(expired.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    Coordinator restarted([&] { return now; });
    assert(restarted.open(path));
    assert(!restarted.on_server_enable(make_enable()));
    assert(restarted.dropped_sample_count() == 1 && restarted.dropped_batch_count() == 1);
    assert(restarted.outbox_size() == 0);
}

void test_v2_migration_preserves_state_and_refuses_unaged_intent() {
    const auto path = temp_outbox_path("v2_migration");
    nstu::exam::ProducerId producer;
    {
        Coordinator coordinator;
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(make_enable()));
        producer = coordinator.active_producer_id();
        coordinator.close();
    }
    auto state = coordinator_fixture_payload(path);
    state.erase(state.end() - 12, state.end() - 4); // v2 had no intent-age field
    put_number(state, 4, 2, 2);
    save_fixture(path.wstring() + L".coord_state", state, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    Coordinator migrated;
    assert(migrated.open(path));
    assert(migrated.active_producer_id() == producer && migrated.next_sequence() == 1);
    assert(migrated.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(coordinator_fixture_payload(path)[4] == std::byte{3});

    const auto pending_path = temp_outbox_path("v2_pending_refused");
    const auto pending = make_pending_fixture(pending_path);
    const auto bytes = nstu::exam::encode_telemetry_batch(pending);
    state = coordinator_fixture_payload(pending_path);
    const auto age_offset = state.size() - bytes.size() - 4 - 8;
    state.erase(state.begin() + static_cast<std::ptrdiff_t>(age_offset),
                state.begin() + static_cast<std::ptrdiff_t>(age_offset + 8));
    put_number(state, 4, 2, 2);
    save_fixture(pending_path.wstring() + L".coord_state", state, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    Coordinator refused;
    std::string error;
    assert(!refused.open(pending_path, &error));
    assert(!error.empty());
    assert(coordinator_fixture_payload(pending_path) == state);
    assert(!refused.on_server_enable(make_enable()));
}

void test_missing_pending_storage_and_expired_file_fail_closed() {
    const auto missing = temp_outbox_path("pending_missing_storage");
    (void)make_pending_fixture(missing);
    const auto intent = coordinator_fixture_payload(missing);
    assert(std::filesystem::remove(missing));
    Coordinator refused;
    assert(!refused.open(missing));
    assert(coordinator_fixture_payload(missing) == intent);
    assert(!std::filesystem::exists(missing));

    const auto aged = temp_outbox_path("pending_aged_file");
    (void)make_pending_fixture(aged);
    std::filesystem::last_write_time(aged, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 31));
    Coordinator expired;
    assert(!expired.open(aged));
    Coordinator restarted;
    assert(restarted.open(aged));
    assert(!restarted.on_server_enable(make_enable()));
    assert(restarted.outbox_size() == 0);
}

void test_maintenance_expiry_write_failure_blocks_offer_until_retry() {
    const auto path = temp_outbox_path("maintenance_expiry_failure");
    auto now = test_now_ms();
    Coordinator coordinator([&] { return now; });
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
    const std::vector samples{make_sample(51)};
    assert(coordinator.ingest_samples(samples));
    coordinator.on_disconnect();
    now += nstu::exam::kTelemetryRetentionMilliseconds;
    {
        LockedFile locked(path);
        std::string error;
        assert(!coordinator.maintain(0, &error));
        assert(!error.empty());
        assert(!coordinator.next_batch_to_send());
        assert(coordinator.outbox_size() == 1);
        assert(coordinator.aggregate_loss().expired_batches == 0);
    }
    assert(coordinator.maintain());
    assert(coordinator.outbox_size() == 0);
    assert(coordinator.aggregate_loss().expired_batches == 1);
}

void test_expired_intent_state_failure_blocks_fresh_restart_after_rollback() {
    const auto path = temp_outbox_path("expired_intent_marker");
    auto now = test_now_ms();
    auto coordinator = std::make_unique<Coordinator>([&] { return now; });
    assert(coordinator->open(path));
    const auto enable = make_enable();
    assert(coordinator->on_server_enable(enable));
    const auto producer = coordinator->active_producer_id();
    const std::vector samples{make_sample(61)};
    {
        LockedFile locked(path);
        assert(!coordinator->ingest_samples(samples));
    }
    const auto intent = coordinator_fixture_payload(path);
    {
        LockedFile locked(path.wstring() + L".coord_state");
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        std::string error;
        assert(!coordinator->maintain(0, &error));
        assert(!error.empty());
        assert(coordinator->lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(coordinator->active_producer_id() == producer);
        coordinator.reset(); // Restart loses all volatile revocation state.
    }
    --now;
    Coordinator restarted([&] { return now; });
    assert(!restarted.open(path));
    assert(std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
    assert(coordinator_fixture_payload(path) == intent);
    assert(!restarted.on_server_enable(enable));
    assert(!restarted.next_batch_to_send());
}

void test_expired_intent_marker_cleanup_keeps_identity_revoked() {
    const auto path = temp_outbox_path("expired_marker_retry");
    auto now = test_now_ms();
    Coordinator coordinator([&] { return now; });
    assert(coordinator.open(path));
    const auto enable = make_enable();
    assert(coordinator.on_server_enable(enable));
    const auto producer = coordinator.active_producer_id();
    const std::vector samples{make_sample(62)};
    {
        LockedFile locked(path);
        assert(!coordinator.ingest_samples(samples));
    }
    {
        LockedFile locked(path.wstring() + L".coord_state");
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        assert(!coordinator.maintain());
        assert(std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
        assert(coordinator.active_producer_id() == producer);
        assert(coordinator.next_sequence() == 1);
        assert(coordinator.outbox_size() == 0);
    }
    --now;
    assert(coordinator.maintain());
    assert(!std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
    assert(coordinator.dropped_sample_count() == 1 && coordinator.dropped_batch_count() == 1);
    assert(!coordinator.on_server_enable(enable));
    const auto different = make_enable("expired-marker-next-context");
    assert(coordinator.on_server_enable(different));
    coordinator.on_service_shutdown();
    coordinator.close();
    Coordinator restarted([&] { return now; });
    assert(restarted.open(path));
    assert(!restarted.on_server_enable(enable));
    assert(restarted.on_server_enable(different));
    assert(restarted.outbox_size() == 0);
}

void test_pending_expiry_is_observed_before_a_dirty_state_write() {
    const auto path = temp_outbox_path("expired_intent_dirty");
    auto now = test_now_ms();
    Coordinator coordinator([&] { return now; });
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
    const std::vector samples{make_sample(63)};
    {
        LockedFile locked(path);
        assert(!coordinator.ingest_samples(samples));
    }
    {
        LockedFile locked(path.wstring() + L".coord_state");
        coordinator.on_disconnect(); // Leaves a failed suspended-state write dirty.
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        assert(!coordinator.maintain());
        assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
    }
    assert(coordinator.maintain());
    assert(!coordinator.on_server_enable(make_enable()));
}

} // namespace

void test_expired_regular_batch_cannot_return_after_fresh_restart() {
    const auto path = temp_outbox_path("review_expiry_restart");
    auto now = test_now_ms();
    const auto enable = make_enable();
    auto coordinator = std::make_unique<Coordinator>([&] { return now; });
    assert(coordinator->open(path));
    assert(coordinator->on_server_enable(enable));
    const std::vector samples{make_sample(91)};
    assert(coordinator->ingest_samples(samples));
    coordinator->on_disconnect();
    {
        LockedFile locked(path);
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        assert(!coordinator->maintain());
        assert(!coordinator->next_batch_to_send());
        coordinator.reset();
    }
    --now;
    Coordinator restarted([&] { return now; });
    assert(restarted.open(path));
    assert(restarted.on_server_enable(enable));
    assert(!restarted.next_batch_to_send());
    assert(restarted.outbox_size() == 0);
}

void test_coordinator_crash_temporary_payloads_are_cleaned() {
    const auto path = temp_outbox_path("review_coord_temps");
    Coordinator coordinator;
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
    { LockedFile locked(path); assert(!coordinator.ingest_samples(std::vector{make_sample(92)})); }
    const auto state = std::filesystem::path(path.wstring() + L".coord_state");
    const auto crash = std::filesystem::path(state.wstring() + L".tmp-123");
    const auto other = std::filesystem::path(state.wstring() + L".tmp-not-owned");
    assert(std::filesystem::copy_file(state, crash, std::filesystem::copy_options::overwrite_existing));
    assert(std::filesystem::copy_file(state, other, std::filesystem::copy_options::overwrite_existing));
    assert(coordinator.maintain());
    assert(!std::filesystem::exists(crash));
    assert(std::filesystem::exists(other));
    assert(std::filesystem::remove(other));
    assert(std::filesystem::copy_file(state, crash));
    coordinator.close();
    Coordinator restarted;
    assert(restarted.open(path));
    assert(!std::filesystem::exists(crash));
    assert(restarted.outbox_size() == 1);
}

void test_blocked_restart_maintenance_scrubs_pending_payload() {
    const auto path = temp_outbox_path("review_blocked_scrub");
    auto now = test_now_ms();
    auto coordinator = std::make_unique<Coordinator>([&] { return now; });
    assert(coordinator->open(path));
    const auto enable = make_enable();
    assert(coordinator->on_server_enable(enable));
    { LockedFile locked(path); assert(!coordinator->ingest_samples(std::vector{make_sample(93)})); }
    const auto state = std::filesystem::path(path.wstring() + L".coord_state");
    { LockedFile locked(state); now += nstu::exam::kTelemetryRetentionMilliseconds;
      assert(!coordinator->maintain()); coordinator.reset(); }
    Coordinator restarted([&] { return now; });
    assert(!restarted.open(path));
    assert(!restarted.maintain());
    assert(!std::filesystem::exists(state));
    assert(std::filesystem::exists(state.wstring() + L".blocked"));
    assert(!restarted.on_server_enable(enable));
    assert(!restarted.next_batch_to_send());
}

void test_initial_state_write_failure_does_not_orphan_outbox() {
    const auto path = temp_outbox_path("review_initial_state");
    const auto state = std::filesystem::path(path.wstring() + L".coord_state");
    assert(std::filesystem::create_directory(state));
    Coordinator coordinator;
    assert(!coordinator.open(path));
    assert(!std::filesystem::exists(path));
    assert(std::filesystem::remove(state));
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(make_enable()));
}

void test_blocked_restart_scrubs_both_durable_payload_stores() {
    const auto path = temp_outbox_path("review_blocked_both");
    const auto state = std::filesystem::path(path.wstring() + L".coord_state");
    auto coordinator = std::make_unique<Coordinator>();
    assert(coordinator->open(path));
    const auto enable = make_enable();
    assert(coordinator->on_server_enable(enable));
    assert(coordinator->ingest_samples(std::vector{make_sample(94)}));
    { LockedFile payload_lock(path); LockedFile state_lock(state);
      coordinator->on_explicit_stop(); coordinator.reset(); }
    Coordinator restarted;
    assert(!restarted.open(path));
    assert(!restarted.maintain());
    assert(!std::filesystem::exists(path));
    assert(!std::filesystem::exists(state));
    assert(std::filesystem::exists(state.wstring() + L".blocked"));
    assert(!restarted.on_server_enable(enable));
}

void test_blocked_restart_retries_owned_payload_cleanup() {
    bool all_outbox_temps_scrubbed = true;
    bool outbox_temp_lock_reported = true;
    // Separate real blocked fixtures exercise each deletion lock independently.
    for (std::size_t locked_store = 0; locked_store < 4; ++locked_store) {
        const auto path = temp_outbox_path("storage_recovery_1a_" + std::to_string(locked_store));
        const auto state = std::filesystem::path(path.wstring() + L".coord_state");
        const auto marker = std::filesystem::path(state.wstring() + L".blocked");
        const auto state_temp = std::filesystem::path(state.wstring() + L".tmp-101");
        const auto blocked_temp = std::filesystem::path(state.wstring() + L".blocked.tmp-102");
        const auto outbox_temp = std::filesystem::path(path.wstring() + L".tmp-103");
        const std::vector sentinels{
            std::filesystem::path(state.wstring() + L".tmp-not-owned"),
            std::filesystem::path(path.wstring() + L".tmp-not-owned"),
            std::filesystem::path(path.wstring() + L".unrelated.tmp-104")};
        const auto directory = std::filesystem::path(path.wstring() + L".tmp-105");
        const auto child = directory / "keep.txt";
        const std::vector owned_temps{state_temp, blocked_temp, outbox_temp};
        // Remove only this fixture's known sidecars, including after a failed run.
        std::error_code ec;
        for (const auto& sidecar : owned_temps) { std::filesystem::remove(sidecar, ec); assert(!ec); }
        for (const auto& sentinel : sentinels) { std::filesystem::remove(sentinel, ec); assert(!ec); }
        std::filesystem::remove(child, ec); assert(!ec);
        std::filesystem::remove(directory, ec); assert(!ec);

        const auto now = test_now_ms();
        const auto enable = make_enable();
        auto coordinator = std::make_unique<Coordinator>([now] { return now; });
        assert(coordinator->open(path));
        assert(coordinator->on_server_enable(enable));
        assert(coordinator->ingest_samples(std::vector{make_sample(95)}, now));
        assert(coordinator->outbox_size() == 1);
        const auto durable_payload = nstu::security::load_machine_secret(path.wstring(),
            entropy_bytes("NSTU-Exam-Telemetry-Outbox-v1"));
        assert(durable_payload.size() > 32);
        nstu::exam::TelemetryBatch pending{enable.package_id, enable.package_digest,
            enable.client_id, enable.session_id, enable.candidate_id, coordinator->active_producer_id(),
            2, now, {make_sample(96)}};
        {
            LockedFile locked(path);
            assert(!coordinator->ingest_samples(pending.samples, now));
        }
        assert(coordinator->next_sequence() == 2);
        const auto pending_payload = coordinator_fixture_payload(path);
        const auto pending_bytes = nstu::exam::encode_telemetry_batch(pending);
        assert(!pending_bytes.empty() && pending_payload.size() >= pending_bytes.size());
        assert(std::equal(pending_bytes.rbegin(), pending_bytes.rend(), pending_payload.rbegin()));
        const auto protected_state = nstu::security::load_machine_secret(state.wstring(),
            entropy_bytes("NSTU-Exam-Telemetry-Service-Coordinator-v1"));
        assert(protected_state.size() == pending_payload.size() + 32);
        assert(std::filesystem::copy_file(state, state_temp));
        assert(std::filesystem::copy_file(state, blocked_temp));
        assert(std::filesystem::copy_file(path, outbox_temp));
        for (const auto& copy : std::vector{state_temp, blocked_temp}) {
            assert(nstu::security::load_machine_secret(copy.wstring(),
                entropy_bytes("NSTU-Exam-Telemetry-Service-Coordinator-v1")) == protected_state);
        }
        assert(nstu::security::load_machine_secret(outbox_temp.wstring(),
            entropy_bytes("NSTU-Exam-Telemetry-Outbox-v1")) == durable_payload);
        assert(std::filesystem::copy_file(state, sentinels[0]));
        assert(std::filesystem::copy_file(path, sentinels[1]));
        assert(std::filesystem::copy_file(path, sentinels[2]));
        assert(std::filesystem::create_directory(directory));
        { std::ofstream stream(child); stream << "preserve nested sentinel"; assert(stream.good()); }
        {
            LockedFile payload_lock(path);
            LockedFile state_lock(state);
            coordinator->on_explicit_stop();
            assert(coordinator->lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
            assert(!coordinator->last_error().empty());
            assert(std::filesystem::exists(marker));
            assert(coordinator_fixture_payload(path) == pending_payload);
            assert(nstu::security::load_machine_secret(path.wstring(),
                entropy_bytes("NSTU-Exam-Telemetry-Outbox-v1")) == durable_payload);
            coordinator.reset();
        }

        Coordinator restarted([now] { return now; });
        const auto assert_refused = [&] {
            assert(!restarted.is_open());
            assert(!restarted.on_server_enable(enable));
            assert(!restarted.on_server_enable(make_enable("storage-recovery-new", "cand-new")));
            assert(!restarted.ingest_samples(std::vector{make_sample(97)}, now));
            assert(!restarted.next_batch_to_send(now));
            assert(!restarted.is_open());
            assert(std::filesystem::exists(marker));
        };
        const std::vector lock_targets{state_temp, outbox_temp, path, state};
        std::string error;
        {
            LockedFile locked(lock_targets[locked_store]);
            assert(!restarted.open(path, &error));
            assert(!error.empty());
            assert_refused();
            error.clear();
            assert(!restarted.maintain(now, &error));
            assert(!error.empty());
            assert(std::filesystem::exists(lock_targets[locked_store]));
            if (locked_store == 1) {
                outbox_temp_lock_reported = error.find("outbox temporary cleanup failed") != std::string::npos &&
                    std::filesystem::exists(path) && std::filesystem::exists(state);
                std::cout << "Outbox temporary deletion lock: " << error << std::endl;
            }
            assert_refused();
        }
        error.clear();
        assert(!restarted.maintain(now, &error));
        assert(!error.empty());
        assert_refused();
        assert(!std::filesystem::exists(path));
        assert(!std::filesystem::exists(state));
        assert(!std::filesystem::exists(state_temp));
        assert(!std::filesystem::exists(blocked_temp));
        const bool outbox_temp_scrubbed = !std::filesystem::exists(outbox_temp);
        all_outbox_temps_scrubbed = all_outbox_temps_scrubbed && outbox_temp_scrubbed;
        std::cout << "Blocked cleanup lock row " << locked_store
                  << ": refusal, locked target and final/coordinator cleanup passed; outbox temp retry "
                  << (outbox_temp_scrubbed ? "passed" : "FAILED") << std::endl;
        for (const auto& sentinel : sentinels) assert(std::filesystem::is_regular_file(sentinel));
        assert(std::filesystem::exists(child));
        assert(!restarted.open(path));
        assert_refused();
        // Preserve the refusal marker through every retry; then tear down this fixture only.
        for (const auto& sidecar : owned_temps) { std::filesystem::remove(sidecar, ec); assert(!ec); }
        for (const auto& sentinel : sentinels) assert(std::filesystem::remove(sentinel));
        assert(std::filesystem::remove(child));
        assert(std::filesystem::remove(directory));
        assert(std::filesystem::remove(marker));
    }
    assert(all_outbox_temps_scrubbed);
    assert(outbox_temp_lock_reported);
}

namespace {

void assert_pristine_regular_file(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    assert(attributes != INVALID_FILE_ATTRIBUTES);
    assert((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0);
}

std::vector<std::byte> pristine_ciphertext(const std::filesystem::path& path) {
    assert_pristine_regular_file(path);
    const auto size = std::filesystem::file_size(path);
    assert(size > 0 && size <= 4096);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    assert(stream.is_open());
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(stream.good());
    return bytes;
}

std::vector<std::byte> pristine_checked_payload(
    const std::filesystem::path& path, std::string_view entropy, std::size_t payload_size) {
    (void)pristine_ciphertext(path);
    auto plaintext = nstu::security::load_machine_secret(path.wstring(), entropy_bytes(entropy));
    assert(plaintext.size() == payload_size + 32);
    const auto digest = nstu::security::sha256(std::span<const std::byte>(plaintext).first(payload_size));
    assert(digest);
    assert(std::equal(digest->begin(), digest->end(),
                      plaintext.begin() + static_cast<std::ptrdiff_t>(payload_size)));
    plaintext.resize(payload_size);
    return plaintext;
}

void assert_pristine_payload_fields(std::span<const std::byte> payload, bool coordinator) {
    std::size_t offset = 0;
    const auto number = [&](std::size_t width) {
        assert(width <= 8 && offset <= payload.size() && width <= payload.size() - offset);
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value |= std::to_integer<std::uint64_t>(payload[offset + index]) << (index * 8);
        }
        offset += width;
        return value;
    };
    const auto zeros = [&](std::size_t width) {
        assert(offset <= payload.size() && width <= payload.size() - offset);
        for (std::size_t index = 0; index < width; ++index) {
            assert(payload[offset + index] == std::byte{});
        }
        offset += width;
    };
    assert(number(4) == (coordinator ? 0x544c4d53ULL : 0x544c4d4fULL));
    assert(number(2) == (coordinator ? 3ULL : 2ULL));
    assert(number(2) == 0);
    if (coordinator) {
        assert(number(4) == 0); // empty package ID
        zeros(nstu::security::Sha256Digest{}.size());
        zeros(nstu::security::ClientId{}.size());
        zeros(nstu::exam::SessionId{}.size());
        assert(number(4) == 0); // empty candidate ID
        zeros(nstu::exam::ProducerId{}.size());
        assert(number(8) == 1);
        assert(number(4) == 0);
        assert(number(1) == static_cast<std::uint8_t>(nstu::client::TelemetryLifecycleState::disabled));
        for (unsigned counter = 0; counter < 4; ++counter) assert(number(8) == 0);
        assert(number(4) == 0); // revoked identities
        assert(number(8) == 0); // pending timestamp
        assert(number(4) == 0); // pending bytes
    } else {
        zeros(nstu::exam::SessionId{}.size());
        assert(number(4) == 0); // aggregate losses
        assert(number(4) == 0); // batches
    }
    assert(offset == payload.size());
}

std::vector<std::byte> pristine_coordinator_payload(const std::filesystem::path& state) {
    auto payload = pristine_checked_payload(state, "NSTU-Exam-Telemetry-Service-Coordinator-v1", 157);
    assert_pristine_payload_fields(payload, true);
    return payload;
}

void assert_pristine_outbox(const std::filesystem::path& path) {
    const auto payload = pristine_checked_payload(path, "NSTU-Exam-Telemetry-Outbox-v1", 32);
    assert_pristine_payload_fields(payload, false);
}

void assert_pristine_markers_absent(const std::filesystem::path& path) {
    assert(!std::filesystem::exists(path.wstring() + L".coord_state.blocked"));
    assert(!std::filesystem::exists(path.wstring() + L".revoked"));
    assert(!std::filesystem::exists(path.wstring() + L".expiry"));
}

void assert_pristine_public_state(Coordinator& coordinator, std::uint64_t now) {
    assert(coordinator.is_open());
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::disabled);
    assert(coordinator.context() == nstu::client::TelemetryContextIdentity{});
    assert(coordinator.active_producer_id() == nstu::exam::ProducerId{});
    assert(coordinator.generation_count() == 0 && coordinator.next_sequence() == 1);
    assert(coordinator.outbox_size() == 0);
    assert(coordinator.dropped_sample_count() == 0 && coordinator.dropped_batch_count() == 0);
    assert(!coordinator.next_batch_to_send(now));
}

void run_pristine_interrupted_initialization(const std::string& tag, bool numeric_temporary) {
    const auto path = temp_outbox_path(tag); // This is the only reset/path-helper call.
    const std::filesystem::path state = path.wstring() + L".coord_state";
    const std::filesystem::path temporary = state.wstring() + L".tmp-123";
    std::error_code ec;
    (void)std::filesystem::remove(path.wstring() + L".expiry", ec);
    assert(!ec);
    const auto now = test_now_ms();
    const auto clock = [now] { return now; };
    std::string error;
    {
        Coordinator seed(clock);
        assert(seed.open(path, &error));
        assert_pristine_public_state(seed, now);
        assert(seed.not_authorized_sample_count() == 0);
        seed.close();
        assert(!seed.is_open());
    }
    const auto original_ciphertext = pristine_ciphertext(state);
    const auto original_payload = pristine_coordinator_payload(state);
    assert_pristine_outbox(path);
    assert_pristine_markers_absent(path);
    if (numeric_temporary) {
        std::filesystem::rename(state, temporary, ec);
        assert(!ec);
        assert(!std::filesystem::exists(state));
        assert_pristine_regular_file(temporary);
        assert(pristine_ciphertext(temporary) == original_ciphertext);
        assert(pristine_coordinator_payload(temporary) == original_payload);
    }
    assert(std::filesystem::remove(path, ec));
    assert(!ec && !std::filesystem::exists(path));
    assert_pristine_markers_absent(path);
    if (!numeric_temporary) {
        assert(pristine_ciphertext(state) == original_ciphertext);
        assert(pristine_coordinator_payload(state) == original_payload);
    }
    {
        Coordinator recovered(clock);
        assert(recovered.open(path, &error));
        assert(!std::filesystem::exists(temporary));
        assert_pristine_public_state(recovered, now);
        assert(recovered.not_authorized_sample_count() == 0);
        assert_pristine_outbox(path);
        assert(pristine_coordinator_payload(state) == original_payload);
        if (!numeric_temporary) assert(pristine_ciphertext(state) == original_ciphertext);
        assert(recovered.maintain(now, &error));
        assert_pristine_public_state(recovered, now);
        assert(pristine_coordinator_payload(state) == original_payload);
        if (!numeric_temporary) assert(pristine_ciphertext(state) == original_ciphertext);
        assert_pristine_markers_absent(path);
        recovered.close();
        assert(!recovered.is_open());
    }
    Coordinator reopened(clock);
    assert(reopened.open(path, &error));
    assert_pristine_public_state(reopened, now);
    assert(pristine_coordinator_payload(state) == original_payload);
    if (!numeric_temporary) assert(pristine_ciphertext(state) == original_ciphertext);
    assert_pristine_outbox(path);
    const std::vector<nstu::exam::TelemetrySample> before_enable{make_sample(91)};
    assert(!reopened.ingest_samples(before_enable, now, &error));
    assert_pristine_public_state(reopened, now);
    assert(reopened.not_authorized_sample_count() == 1);
    assert_pristine_outbox(path);
    auto enable = make_enable();
    enable.server_time_unix_milliseconds = now;
    assert(reopened.on_server_enable(enable, &error));
    assert(reopened.context().matches(enable));
    assert(reopened.lifecycle_state() == nstu::client::TelemetryLifecycleState::enabled);
    const auto producer = reopened.active_producer_id();
    assert(producer != nstu::exam::ProducerId{});
    assert(reopened.generation_count() == 1 && reopened.next_sequence() == 1);
    assert(reopened.outbox_size() == 0 && !reopened.next_batch_to_send(now));
    const auto sample = make_sample(92);
    const std::vector<nstu::exam::TelemetrySample> enabled_samples{sample};
    assert(reopened.ingest_samples(enabled_samples, now, &error));
    const auto first = reopened.next_batch_to_send(now);
    assert(first && first->sequence == 1 && first->producer_id == producer);
    assert(first->first_enqueued_at_unix_milliseconds == now);
    assert(reopened.context().matches(*first));
    assert(first->samples.size() == 1 && reopened.outbox_size() == 1);
    auto expected = *first;
    expected.samples = enabled_samples;
    const auto expected_bytes = nstu::exam::encode_telemetry_batch(expected);
    assert(!expected_bytes.empty());
    assert(nstu::exam::encode_telemetry_batch(*first) == expected_bytes);
    assert(reopened.next_sequence() == 2);
    assert(reopened.on_server_enable(enable, &error));
    assert(reopened.active_producer_id() == producer);
    assert(reopened.generation_count() == 1 && reopened.next_sequence() == 2);
    assert(reopened.outbox_size() == 1);
    assert_pristine_markers_absent(path);
    reopened.close();
    assert(!reopened.is_open());
}

void test_pristine_final_state_without_outbox() {
    run_pristine_interrupted_initialization("pristine_final_only", false);
    std::cout << "Pristine final-state/missing-outbox recovery and first enabled sequence one passed.\n";
}

void test_pristine_numeric_state_temporary_without_final_stores() {
    run_pristine_interrupted_initialization("pristine_numeric_only", true);
    std::cout << "Pristine numeric-state-temp/no-final-stores recovery and first enabled sequence one passed.\n";
}

} // namespace

namespace {

class StopReadSharingLock {
public:
    explicit StopReadSharingLock(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        assert(handle_ != INVALID_HANDLE_VALUE);
    }
    ~StopReadSharingLock() { assert(CloseHandle(handle_)); }
    StopReadSharingLock(const StopReadSharingLock&) = delete;
    StopReadSharingLock& operator=(const StopReadSharingLock&) = delete;
private:
    HANDLE handle_;
};

DWORD stop_native_create_probe(const std::filesystem::path& path, bool should_succeed) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    const DWORD creation_error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (handle != INVALID_HANDLE_VALUE) {
        const std::byte value{1};
        DWORD written = 0;
        assert(WriteFile(handle, &value, 1, &written, nullptr));
        assert(written == 1);
        assert(FlushFileBuffers(handle));
        assert(CloseHandle(handle));
    }
    assert((handle != INVALID_HANDLE_VALUE) == should_succeed);
    return creation_error;
}

void stop_remove_owned_file(const std::filesystem::path& path) {
    std::error_code error;
    (void)std::filesystem::remove(path, error);
    assert(!error);
}

void test_stop_survives_state_and_marker_temp_failures() {
    const auto plain_directory = std::filesystem::absolute(
        std::filesystem::temp_directory_path() / L"nstu_stop_durability_tests" /
        (L"case-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))).lexically_normal();
    assert(plain_directory.has_root_name() && plain_directory.has_root_directory());
    assert(plain_directory.native().size() >= 3 && plain_directory.native()[1] == L':');
    assert(std::filesystem::create_directories(plain_directory));
    const std::filesystem::path directory(std::wstring(LR"(\\?\)") + plain_directory.native());
    std::array<wchar_t, MAX_PATH> volume{};
    assert(GetVolumePathNameW(plain_directory.c_str(), volume.data(), static_cast<DWORD>(volume.size())));
    DWORD maximum_component = 0;
    assert(GetVolumeInformationW(volume.data(), nullptr, 0, nullptr,
                                 &maximum_component, nullptr, nullptr, 0));
    assert(maximum_component >= 64 && maximum_component <= 512);
    const auto tick_digits = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count()).size();
    const std::wstring state_suffix = L".coord_state";
    const std::wstring marker_suffix = L".blocked";
    const std::size_t temporary_suffix_size = 5 + tick_digits;
    const auto base_size = static_cast<std::size_t>(maximum_component) -
                           state_suffix.size() - temporary_suffix_size - 4;
    std::wstring base_name = L"coordinator_stop_double_failure_";
    assert(base_name.size() < base_size);
    base_name.resize(base_size, L'a');
    const auto path = directory / base_name;
    const std::filesystem::path state = path.wstring() + state_suffix;
    const std::filesystem::path marker = state.wstring() + marker_suffix;
    const auto within_probe = directory / std::wstring(maximum_component - 4, L'p');
    const auto above_probe = directory / std::wstring(maximum_component + 4, L'q');
    const auto final_marker_probe = directory / std::wstring(marker.filename().native().size(), L'm');
    const auto held_probe = directory / L"held-state-probe";
    assert(stop_native_create_probe(within_probe, true) == ERROR_SUCCESS);
    const auto component_error = stop_native_create_probe(above_probe, false);
    assert(component_error == ERROR_FILENAME_EXCED_RANGE || component_error == ERROR_INVALID_NAME);
    assert(stop_native_create_probe(final_marker_probe, true) == ERROR_SUCCESS);
    assert(stop_native_create_probe(held_probe, true) == ERROR_SUCCESS);
    DWORD replacement_error = ERROR_SUCCESS;
    {
        StopReadSharingLock held(held_probe);
        const BOOL replaced = MoveFileExW(within_probe.c_str(), held_probe.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        replacement_error = replaced ? ERROR_SUCCESS : GetLastError();
        assert(!replaced);
        assert(replacement_error == ERROR_SHARING_VIOLATION || replacement_error == ERROR_ACCESS_DENIED);
    }
    const BOOL unlocked_replacement = MoveFileExW(within_probe.c_str(), held_probe.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const DWORD unlocked_error = unlocked_replacement ? ERROR_SUCCESS : GetLastError();
    std::cout << "STOP-DURABILITY locked replacement-error=" << replacement_error
              << " unlocked-replaced=" << unlocked_replacement << " unlocked-error=" << unlocked_error << std::endl;
    assert(unlocked_replacement && unlocked_error == ERROR_SUCCESS);
    stop_remove_owned_file(within_probe);
    stop_remove_owned_file(final_marker_probe);
    stop_remove_owned_file(held_probe);
    const auto now = test_now_ms();
    const auto clock = [now] { return now; };
    auto enable = make_enable("stop-durable-pkg", "stop-durable-candidate");
    enable.server_time_unix_milliseconds = now;
    auto coordinator = std::make_unique<Coordinator>(clock);
    std::string error;
    assert(coordinator->open(path, &error));
    assert(coordinator->on_server_enable(enable, &error));
    assert(coordinator->ingest_samples(std::vector{make_sample(151)}, now, &error));
    assert(coordinator->outbox_size() == 1 && coordinator->next_sequence() == 2);
    const auto producer = coordinator->active_producer_id();
    const auto original_state = pristine_ciphertext(state);
    const auto original_payload = pristine_checked_payload(state,
        "NSTU-Exam-Telemetry-Service-Coordinator-v1",
        157 + enable.package_id.size() + enable.candidate_id.size());
    assert(original_payload.size() >= 12);
    assert(std::all_of(original_payload.end() - 12, original_payload.end(),
                       [](std::byte value) { return value == std::byte{}; })); // no pending intent
    assert_pristine_markers_absent(path);
    const auto digits_before_stop = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count()).size();
    std::string stop_error;
    {
        StopReadSharingLock held(state);
        coordinator->on_explicit_stop();
        assert(coordinator->lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(coordinator->outbox_size() == 0);
        stop_error = coordinator->last_error();
        assert(stop_error.find("revocation state") != std::string::npos);
        assert(pristine_ciphertext(state) == original_state);
        coordinator.reset(); // destroy without a successful retry while replacement remains blocked
        assert(pristine_ciphertext(state) == original_state);
    }
    const auto digits_after_stop = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count()).size();
    for (const auto digits : {digits_before_stop, digits_after_stop}) {
        assert(base_size + state_suffix.size() + 5 + digits <= maximum_component);
        assert(base_size + state_suffix.size() + marker_suffix.size() + 5 + digits > maximum_component);
        assert(base_size + 5 + digits <= maximum_component);
    }
    const bool refusal_marker = std::filesystem::exists(marker);
    const bool outbox_marker = std::filesystem::exists(path.wstring() + L".revoked");
    assert(!outbox_marker);
    assert_pristine_regular_file(path);
    {
        nstu::exam::TelemetryOutbox inspect;
        assert(inspect.open(path, now, &error));
        assert(inspect.size() == 0 && inspect.pending_batches().empty());
        inspect.close();
    }
    error.clear();
    Coordinator restarted(clock);
    const bool reopened = restarted.open(path, &error);
    const auto reopen_error = error;
    error.clear();
    const bool reenabled = reopened && restarted.on_server_enable(enable, &error);
    const auto recovered_state = restarted.lifecycle_state();
    const auto recovered_producer = restarted.active_producer_id();
    std::cout << "STOP-DURABILITY component=" << maximum_component
              << " tick-digits=" << tick_digits << " base-component=" << base_size
              << " creation-error=" << component_error << " replace-error=" << replacement_error
              << " marker=" << refusal_marker << " reopened=" << reopened
              << " old-enable=" << reenabled << " state=" << static_cast<unsigned>(recovered_state)
              << " same-producer=" << (recovered_producer == producer)
              << " stop-error=" << stop_error << " reopen-error=" << reopen_error
              << " enable-error=" << error << std::endl;
    if (reopened) restarted.close();
    stop_remove_owned_file(path);
    stop_remove_owned_file(state);
    stop_remove_owned_file(marker);
    stop_remove_owned_file(path.wstring() + L".revoked");
    stop_remove_owned_file(path.wstring() + L".expiry");
    std::error_code cleanup_error;
    assert(std::filesystem::remove(directory, cleanup_error));
    assert(!cleanup_error);
    if (!reopened) assert(refusal_marker && reopen_error.find("revocation") != std::string::npos);
    if (reopened && !reenabled) assert(recovered_state == nstu::client::TelemetryLifecycleState::revoked);
    assert(!reenabled); // explicit stop must survive the qualified double persistence failure
}

} // namespace

namespace {

void test_stop_repair_preserves_active_empty_recovery() {
    const auto path = temp_outbox_path("stop_repair_active_empty");
    const auto now = test_now_ms();
    const auto clock = [now] { return now; };
    auto enable = make_enable("active-empty-pkg", "active-empty-candidate");
    enable.server_time_unix_milliseconds = now;
    nstu::exam::ProducerId producer{};
    {
        Coordinator seed(clock);
        assert(seed.open(path));
        assert(seed.on_server_enable(enable));
        producer = seed.active_producer_id();
        assert(producer != nstu::exam::ProducerId{});
        assert(seed.outbox_size() == 0 && seed.next_sequence() == 1);
        seed.close();
        assert(!seed.is_open());
    }
    Coordinator recovered(clock);
    assert(recovered.open(path));
    assert(recovered.lifecycle_state() == nstu::client::TelemetryLifecycleState::suspended);
    assert(!recovered.next_batch_to_send(now));
    assert(recovered.on_server_enable(enable));
    assert(recovered.active_producer_id() == producer && recovered.generation_count() == 1);
    assert(recovered.next_sequence() == 1 && recovered.outbox_size() == 0);
    assert_pristine_markers_absent(path);
    assert(recovered.ingest_samples(std::vector{make_sample(152)}, now));
    const auto first = recovered.next_batch_to_send(now);
    assert(first && first->sequence == 1 && first->producer_id == producer);
    recovered.close();
    assert(!recovered.is_open());
}

void test_stop_repair_keeps_existing_refusal_markers() {
    for (const bool empty : {false, true}) {
        const auto path = temp_outbox_path(empty ? "stop_marker_empty" : "stop_marker_existing");
        const std::filesystem::path state = path.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto now = test_now_ms();
        const auto clock = [now] { return now; };
        auto seed = std::make_unique<Coordinator>(clock);
        const auto enable = make_enable();
        assert(seed->open(path));
        assert(seed->on_server_enable(enable));
        const HANDLE native_marker = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        assert(native_marker != INVALID_HANDLE_VALUE);
        const std::array<std::byte, 4> sentinel{std::byte{0x51}, std::byte{0x72}, std::byte{0x93}, std::byte{0xb4}};
        if (!empty) {
            DWORD written = 0;
            assert(WriteFile(native_marker, sentinel.data(), static_cast<DWORD>(sentinel.size()), &written, nullptr));
            assert(written == sentinel.size());
        }
        assert(FlushFileBuffers(native_marker));
        assert(CloseHandle(native_marker));
        const auto original_size = std::filesystem::file_size(marker);
        std::vector<std::byte> original;
        if (!empty) original = pristine_ciphertext(marker);
        {
            StopReadSharingLock held(state);
            seed->on_explicit_stop();
            assert(seed->lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
            assert(std::filesystem::file_size(marker) == original_size);
            if (!empty) assert(pristine_ciphertext(marker) == original);
            seed.reset();
        }
        Coordinator recovered(clock);
        std::string error;
        assert(!recovered.open(path, &error));
        assert(error.find("revocation") != std::string::npos);
        assert(!recovered.on_server_enable(enable));
        assert(std::filesystem::file_size(marker) == original_size);
        if (!empty) assert(pristine_ciphertext(marker) == original);
        std::cout << "STOP-DURABILITY existing-marker empty=" << empty << " preserved and refused.\n";
        stop_remove_owned_file(path);
        stop_remove_owned_file(state);
        stop_remove_owned_file(marker);
    }
}

} // namespace


namespace {

using StopOutcome = nstu::client::TelemetryStopOutcome;
using StopApplicability = nstu::client::TelemetryStopApplicability;
using StopRevocation = nstu::client::TelemetryStopMemoryRevocation;
using StopWitness = nstu::client::TelemetryStopDurableWitness;
using StopCleanup = nstu::client::TelemetryStopCleanup;
using StopError = nstu::client::TelemetryStopError;
static_assert(noexcept(std::declval<Coordinator&>().on_explicit_stop_with_outcome()));
static_assert(noexcept(std::declval<Coordinator&>().on_explicit_stop()));
static_assert(std::is_same_v<decltype(StopOutcome{}.applicability), StopApplicability>);
static_assert(std::is_same_v<decltype(StopOutcome{}.memory_revocation), StopRevocation>);
static_assert(std::is_same_v<decltype(StopOutcome{}.durable_witness), StopWitness>);
static_assert(std::is_same_v<decltype(StopOutcome{}.cleanup), StopCleanup>);
static_assert(std::is_same_v<decltype(StopOutcome{}.error), StopError>);

void assert_stop_outcome(const StopOutcome& result, StopApplicability applicability,
                         StopRevocation memory, StopWitness witness,
                         StopCleanup cleanup, StopError error) {
    assert(result.applicability == applicability);
    assert(result.memory_revocation == memory);
    assert(result.durable_witness == witness);
    assert(result.cleanup == cleanup);
    assert(result.error == error);
}

void assert_stop_revoked_fixture(const std::filesystem::path& path,
                                 const nstu::exam::TelemetryEnable& enable) {
    const auto payload = coordinator_fixture_payload(path);
    const auto offset = sequence_fixture_offset(enable);
    assert(offset + 13 <= payload.size());
    assert(payload[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::revoked));
}

void assert_stop_restart_refuses_old(const std::filesystem::path& path,
                                    const nstu::exam::TelemetryEnable& enable,
                                    std::uint64_t now) {
    Coordinator restarted([now] { return now; });
    assert(restarted.open(path));
    assert(restarted.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(!restarted.on_server_enable(enable));
    assert(!restarted.next_batch_to_send(now));
    assert(restarted.outbox_size() == 0);
}

void test_stop_outcomes_normal_repeated_and_nonapplicable() {
    const auto now = test_now_ms();
    Coordinator closed([now] { return now; });
    assert_stop_outcome(closed.on_explicit_stop_with_outcome(), StopApplicability::closed,
        StopRevocation::not_applicable, StopWitness::not_attempted, StopCleanup::not_needed, StopError::none);
    const auto path = temp_outbox_path("stop_outcomes_normal");
    assert(closed.open(path));
    const auto unbound_state = pristine_ciphertext(path.wstring() + L".coord_state");
    const auto unbound_payload = pristine_ciphertext(path);
    assert_stop_outcome(closed.on_explicit_stop_with_outcome(), StopApplicability::unbound,
        StopRevocation::not_applicable, StopWitness::not_attempted, StopCleanup::not_needed, StopError::none);
    assert(pristine_ciphertext(path.wstring() + L".coord_state") == unbound_state);
    assert(pristine_ciphertext(path) == unbound_payload);
    assert_pristine_markers_absent(path);
    const auto enable = make_enable("stop-outcome-normal");
    assert(closed.on_server_enable(enable));
    assert(closed.ingest_samples(std::vector{make_sample(171)}, now));
    assert_stop_outcome(closed.on_explicit_stop_with_outcome(), StopApplicability::applicable,
        StopRevocation::revoked_now, StopWitness::protected_state_confirmed, StopCleanup::complete, StopError::none);
    assert_stop_revoked_fixture(path, enable);
    assert(closed.outbox_size() == 0 && !closed.next_batch_to_send(now));
    assert_stop_outcome(closed.on_explicit_stop_with_outcome(), StopApplicability::applicable,
        StopRevocation::already_revoked, StopWitness::protected_state_confirmed, StopCleanup::complete, StopError::none);
    closed.on_explicit_stop(); // Additive API retains the existing void entry point.
    assert(!closed.on_server_enable(enable));
    closed.close();
    assert(!closed.is_open());
    assert_stop_outcome(closed.on_explicit_stop_with_outcome(), StopApplicability::closed,
        StopRevocation::not_applicable, StopWitness::not_attempted, StopCleanup::not_needed, StopError::none);
    assert_stop_restart_refuses_old(path, enable, now);
}

void test_stop_outcomes_marker_fallback_and_error_priority() {
    for (const bool block_payload : {false, true}) {
        const auto path = temp_outbox_path(block_payload ? "stop_outcomes_fallback_cleanup" : "stop_outcomes_fallback");
        const std::filesystem::path state = path.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto now = test_now_ms();
        Coordinator coordinator([now] { return now; });
        const auto enable = make_enable();
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        assert(coordinator.ingest_samples(std::vector{make_sample(172)}, now));
        const auto original_state = pristine_ciphertext(state);
        const auto original_payload = pristine_ciphertext(path);
        {
            LockedFile state_lock(state);
            std::unique_ptr<LockedFile> payload_lock;
            if (block_payload) payload_lock = std::make_unique<LockedFile>(path);
            assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
                StopRevocation::revoked_now, StopWitness::refusal_marker_confirmed, StopCleanup::pending,
                StopError::protected_state_failed);
            assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
            assert(pristine_ciphertext(state) == original_state);
            assert_pristine_regular_file(marker);
            assert(pristine_ciphertext(marker) == std::vector<std::byte>{std::byte{1}});
            if (block_payload) assert(pristine_ciphertext(path) == original_payload);
            else {
                nstu::exam::TelemetryOutbox inspect;
                assert(inspect.open(path, now, nullptr));
                assert(inspect.size() == 0);
            }
            Coordinator restarted([now] { return now; });
            assert(!restarted.open(path));
            assert(!restarted.on_server_enable(enable));
            assert(!restarted.next_batch_to_send(now));
        }
        const auto history = coordinator.last_error();
        assert(!history.empty());
        assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
            StopRevocation::already_revoked, StopWitness::protected_state_confirmed, StopCleanup::complete, StopError::none);
        assert(coordinator.last_error() == history); // Current success does not erase historical diagnostics.
        assert(!std::filesystem::exists(marker));
        assert(!coordinator.next_batch_to_send(now));
        coordinator.close();
        assert_stop_restart_refuses_old(path, enable, now);
    }
}

void test_stop_outcomes_existing_markers_are_unconfirmed() {
    for (const bool empty : {false, true}) {
        const auto path = temp_outbox_path(empty ? "stop_outcomes_marker_empty" : "stop_outcomes_marker_bytes");
        const std::filesystem::path state = path.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto now = test_now_ms();
        Coordinator coordinator([now] { return now; });
        const auto enable = make_enable();
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        assert(coordinator.ingest_samples(std::vector{make_sample(173)}, now));
        const HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        assert(file != INVALID_HANDLE_VALUE);
        const std::array<std::byte, 3> sentinel{std::byte{0x24}, std::byte{0x68}, std::byte{0xac}};
        if (!empty) {
            DWORD written = 0;
            assert(WriteFile(file, sentinel.data(), static_cast<DWORD>(sentinel.size()), &written, nullptr));
            assert(written == sentinel.size());
        }
        assert(FlushFileBuffers(file));
        assert(CloseHandle(file));
        const auto original_state = pristine_ciphertext(state);
        const auto marker_size = std::filesystem::file_size(marker);
        std::vector<std::byte> original_marker;
        if (!empty) original_marker = pristine_ciphertext(marker);
        {
            LockedFile state_lock(state);
            LockedFile payload_lock(path); // Unconfirmed witness outranks the simultaneous purge failure.
            assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
                StopRevocation::revoked_now, StopWitness::unconfirmed, StopCleanup::pending,
                StopError::revocation_witness_unconfirmed);
            assert(pristine_ciphertext(state) == original_state);
            assert(std::filesystem::file_size(marker) == marker_size);
            if (!empty) assert(pristine_ciphertext(marker) == original_marker);
            Coordinator restarted([now] { return now; });
            assert(!restarted.open(path));
            assert(!restarted.on_server_enable(enable));
            assert(!restarted.next_batch_to_send(now));
            assert(std::filesystem::file_size(marker) == marker_size);
            if (!empty) assert(pristine_ciphertext(marker) == original_marker);
        }
        assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
            StopRevocation::already_revoked, StopWitness::protected_state_confirmed, StopCleanup::complete, StopError::none);
        assert(!std::filesystem::exists(marker) && !coordinator.next_batch_to_send(now));
        coordinator.close();
        assert_stop_restart_refuses_old(path, enable, now);
    }
}

void test_stop_outcomes_purge_and_marker_removal_failures() {
    for (const bool block_marker : {false, true}) {
        const auto path = temp_outbox_path(block_marker ? "stop_outcomes_marker_cleanup" : "stop_outcomes_purge");
        const std::filesystem::path state = path.wstring() + L".coord_state";
        const std::filesystem::path marker = state.wstring() + L".blocked";
        const auto now = test_now_ms();
        Coordinator coordinator([now] { return now; });
        const auto enable = make_enable();
        assert(coordinator.open(path));
        assert(coordinator.on_server_enable(enable));
        assert(coordinator.ingest_samples(std::vector{make_sample(174)}, now));
        const auto original_payload = pristine_ciphertext(path);
        if (block_marker) assert(stop_native_create_probe(marker, true) == ERROR_SUCCESS);
        const auto locked_path = block_marker ? marker : path;
        {
            LockedFile cleanup_lock(locked_path);
            assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
                StopRevocation::revoked_now, StopWitness::protected_state_confirmed, StopCleanup::pending, StopError::cleanup_failed);
            assert_stop_revoked_fixture(path, enable);
            if (block_marker) {
                assert(pristine_ciphertext(marker) == std::vector<std::byte>{std::byte{1}});
                nstu::exam::TelemetryOutbox inspect;
                assert(inspect.open(path, now, nullptr));
                assert(inspect.size() == 0);
            } else assert(pristine_ciphertext(path) == original_payload);
            assert(!coordinator.next_batch_to_send(now));
            assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
                StopRevocation::already_revoked, StopWitness::protected_state_confirmed, StopCleanup::pending, StopError::cleanup_failed);
        }
        assert(coordinator.maintain(now));
        assert(!coordinator.next_batch_to_send(now) && coordinator.outbox_size() == 0);
        assert(!std::filesystem::exists(marker));
        assert_stop_outcome(coordinator.on_explicit_stop_with_outcome(), StopApplicability::applicable,
            StopRevocation::already_revoked, StopWitness::protected_state_confirmed, StopCleanup::complete, StopError::none);
        coordinator.close();
        assert_stop_restart_refuses_old(path, enable, now);
    }
}

void test_stop_outcomes_second_state_write_failure_retains_first_witness() {
    const auto path = temp_outbox_path("stop_outcomes_second_state");
    const std::filesystem::path state = path.wstring() + L".coord_state";
    const auto now = test_now_ms();
    const auto aged = now + nstu::exam::kTelemetryRetentionMilliseconds;
    bool armed = false;
    unsigned stop_clock_calls = 0;
    std::unique_ptr<LockedFile> state_lock;
    Coordinator coordinator([&] {
        if (!armed) return now;
        ++stop_clock_calls;
        if (stop_clock_calls == 1) return std::uint64_t{0};
        assert(stop_clock_calls == 2);
        state_lock = std::make_unique<LockedFile>(state); // Flush fallback occurs after initial publication.
        return aged;
    });
    const auto enable = make_enable();
    assert(coordinator.open(path));
    assert(coordinator.on_server_enable(enable));
    coordinator.on_disconnect();
    assert(!coordinator.ingest_samples(std::vector{make_sample(175)}, now));
    assert(coordinator.not_authorized_sample_count() == 1);
    assert(coordinator.on_server_enable(enable));
    assert(coordinator.ingest_samples(std::vector{make_sample(176)}, now));
    const auto original_payload = pristine_ciphertext(path);
    const auto before = coordinator_fixture_payload(path);
    const auto offset = sequence_fixture_offset(enable);
    assert(before[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::enabled));
    assert(before[offset + 29] == std::byte{1}); // Persisted unauthorized count before stop.
    armed = true;
    const auto result = coordinator.on_explicit_stop_with_outcome();
    armed = false;
    assert(stop_clock_calls == 2 && state_lock);
    assert_stop_outcome(result, StopApplicability::applicable, StopRevocation::revoked_now,
        StopWitness::protected_state_confirmed, StopCleanup::pending, StopError::protected_state_failed);
    const auto published = coordinator_fixture_payload(path);
    assert(published[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::revoked));
    assert(std::equal(before.begin() + offset + 13, before.begin() + offset + 45,
                      published.begin() + offset + 13)); // Initial publication retains old loss window/counters.
    assert(coordinator.not_authorized_sample_count() == 0);
    assert(coordinator.dropped_sample_count() == 0 && coordinator.dropped_batch_count() == 0);
    assert(pristine_ciphertext(path) == original_payload);
    assert(!std::filesystem::exists(state.wstring() + L".blocked")); // No new fallback after second write failure.
    state_lock.reset();
    assert(coordinator.maintain(aged));
    assert(!coordinator.next_batch_to_send(aged) && coordinator.outbox_size() == 0);
    const auto recovered = coordinator_fixture_payload(path);
    assert(std::all_of(recovered.begin() + offset + 13, recovered.begin() + offset + 45,
                       [](std::byte value) { return value == std::byte{0}; }));
    coordinator.close();
    assert_stop_restart_refuses_old(path, enable, aged);
}

void run_stop_outcome_tests() {
    test_stop_outcomes_normal_repeated_and_nonapplicable();
    test_stop_outcomes_marker_fallback_and_error_priority();
    test_stop_outcomes_existing_markers_are_unconfirmed();
    test_stop_outcomes_purge_and_marker_removal_failures();
    test_stop_outcomes_second_state_write_failure_retains_first_witness();
    std::cout << "Stop outcome native state/marker/cleanup/recovery controls passed.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "stop-outcomes") { run_stop_outcome_tests(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "stop-marker-controls") { test_stop_repair_preserves_active_empty_recovery(); test_stop_repair_keeps_existing_refusal_markers(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "stop-durability") { test_stop_survives_state_and_marker_temp_failures(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "pristine-final") { test_pristine_final_state_without_outbox(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "pristine-temporary") { test_pristine_numeric_state_temporary_without_final_stores(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "storage-recovery-1a") { test_blocked_restart_retries_owned_payload_cleanup(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "review-scrub-both") { test_blocked_restart_scrubs_both_durable_payload_stores(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "review-expiry") { test_expired_regular_batch_cannot_return_after_fresh_restart(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "review-temps") { test_coordinator_crash_temporary_payloads_are_cleaned(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "review-scrub") { test_blocked_restart_maintenance_scrubs_pending_payload(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "review-bootstrap") { test_initial_state_write_failure_does_not_orphan_outbox(); return 0; }
    test_expired_regular_batch_cannot_return_after_fresh_restart();
    test_coordinator_crash_temporary_payloads_are_cleaned();
    test_blocked_restart_maintenance_scrubs_pending_payload();
    test_initial_state_write_failure_does_not_orphan_outbox();
    test_blocked_restart_scrubs_both_durable_payload_stores();
    test_blocked_restart_retries_owned_payload_cleanup();
    std::cout << std::unitbuf;
    std::cout << "Running exam_telemetry_service_tests...\n";
    if (argc == 2 && std::string_view(argv[1]) == "expiry-marker") {
        test_expired_intent_state_failure_blocks_fresh_restart_after_rollback();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "expiry-marker-retry") {
        test_expired_intent_marker_cleanup_keeps_identity_revoked();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "expiry-dirty") {
        test_pending_expiry_is_observed_before_a_dirty_state_write();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "idle-retention") {
        test_idle_provider_expires_suspended_batches_and_disabled_loss();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "wal-age") {
        test_pending_future_wire_cannot_renew_an_expired_arrival();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "maintenance") {
        test_maintenance_reports_failure_and_preserves_pending_intent();
        test_maintenance_expiry_write_failure_blocks_offer_until_retry();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "intent-age") {
        test_service_owned_intent_age_expires_future_wire();
        test_v2_migration_preserves_state_and_refuses_unaged_intent();
        test_missing_pending_storage_and_expired_file_fail_closed();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "reopen") {
        test_reopen_preserves_active_state();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "revoked") {
        test_revoked_identities_survive_new_exam_and_restart();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "corrupt") {
        test_corrupt_state_is_preserved_and_refused();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "orphan") {
        test_missing_state_refuses_orphan_batches();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "intent") {
        test_failed_enqueue_intent_recovers_after_restart();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "full-archive") {
        test_full_revocation_archive_is_reported();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "unbound-stop") {
        test_stop_before_first_enable_does_not_poison_restart();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "pending-drop") {
        test_new_packets_dropped_during_pending_failure_are_counted();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "wal-recovery") {
        test_pending_duplicate_conflict_and_expiry_recovery();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "loss-expiry") {
        test_coordinator_loss_counters_expire_after_thirty_days();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "loss-expiry-packet") {
        test_first_unauthorized_packet_after_expiry_is_counted();
        return 0;
    }
    test_producer_and_sequence_assignment();
    test_unauthorized_samples_are_separate_from_loss();
    test_lifecycle_and_reconnect_semantics();
    test_stale_context_and_restart();
    test_reopen_preserves_active_state();
    test_revoked_identities_survive_new_exam_and_restart();
    test_corrupt_state_is_preserved_and_refused();
    test_missing_state_refuses_orphan_batches();
    test_failed_enqueue_intent_recovers_after_restart();
    test_full_revocation_archive_is_reported();
    test_stop_before_first_enable_does_not_poison_restart();
    test_new_packets_dropped_during_pending_failure_are_counted();
    test_full_outbox_counts_every_sample_and_saturates();
    test_stop_storage_failures_block_restart_and_retry_cleanup();
    test_pending_duplicate_conflict_and_expiry_recovery();
    test_coordinator_loss_counters_expire_after_thirty_days();
    test_first_unauthorized_packet_after_expiry_is_counted();
    test_future_saved_loss_timestamp_cannot_extend_retention();
    test_state_write_failure_blocks_mutations_until_recovery();
    test_ack_variants_and_shutdown_retention();
    test_idle_provider_expires_suspended_batches_and_disabled_loss();
    test_pending_future_wire_cannot_renew_an_expired_arrival();
    test_maintenance_reports_failure_and_preserves_pending_intent();
    test_service_owned_intent_age_expires_future_wire();
    test_v2_migration_preserves_state_and_refuses_unaged_intent();
    test_missing_pending_storage_and_expired_file_fail_closed();
    test_maintenance_expiry_write_failure_blocks_offer_until_retry();
    test_expired_intent_state_failure_blocks_fresh_restart_after_rollback();
    test_expired_intent_marker_cleanup_keeps_identity_revoked();
    test_pending_expiry_is_observed_before_a_dirty_state_write();
    test_pristine_final_state_without_outbox();
    test_pristine_numeric_state_temporary_without_final_stores();
    test_stop_survives_state_and_marker_temp_failures();
    test_stop_repair_preserves_active_empty_recovery();
    test_stop_repair_keeps_existing_refusal_markers();
    run_stop_outcome_tests();
    std::cout << "All exam_telemetry_service_tests passed successfully.\n";
    return 0;
}
