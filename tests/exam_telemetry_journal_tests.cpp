#include "nstu/exam_telemetry_journal.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::uint64_t g_test_counter = 0;

std::filesystem::path temp_test_dir(const std::string& tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("nstu_journal_test_" + tag + "_" +
                      std::to_string(++g_test_counter) + "_" +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

nstu::exam::SessionId make_session(std::uint8_t byte_val) {
    nstu::exam::SessionId s{};
    s.fill(static_cast<std::byte>(byte_val));
    return s;
}

nstu::exam::ProducerId make_producer(std::uint8_t byte_val) {
    nstu::exam::ProducerId p{};
    p.fill(static_cast<std::byte>(byte_val));
    return p;
}

nstu::exam::TelemetrySample make_sample(std::uint64_t collector_id,
                                        const std::string& question_id = "q1",
                                        std::uint32_t count = 1) {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_first_view;
    sample.source = nstu::exam::TelemetrySource::native;
    sample.collector_id = collector_id;
    sample.question_id = question_id;
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 1000;
    sample.duration_milliseconds = 0;
    sample.client_time_unix_milliseconds = 1700000000000ULL;
    sample.numeric_value = 0;
    sample.count = count;
    sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}

nstu::exam::TelemetryBatch make_batch(
    const nstu::exam::SessionId& session_id,
    const nstu::exam::ProducerId& producer_id,
    std::uint64_t sequence,
    std::uint64_t client_enqueue_time = 1700000000000ULL,
    const std::string& sample_tag = "q1") {
    nstu::exam::TelemetryBatch batch;
    batch.package_id = "test-pkg";
    batch.package_digest = nstu::security::Sha256Digest{std::byte{0xaa}};
    batch.client_id = nstu::security::ClientId{std::byte{0xbb}};
    batch.session_id = session_id;
    batch.candidate_id = "candidate-1";
    batch.producer_id = producer_id;
    batch.sequence = sequence;
    batch.first_enqueued_at_unix_milliseconds = client_enqueue_time;
    batch.samples.push_back(make_sample(sequence, sample_tag));
    return batch;
}

void test_initial_state_and_open_close() {
    std::cout << "Running test_initial_state_and_open_close...\n";
    const auto dir = temp_test_dir("open_close");
    nstu::server::ExamTelemetryJournal journal;
    assert(!journal.is_open());
    assert(journal.active_context_count() == 0);
    assert(journal.tracked_hash_count() == 0);
    assert(journal.detail_bytes() == 0);

    std::string error;
    assert(journal.open(dir, 1000000ULL, &error));
    assert(journal.is_open());
    assert(journal.active_context_count() == 0);

    journal.close();
    assert(!journal.is_open());
    std::filesystem::remove_all(dir);
}

void test_authorization_lifecycle() {
    std::cout << "Running test_authorization_lifecycle...\n";
    const auto dir = temp_test_dir("auth_lifecycle");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x01);
    const auto s2 = make_session(0x02);

    assert(!journal.is_context_authorized(s1, 1000000ULL));
    assert(journal.authorize_context(s1, 1000000ULL));
    assert(journal.is_context_authorized(s1, 1000000ULL));
    assert(journal.is_context_authorized(s1, 1000000ULL + 3600ULL * 1000ULL)); // +1h

    // 25-hour limit
    const auto max_active_ms = nstu::server::kTelemetryMaxActiveContextMilliseconds;
    assert(journal.is_context_authorized(s1, 1000000ULL + max_active_ms - 1));
    assert(!journal.is_context_authorized(s1, 1000000ULL + max_active_ms)); // expired past 25h

    // Explicit stop
    assert(journal.authorize_context(s2, 2000000ULL));
    assert(journal.is_context_authorized(s2, 2000000ULL));
    assert(journal.stop_authorization(s2));
    assert(!journal.is_context_authorized(s2, 2000000ULL));

    // Remove context
    assert(journal.remove_context(s2));
    assert(!journal.is_context_authorized(s2, 2000000ULL));

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_first_batch_sequence_one_requirement() {
    std::cout << "Running test_first_batch_sequence_one_requirement...\n";
    const auto dir = temp_test_dir("seq_one");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x10);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p_invalid = make_producer(0x20);
    // Unseen producer attempting sequence 2 -> must be rejected
    auto batch_bad = make_batch(s1, p_invalid, 2);
    auto ack_bad = journal.record_batch(batch_bad, 1000000ULL);
    assert(ack_bad.status == nstu::exam::TelemetryAckStatus::rejected);
    assert(!journal.is_producer_admitted(s1, p_invalid));

    // Unseen producer attempting sequence 0 -> rejected
    auto batch_zero = make_batch(s1, p_invalid, 0);
    auto ack_zero = journal.record_batch(batch_zero, 1000000ULL);
    assert(ack_zero.status == nstu::exam::TelemetryAckStatus::rejected);
    assert(!journal.is_producer_admitted(s1, p_invalid));

    // Unseen producer attempting sequence 1 -> accepted!
    const auto p_valid = make_producer(0x21);
    auto batch_good = make_batch(s1, p_valid, 1);
    auto ack_good = journal.record_batch(batch_good, 1000000ULL);
    assert(ack_good.status == nstu::exam::TelemetryAckStatus::accepted);
    assert(journal.is_producer_admitted(s1, p_valid));
    assert(journal.producer_generation(s1, p_valid) == 1);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_sequence_advancement_and_gaps() {
    std::cout << "Running test_sequence_advancement_and_gaps...\n";
    const auto dir = temp_test_dir("adv_gaps");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x11);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x31);
    // Seq 1 accepted
    auto ack1 = journal.record_batch(make_batch(s1, p1, 1), 1000000ULL);
    assert(ack1.status == nstu::exam::TelemetryAckStatus::accepted);

    // Gap arrival: Seq 5 accepted
    auto ack5 = journal.record_batch(make_batch(s1, p1, 5), 1000005ULL);
    assert(ack5.status == nstu::exam::TelemetryAckStatus::accepted);

    // Later arrival of missing Seq 3 (within floor) -> accepted
    auto ack3 = journal.record_batch(make_batch(s1, p1, 3), 1000010ULL);
    assert(ack3.status == nstu::exam::TelemetryAckStatus::accepted);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_exact_duplicates_and_conflicts() {
    std::cout << "Running test_exact_duplicates_and_conflicts...\n";
    const auto dir = temp_test_dir("dup_conflict");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x12);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x32);
    const auto batch1 = make_batch(s1, p1, 1, 1700000000000ULL, "sample_A");
    auto ack1 = journal.record_batch(batch1, 1000000ULL);
    assert(ack1.status == nstu::exam::TelemetryAckStatus::accepted);
    const auto detail_bytes_initial = journal.detail_bytes();
    assert(detail_bytes_initial > 0);

    // Exact duplicate -> same sequence, same hash
    auto ack1_dup = journal.record_batch(batch1, 1000005ULL);
    assert(ack1_dup.status == nstu::exam::TelemetryAckStatus::duplicate);
    // Exact duplicate consumes zero extra detail storage
    assert(journal.detail_bytes() == detail_bytes_initial);

    // Conflict -> same sequence, mismatching hash (different question / payload)
    const auto batch1_conflict = make_batch(s1, p1, 1, 1700000000000ULL, "sample_CONFLICT_PAYLOAD");
    auto ack1_conflict = journal.record_batch(batch1_conflict, 1000010ULL);
    assert(ack1_conflict.status == nstu::exam::TelemetryAckStatus::conflict);
    assert(journal.detail_bytes() == detail_bytes_initial);

    // Conflict is terminal: repeating conflict still returns conflict
    auto ack1_conflict_repeat = journal.record_batch(batch1_conflict, 1000015ULL);
    assert(ack1_conflict_repeat.status == nstu::exam::TelemetryAckStatus::conflict);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_producer_retirement_and_generation_cap() {
    std::cout << "Running test_producer_retirement_and_generation_cap...\n";
    const auto dir = temp_test_dir("gen_cap");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x13);
    assert(journal.authorize_context(s1, 1000000ULL));

    // Register 16 consecutive producer generations
    for (std::uint8_t gen = 1; gen <= 16; ++gen) {
        const auto p = make_producer(gen);
        auto ack = journal.record_batch(make_batch(s1, p, 1), 1000000ULL + gen);
        assert(ack.status == nstu::exam::TelemetryAckStatus::accepted);
        assert(journal.is_producer_admitted(s1, p));
        assert(journal.producer_generation(s1, p) == gen);

        // Retire producer
        assert(journal.retire_producer(s1, p));
        assert(journal.is_producer_retired(s1, p));

        // Retired producer cannot send again
        auto ack_retired = journal.record_batch(make_batch(s1, p, 2), 1000100ULL);
        assert(ack_retired.status == nstu::exam::TelemetryAckStatus::rejected);
    }

    // 17th producer must be rejected due to 16-generation lifetime cap
    const auto p17 = make_producer(17);
    auto ack17 = journal.record_batch(make_batch(s1, p17, 1), 1000200ULL);
    assert(ack17.status == nstu::exam::TelemetryAckStatus::rejected);
    assert(!journal.is_producer_admitted(s1, p17));

    // Generation cap and retired status survive restart
    journal.close();
    assert(journal.open(dir, 1000300ULL));
    assert(journal.is_producer_retired(s1, make_producer(1)));
    assert(journal.is_producer_retired(s1, make_producer(16)));

    // Attempting 17th producer after restart must still be rejected
    auto ack17_reopen = journal.record_batch(make_batch(s1, p17, 1), 1000350ULL);
    assert(ack17_reopen.status == nstu::exam::TelemetryAckStatus::rejected);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_replay_floor_and_expiry() {
    std::cout << "Running test_replay_floor_and_expiry...\n";
    const auto dir = temp_test_dir("replay_floor");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x14);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x41);
    assert(journal.record_batch(make_batch(s1, p1, 1), 1000000ULL).status ==
           nstu::exam::TelemetryAckStatus::accepted);
    assert(journal.record_batch(make_batch(s1, p1, 2), 1000010ULL).status ==
           nstu::exam::TelemetryAckStatus::accepted);

    // Prune expired batches after 30 days
    const auto retention_ms = nstu::server::kTelemetryRetentionMilliseconds;
    const auto now_after_30_days = 1000000ULL + retention_ms + 1000ULL;
    const auto pruned_count = journal.prune_expired(now_after_30_days);
    assert(pruned_count >= 2);

    // Replay floor has advanced to 2
    assert(journal.producer_replay_floor(s1, p1) >= 2);

    // Replay after complete expiry while authorization remains active is rejected by floor
    auto replay_ack = journal.record_batch(make_batch(s1, p1, 1), now_after_30_days);
    assert(replay_ack.status == nstu::exam::TelemetryAckStatus::rejected);

    // Now stop authorization -> replay floor deleted
    assert(journal.stop_authorization(s1));
    assert(journal.producer_replay_floor(s1, p1) == 0);

    // Replay after stop rejected for inactive authorization
    auto ack_after_stop = journal.record_batch(make_batch(s1, p1, 1), now_after_30_days + 10);
    assert(ack_after_stop.status == nstu::exam::TelemetryAckStatus::rejected);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_crash_recovery_and_torn_tail() {
    std::cout << "Running test_crash_recovery_and_torn_tail...\n";
    const auto dir = temp_test_dir("crash_recovery");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x15);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x51);
    const auto b1 = make_batch(s1, p1, 1);
    assert(journal.record_batch(b1, 1000000ULL).status ==
           nstu::exam::TelemetryAckStatus::accepted);

    // Record conflict on sequence 2
    const auto b2 = make_batch(s1, p1, 2, 1000000ULL, "orig_seq2");
    assert(journal.record_batch(b2, 1000005ULL).status ==
           nstu::exam::TelemetryAckStatus::accepted);

    const auto b2_conflict = make_batch(s1, p1, 2, 1000000ULL, "conflict_seq2");
    assert(journal.record_batch(b2_conflict, 1000010ULL).status ==
           nstu::exam::TelemetryAckStatus::conflict);

    journal.close();

    // Corrupt segment file by appending a torn tail (incomplete header)
    const auto seg0 = dir / "segment_0.bin";
    if (std::filesystem::exists(seg0)) {
        std::ofstream out(seg0, std::ios::binary | std::ios::app);
        const char garbage[] = {0x4e, 0x54, 0x44, 0x31, 0x01, 0x00, 0x7f, 0x00}; // torn partial header
        out.write(garbage, sizeof(garbage));
    }

    // Reopen journal -> must recover cleanly from torn tail
    nstu::server::ExamTelemetryJournal recovered_journal;
    std::string err;
    assert(recovered_journal.open(dir, 1000020ULL, &err));

    // Duplicate check on b1 survives
    assert(recovered_journal.record_batch(b1, 1000025ULL).status ==
           nstu::exam::TelemetryAckStatus::duplicate);

    // Conflict on b2 survives
    assert(recovered_journal.record_batch(b2_conflict, 1000030ULL).status ==
           nstu::exam::TelemetryAckStatus::conflict);

    recovered_journal.close();
    std::filesystem::remove_all(dir);
}

void test_simulated_disk_failure() {
    std::cout << "Running test_simulated_disk_failure...\n";
    const auto dir = temp_test_dir("disk_failure");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x16);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x61);
    assert(journal.record_batch(make_batch(s1, p1, 1), 1000000ULL).status ==
           nstu::exam::TelemetryAckStatus::accepted);

    // Inject disk failure
    journal.set_disk_failure_injection(true);

    auto ack_fail = journal.record_batch(make_batch(s1, p1, 2), 1000010ULL);
    assert(ack_fail.status == nstu::exam::TelemetryAckStatus::unavailable);

    // Restore disk
    journal.set_disk_failure_injection(false);

    // Retry batch 2 -> succeeds and state was not corrupted
    auto ack_retry = journal.record_batch(make_batch(s1, p1, 2), 1000020ULL);
    assert(ack_retry.status == nstu::exam::TelemetryAckStatus::accepted);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_client_timestamp_untrusted_retention() {
    std::cout << "Running test_client_timestamp_untrusted_retention...\n";
    const auto dir = temp_test_dir("untrusted_ts");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x17);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x71);
    // Client reports timestamp in year 2099 (untrusted observation)
    const auto future_client_time = 4102444800000ULL;
    auto batch_future = make_batch(s1, p1, 1, future_client_time);

    // Server receives at 1000000ULL
    auto ack = journal.record_batch(batch_future, 1000000ULL);
    assert(ack.status == nstu::exam::TelemetryAckStatus::accepted);

    // 30 days later based on server receive time:
    const auto retention_ms = nstu::server::kTelemetryRetentionMilliseconds;
    const auto pruned = journal.prune_expired(1000000ULL + retention_ms + 10ULL);
    assert(pruned == 1); // Client future timestamp did NOT extend 30-day server retention bound!

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_producer_hash_quota_and_eviction() {
    std::cout << "Running test_producer_hash_quota_and_eviction...\n";
    const auto dir = temp_test_dir("quota_evict");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x18);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x81);
    // Send 4100 batches (exceeding 4096 per-producer quota)
    for (std::uint64_t seq = 1; seq <= 4100; ++seq) {
        auto ack = journal.record_batch(make_batch(s1, p1, seq), 1000000ULL + seq);
        assert(ack.status == nstu::exam::TelemetryAckStatus::accepted);
    }

    // Hashes per producer quota is 4096. 4 were evicted -> replay floor >= 4
    assert(journal.producer_replay_floor(s1, p1) >= 4);

    // Evicted sequences must now be rejected by replay floor
    auto ack_evicted = journal.record_batch(make_batch(s1, p1, 1), 1005000ULL);
    assert(ack_evicted.status == nstu::exam::TelemetryAckStatus::rejected);

    auto ack_evicted3 = journal.record_batch(make_batch(s1, p1, 3), 1005005ULL);
    assert(ack_evicted3.status == nstu::exam::TelemetryAckStatus::rejected);

    // Recent sequences (e.g. 4100) are still tracked in dedup index -> duplicate
    auto ack_dup = journal.record_batch(make_batch(s1, p1, 4100), 1005010ULL);
    assert(ack_dup.status == nstu::exam::TelemetryAckStatus::duplicate);

    journal.close();
    std::filesystem::remove_all(dir);
}

void test_segment_rotation() {
    std::cout << "Running test_segment_rotation...\n";
    const auto dir = temp_test_dir("rotation");
    nstu::server::ExamTelemetryJournal journal;
    assert(journal.open(dir, 1000000ULL));

    const auto s1 = make_session(0x19);
    assert(journal.authorize_context(s1, 1000000ULL));

    const auto p1 = make_producer(0x91);
    assert(journal.active_segment_index() == 0);

    // Set segment size to 512 bytes for fast test of full 8-segment rotation
    journal.set_segment_size_limit_for_testing(512);

    // Each batch record is ~200-300 bytes, so 2-3 batches will fill 512 bytes and rotate
    // Send 30 batches -> rotates through all 8 segments and wraps around
    for (std::uint64_t seq = 1; seq <= 30; ++seq) {
        auto ack = journal.record_batch(make_batch(s1, p1, seq), 1000000ULL + seq);
        assert(ack.status == nstu::exam::TelemetryAckStatus::accepted);
    }

    // Verify rotation occurred and stayed within 8 segments
    assert(journal.segment_count() == 8);
    // After 30 batches with 512B limit, active_segment has rotated multiple times
    assert(journal.active_segment_index() < 8);
    assert(journal.detail_bytes() > 0);

    journal.close();
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    std::cout << "=== Running ExamTelemetryJournal Tests ===\n";
    test_initial_state_and_open_close();
    test_authorization_lifecycle();
    test_first_batch_sequence_one_requirement();
    test_sequence_advancement_and_gaps();
    test_exact_duplicates_and_conflicts();
    test_producer_retirement_and_generation_cap();
    test_replay_floor_and_expiry();
    test_crash_recovery_and_torn_tail();
    test_simulated_disk_failure();
    test_client_timestamp_untrusted_retention();
    test_producer_hash_quota_and_eviction();
    test_segment_rotation();
    std::cout << "All ExamTelemetryJournal tests passed successfully!\n";
    return 0;
}
